/*
  fileio.c - File i/o handler for zstd
  Copyright (C) Yann Collet 2013-2016

  GPL v2 License

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

  You can contact the author at :
  - zstd homepage : http://www.zstd.net
*/
/*
  Note : this is stand-alone program.
  It is not part of ZSTD compression library, it is a user program of ZSTD library.
  The license of ZSTD library is BSD.
  The license of this file is GPLv2.
*/

/* *************************************
*  Tuning options
***************************************/
#ifndef ZSTD_LEGACY_SUPPORT
/* LEGACY_SUPPORT :
*  decompressor can decode older formats (starting from Zstd 0.1+) */
#  define ZSTD_LEGACY_SUPPORT 1
#endif


/* *************************************
*  Compiler Options
***************************************/
#define _POSIX_SOURCE 1        /* enable %llu on Windows */


/*-*************************************
*  Includes
***************************************/
#include "util.h"       /* Compiler options, UTIL_GetFileSize */
#include <stdio.h>      /* fprintf, fopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>     /* malloc, free */
#include <string.h>     /* strcmp, strlen */
#include <time.h>       /* clock */
#include <errno.h>      /* errno */

#include "mem.h"
#include "fileio.h"
#define ZSTD_STATIC_LINKING_ONLY   /* ZSTD_magicNumber, ZSTD_frameHeaderSize_max */
#include "zstd.h"
#include "zstd_internal.h" /* MIN, KB, MB */
#define ZBUFF_STATIC_LINKING_ONLY
#include "zbuff.h"

#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT==1)
#  include "zstd_legacy.h"    /* ZSTD_isLegacy */
#  include "fileio_legacy.h"  /* FIO_decompressLegacyFrame */
#endif


/*-*************************************
*  OS-specific Includes
***************************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>    /* _O_BINARY */
#  include <io.h>       /* _setmode, _isatty */
#  define SET_BINARY_MODE(file) { if (_setmode(_fileno(file), _O_BINARY) == -1) perror("Cannot set _O_BINARY"); }
#else
#  include <unistd.h>   /* isatty */
#  define SET_BINARY_MODE(file)
#endif


/*-*************************************
*  Constants
***************************************/
#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _6BITS 0x3F
#define _8BITS 0xFF

#define BLOCKSIZE      (128 KB)
#define ROLLBUFFERSIZE (BLOCKSIZE*8*64)

#define FIO_FRAMEHEADERSIZE  5        /* as a define, because needed to allocated table on stack */
#define FSE_CHECKSUM_SEED    0

#define CACHELINE 64

#define MAX_DICT_SIZE (8 MB)   /* protection against large input (attack scenario) */

#define FNSPACE 30


/*-*************************************
*  Macros
***************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static U32 g_displayLevel = 2;   /* 0 : no display;   1: errors;   2 : + result + interaction + warnings;   3 : + progression;   4 : + information */
void FIO_setNotificationLevel(unsigned level) { g_displayLevel=level; }

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((FIO_GetMilliSpan(g_time) > refreshRate) || (g_displayLevel>=4)) \
            { g_time = clock(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(stdout); } }
static const unsigned refreshRate = 150;
static clock_t g_time = 0;

static unsigned FIO_GetMilliSpan(clock_t nPrevious)
{
    clock_t const nCurrent = clock();
    return (unsigned)(((nCurrent - nPrevious) * 1000) / CLOCKS_PER_SEC);
}


/*-*************************************
*  Local Parameters
***************************************/
static U32 g_overwrite = 0;
void FIO_overwriteMode(void) { g_overwrite=1; }
static U32 g_maxWLog = 23;
void FIO_setMaxWLog(unsigned maxWLog) { g_maxWLog = maxWLog; }
static U32 g_sparseFileSupport = 1;   /* 0 : no sparse allowed; 1: auto (file yes, stdout no); 2: force sparse */
void FIO_setSparseWrite(unsigned sparse) { g_sparseFileSupport=sparse; }
static U32 g_dictIDFlag = 1;
void FIO_setDictIDFlag(unsigned dictIDFlag) { g_dictIDFlag = dictIDFlag; }
static U32 g_checksumFlag = 0;
void FIO_setChecksumFlag(unsigned checksumFlag) { g_checksumFlag = checksumFlag; }
static U32 g_removeSrcFile = 0;
void FIO_setRemoveSrcFile(unsigned flag) { g_removeSrcFile = (flag>0); }


/*-*************************************
*  Exceptions
***************************************/
#ifndef DEBUG
#  define DEBUG 0
#endif
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/*-*************************************
*  Functions
***************************************/
static FILE* FIO_openSrcFile(const char* srcFileName)
{
    FILE* f;

    if (!strcmp (srcFileName, stdinmark)) {
        DISPLAYLEVEL(4,"Using stdin for input\n");
        f = stdin;
        SET_BINARY_MODE(stdin);
    } else {
        f = fopen(srcFileName, "rb");
    }

    if ( f==NULL ) DISPLAYLEVEL(1, "zstd: %s: No such file\n", srcFileName);

    return f;
}


static FILE* FIO_openDstFile(const char* dstFileName)
{
    FILE* f;

    if (!strcmp (dstFileName, stdoutmark)) {
        DISPLAYLEVEL(4,"Using stdout for output\n");
        f = stdout;
        SET_BINARY_MODE(stdout);
        if (g_sparseFileSupport==1) {
            g_sparseFileSupport = 0;
            DISPLAYLEVEL(4, "Sparse File Support is automatically disabled on stdout ; try --sparse \n");
        }
    } else {
        if (!g_overwrite && strcmp (dstFileName, nulmark)) {  /* Check if destination file already exists */
            f = fopen( dstFileName, "rb" );
            if (f != 0) {  /* dest file exists, prompt for overwrite authorization */
                fclose(f);
                if (g_displayLevel <= 1) {
                    /* No interaction possible */
                    DISPLAY("zstd: %s already exists; not overwritten  \n", dstFileName);
                    return 0;
                }
                DISPLAY("zstd: %s already exists; do you wish to overwrite (y/N) ? ", dstFileName);
                {   int ch = getchar();
                    if ((ch!='Y') && (ch!='y')) {
                        DISPLAY("    not overwritten  \n");
                        return 0;
                    }
                    while ((ch!=EOF) && (ch!='\n')) ch = getchar();  /* flush rest of input line */
        }   }   }
        f = fopen( dstFileName, "wb" );
    }
    return f;
}


/*! FIO_loadFile() :
*   creates a buffer, pointed by `*bufferPtr`,
*   loads `filename` content into it,
*   up to MAX_DICT_SIZE bytes.
*   @return : loaded size
*/
static size_t FIO_loadFile(void** bufferPtr, const char* fileName)
{
    FILE* fileHandle;
    U64 fileSize;

    *bufferPtr = NULL;
    if (fileName == NULL) return 0;

    DISPLAYLEVEL(4,"Loading %s as dictionary \n", fileName);
    fileHandle = fopen(fileName, "rb");
    if (fileHandle==0) EXM_THROW(31, "Error opening file %s", fileName);
    fileSize = UTIL_getFileSize(fileName);
    if (fileSize > MAX_DICT_SIZE) {
        int seekResult;
        if (fileSize > 1 GB) EXM_THROW(32, "Dictionary file %s is too large", fileName);   /* avoid extreme cases */
        DISPLAYLEVEL(2,"Dictionary %s is too large : using last %u bytes only \n", fileName, MAX_DICT_SIZE);
        seekResult = fseek(fileHandle, (long int)(fileSize-MAX_DICT_SIZE), SEEK_SET);   /* use end of file */
        if (seekResult != 0) EXM_THROW(33, "Error seeking into file %s", fileName);
        fileSize = MAX_DICT_SIZE;
    }
    *bufferPtr = (BYTE*)malloc((size_t)fileSize);
    if (*bufferPtr==NULL) EXM_THROW(34, "Allocation error : not enough memory for dictBuffer");
    { size_t const readSize = fread(*bufferPtr, 1, (size_t)fileSize, fileHandle);
      if (readSize!=fileSize) EXM_THROW(35, "Error reading dictionary file %s", fileName); }
    fclose(fileHandle);
    return (size_t)fileSize;
}

#ifndef ZSTD_NOCOMPRESS

/*-**********************************************************************
*  Compression
************************************************************************/
typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    void*  dictBuffer;
    size_t dictBufferSize;
    ZBUFF_CCtx* ctx;
    FILE* dstFile;
    FILE* srcFile;
} cRess_t;

static cRess_t FIO_createCResources(const char* dictFileName)
{
    cRess_t ress;

    ress.ctx = ZBUFF_createCCtx();
    if (ress.ctx == NULL) EXM_THROW(30, "Allocation error : can't create ZBUFF context");

    /* Allocate Memory */
    ress.srcBufferSize = ZBUFF_recommendedCInSize();
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = ZBUFF_recommendedCOutSize();
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(31, "Allocation error : not enough memory");

    /* dictionary */
    ress.dictBufferSize = FIO_loadFile(&(ress.dictBuffer), dictFileName);

    return ress;
}

static void FIO_freeCResources(cRess_t ress)
{
    size_t errorCode;
    free(ress.srcBuffer);
    free(ress.dstBuffer);
    free(ress.dictBuffer);
    errorCode = ZBUFF_freeCCtx(ress.ctx);
    if (ZBUFF_isError(errorCode)) EXM_THROW(38, "Error : can't release ZBUFF context resource : %s", ZBUFF_getErrorName(errorCode));
}


/*! FIO_compressFilename_internal() :
 *  same as FIO_compressFilename_extRess(), with `ress.desFile` already opened.
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int FIO_compressFilename_internal(cRess_t ress,
                                         const char* dstFileName, const char* srcFileName,
                                         int cLevel)
{
    FILE* const srcFile = ress.srcFile;
    FILE* const dstFile = ress.dstFile;
    U64 readsize = 0;
    U64 compressedfilesize = 0;
    U64 const fileSize = UTIL_getFileSize(srcFileName);

    /* init */
    {   ZSTD_parameters params;
        memset(&params, 0, sizeof(params));
        params.cParams = ZSTD_getCParams(cLevel, fileSize, ress.dictBufferSize);
        params.fParams.contentSizeFlag = 1;
        params.fParams.checksumFlag = g_checksumFlag;
        params.fParams.noDictIDFlag = !g_dictIDFlag;
        if ((g_maxWLog) && (params.cParams.windowLog > g_maxWLog)) {
            params.cParams.windowLog = g_maxWLog;
            params.cParams = ZSTD_adjustCParams(params.cParams, fileSize, ress.dictBufferSize);
        }
        {   size_t const errorCode = ZBUFF_compressInit_advanced(ress.ctx, ress.dictBuffer, ress.dictBufferSize, params, fileSize);
            if (ZBUFF_isError(errorCode)) EXM_THROW(21, "Error initializing compression : %s", ZBUFF_getErrorName(errorCode));
    }   }

    /* Main compression loop */
    readsize = 0;
    while (1) {
        /* Fill input Buffer */
        size_t const inSize = fread(ress.srcBuffer, (size_t)1, ress.srcBufferSize, srcFile);
        if (inSize==0) break;
        readsize += inSize;
        DISPLAYUPDATE(2, "\rRead : %u MB  ", (U32)(readsize>>20));

        {   /* Compress using buffered streaming */
            size_t usedInSize = inSize;
            size_t cSize = ress.dstBufferSize;
            { size_t const result = ZBUFF_compressContinue(ress.ctx, ress.dstBuffer, &cSize, ress.srcBuffer, &usedInSize);
              if (ZBUFF_isError(result)) EXM_THROW(23, "Compression error : %s ", ZBUFF_getErrorName(result)); }
            if (inSize != usedInSize)
                /* inBuff should be entirely consumed since buffer sizes are recommended ones */
                EXM_THROW(24, "Compression error : input block not fully consumed");

            /* Write cBlock */
            { size_t const sizeCheck = fwrite(ress.dstBuffer, 1, cSize, dstFile);
              if (sizeCheck!=cSize) EXM_THROW(25, "Write error : cannot write compressed block into %s", dstFileName); }
            compressedfilesize += cSize;
        }
        DISPLAYUPDATE(2, "\rRead : %u MB  ==> %.2f%%   ", (U32)(readsize>>20), (double)compressedfilesize/readsize*100);
    }

    /* End of Frame */
    {   size_t cSize = ress.dstBufferSize;
        size_t const result = ZBUFF_compressEnd(ress.ctx, ress.dstBuffer, &cSize);
        if (result!=0) EXM_THROW(26, "Compression error : cannot create frame end");

        { size_t const sizeCheck = fwrite(ress.dstBuffer, 1, cSize, dstFile);
          if (sizeCheck!=cSize) EXM_THROW(27, "Write error : cannot write frame end into %s", dstFileName); }
        compressedfilesize += cSize;
    }

    /* Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2,"%-20.20s :%6.2f%%   (%6llu =>%6llu bytes, %s) \n", srcFileName,
        (double)compressedfilesize/readsize*100, (unsigned long long)readsize, (unsigned long long) compressedfilesize,
                 dstFileName);

    return 0;
}


/*! FIO_compressFilename_internal() :
 *  same as FIO_compressFilename_extRess(), with ress.destFile already opened (typically stdout)
 *  @return : 0 : compression completed correctly,
 *            1 : missing or pb opening srcFileName
 */
static int FIO_compressFilename_srcFile(cRess_t ress,
                                        const char* dstFileName, const char* srcFileName,
                                        int cLevel)
{
    int result;

    /* File check */
    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
        return 1;
    }
    ress.srcFile = FIO_openSrcFile(srcFileName);
    if (!ress.srcFile) return 1;   /* srcFile could not be opened */

    result = FIO_compressFilename_internal(ress, dstFileName, srcFileName, cLevel);

    fclose(ress.srcFile);
    if ((g_removeSrcFile) && (!result)) remove(srcFileName);
    return result;
}


/*! FIO_compressFilename_dstFile() :
 *  @return : 0 : compression completed correctly,
 *            1 : pb
 */
static int FIO_compressFilename_dstFile(cRess_t ress,
                                        const char* dstFileName, const char* srcFileName,
                                        int cLevel)
{
    int result;

    ress.dstFile = FIO_openDstFile(dstFileName);
    if (ress.dstFile==0) { fclose(ress.srcFile); return 1; }

    result = FIO_compressFilename_srcFile(ress, dstFileName, srcFileName, cLevel);

    if (fclose(ress.dstFile)) EXM_THROW(28, "Write error : cannot properly close %s", dstFileName);
    if (result!=0) remove(dstFileName);   /* remove operation artefact */
    return result;
}


int FIO_compressFilename(const char* dstFileName, const char* srcFileName,
                         const char* dictFileName, int compressionLevel)
{
    clock_t const start = clock();

    cRess_t const ress = FIO_createCResources(dictFileName);
    int const issueWithSrcFile = FIO_compressFilename_dstFile(ress, dstFileName, srcFileName, compressionLevel);
    FIO_freeCResources(ress);

    {   double const seconds = (double)(clock() - start) / CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Completed in %.2f sec \n", seconds);
    }
    return issueWithSrcFile;
}


int FIO_compressMultipleFilenames(const char** inFileNamesTable, unsigned nbFiles,
                                  const char* suffix,
                                  const char* dictFileName, int compressionLevel)
{
    int missed_files = 0;
    char*  dstFileName = (char*)malloc(FNSPACE);
    size_t dfnSize = FNSPACE;
    size_t const suffixSize = suffix ? strlen(suffix) : 0;
    cRess_t ress;

    /* init */
    ress = FIO_createCResources(dictFileName);

    /* loop on each file */
    if (!strcmp(suffix, stdoutmark)) {
        unsigned u;
        ress.dstFile = stdout;
        SET_BINARY_MODE(stdout);
        for (u=0; u<nbFiles; u++)
            missed_files += FIO_compressFilename_srcFile(ress, stdoutmark,
                                                         inFileNamesTable[u], compressionLevel);
        if (fclose(ress.dstFile)) EXM_THROW(29, "Write error : cannot properly close %s", stdoutmark);
    } else {
        unsigned u;
        for (u=0; u<nbFiles; u++) {
            size_t ifnSize = strlen(inFileNamesTable[u]);
            if (dfnSize <= ifnSize+suffixSize+1) { free(dstFileName); dfnSize = ifnSize + 20; dstFileName = (char*)malloc(dfnSize); }
            strcpy(dstFileName, inFileNamesTable[u]);
            strcat(dstFileName, suffix);
            missed_files += FIO_compressFilename_dstFile(ress, dstFileName,
                                                         inFileNamesTable[u], compressionLevel);
    }   }

    /* Close & Free */
    FIO_freeCResources(ress);
    free(dstFileName);

    return missed_files;
}

#endif /* #ifndef ZSTD_NOCOMPRESS */



#ifndef ZSTD_NODECOMPRESS

/* **************************************************************************
*  Decompression
****************************************************************************/
typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    void*  dictBuffer;
    size_t dictBufferSize;
    ZBUFF_DCtx* dctx;
    FILE*  dstFile;
} dRess_t;

static dRess_t FIO_createDResources(const char* dictFileName)
{
    dRess_t ress;

    /* init */
    ress.dctx = ZBUFF_createDCtx();
    if (ress.dctx==NULL) EXM_THROW(60, "Can't create ZBUFF decompression context");

    /* Allocate Memory */
    ress.srcBufferSize = ZBUFF_recommendedDInSize();
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = ZBUFF_recommendedDOutSize();
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(61, "Allocation error : not enough memory");

    /* dictionary */
    ress.dictBufferSize = FIO_loadFile(&(ress.dictBuffer), dictFileName);

    return ress;
}

static void FIO_freeDResources(dRess_t ress)
{
    size_t const errorCode = ZBUFF_freeDCtx(ress.dctx);
    if (ZBUFF_isError(errorCode)) EXM_THROW(69, "Error : can't free ZBUFF context resource : %s", ZBUFF_getErrorName(errorCode));
    free(ress.srcBuffer);
    free(ress.dstBuffer);
    free(ress.dictBuffer);
}


/** FIO_fwriteSparse() :
*   @return : storedSkips, to be provided to next call to FIO_fwriteSparse() of LZ4IO_fwriteSparseEnd() */
static unsigned FIO_fwriteSparse(FILE* file, const void* buffer, size_t bufferSize, unsigned storedSkips)
{
    const size_t* const bufferT = (const size_t*)buffer;   /* Buffer is supposed malloc'ed, hence aligned on size_t */
    size_t bufferSizeT = bufferSize / sizeof(size_t);
    const size_t* const bufferTEnd = bufferT + bufferSizeT;
    const size_t* ptrT = bufferT;
    static const size_t segmentSizeT = (32 KB) / sizeof(size_t);   /* 0-test re-attempted every 32 KB */

    if (!g_sparseFileSupport) {  /* normal write */
        size_t const sizeCheck = fwrite(buffer, 1, bufferSize, file);
        if (sizeCheck != bufferSize) EXM_THROW(70, "Write error : cannot write decoded block");
        return 0;
    }

    /* avoid int overflow */
    if (storedSkips > 1 GB) {
        int const seekResult = fseek(file, 1 GB, SEEK_CUR);
        if (seekResult != 0) EXM_THROW(71, "1 GB skip error (sparse file support)");
        storedSkips -= 1 GB;
    }

    while (ptrT < bufferTEnd) {
        size_t seg0SizeT = segmentSizeT;
        size_t nb0T;

        /* count leading zeros */
        if (seg0SizeT > bufferSizeT) seg0SizeT = bufferSizeT;
        bufferSizeT -= seg0SizeT;
        for (nb0T=0; (nb0T < seg0SizeT) && (ptrT[nb0T] == 0); nb0T++) ;
        storedSkips += (unsigned)(nb0T * sizeof(size_t));

        if (nb0T != seg0SizeT) {   /* not all 0s */
            int const seekResult = fseek(file, storedSkips, SEEK_CUR);
            if (seekResult) EXM_THROW(72, "Sparse skip error ; try --no-sparse");
            storedSkips = 0;
            seg0SizeT -= nb0T;
            ptrT += nb0T;
            {   size_t const sizeCheck = fwrite(ptrT, sizeof(size_t), seg0SizeT, file);
                if (sizeCheck != seg0SizeT) EXM_THROW(73, "Write error : cannot write decoded block");
        }   }
        ptrT += seg0SizeT;
    }

    {   static size_t const maskT = sizeof(size_t)-1;
        if (bufferSize & maskT) {   /* size not multiple of sizeof(size_t) : implies end of block */
            const char* const restStart = (const char*)bufferTEnd;
            const char* restPtr = restStart;
            size_t restSize =  bufferSize & maskT;
            const char* const restEnd = restStart + restSize;
            for ( ; (restPtr < restEnd) && (*restPtr == 0); restPtr++) ;
            storedSkips += (unsigned) (restPtr - restStart);
            if (restPtr != restEnd) {
                int seekResult = fseek(file, storedSkips, SEEK_CUR);
                if (seekResult) EXM_THROW(74, "Sparse skip error ; try --no-sparse");
                storedSkips = 0;
                {   size_t const sizeCheck = fwrite(restPtr, 1, restEnd - restPtr, file);
                    if (sizeCheck != (size_t)(restEnd - restPtr)) EXM_THROW(75, "Write error : cannot write decoded end of block");
    }   }   }   }

    return storedSkips;
}

static void FIO_fwriteSparseEnd(FILE* file, unsigned storedSkips)
{
    if (storedSkips-->0) {   /* implies g_sparseFileSupport>0 */
        int const seekResult = fseek(file, storedSkips, SEEK_CUR);
        if (seekResult != 0) EXM_THROW(69, "Final skip error (sparse file)\n");
        {   const char lastZeroByte[1] = { 0 };
            size_t const sizeCheck = fwrite(lastZeroByte, 1, 1, file);
            if (sizeCheck != 1) EXM_THROW(69, "Write error : cannot write last zero\n");
    }   }
}

/** FIO_decompressFrame() :
    @return : size of decoded frame
*/
unsigned long long FIO_decompressFrame(dRess_t ress,
                                       FILE* foutput, FILE* finput, size_t alreadyLoaded)
{
    U64    frameSize = 0;
    size_t readSize;
    U32 storedSkips = 0;

    ZBUFF_decompressInitDictionary(ress.dctx, ress.dictBuffer, ress.dictBufferSize);

    /* Header loading (optional, saves one loop) */
    {   size_t const toLoad = 9 - alreadyLoaded;   /* assumption : 9 >= alreadyLoaded */
        size_t const loadedSize = fread(((char*)ress.srcBuffer) + alreadyLoaded, 1, toLoad, finput);
        readSize = alreadyLoaded + loadedSize;
    }

    /* Main decompression Loop */
    while (1) {
        size_t inSize=readSize, decodedSize=ress.dstBufferSize;
        size_t const toRead = ZBUFF_decompressContinue(ress.dctx, ress.dstBuffer, &decodedSize, ress.srcBuffer, &inSize);
        if (ZBUFF_isError(toRead)) EXM_THROW(36, "Decoding error : %s", ZBUFF_getErrorName(toRead));
        readSize -= inSize;

        /* Write block */
        storedSkips = FIO_fwriteSparse(foutput, ress.dstBuffer, decodedSize, storedSkips);
        frameSize += decodedSize;
        DISPLAYUPDATE(2, "\rDecoded : %u MB...     ", (U32)(frameSize>>20) );

        if (toRead == 0) break;   /* end of frame */
        if (readSize) EXM_THROW(38, "Decoding error : should consume entire input");

        /* Fill input buffer */
        if (toRead > ress.srcBufferSize) EXM_THROW(34, "too large block");
        readSize = fread(ress.srcBuffer, 1, toRead, finput);
        if (readSize != toRead)
            EXM_THROW(35, "Read error");
    }

    FIO_fwriteSparseEnd(foutput, storedSkips);

    return frameSize;
}


/** FIO_passThrough() : just copy input into output, for compatibility with gzip -df mode
    @return : 0 (no error) */
static unsigned FIO_passThrough(FILE* foutput, FILE* finput, void* buffer, size_t bufferSize)
{
    size_t const blockSize = MIN (64 KB, bufferSize);
    size_t readFromInput = 1;
    unsigned storedSkips = 0;

    /* assumption : first 4 bytes already loaded (magic number detection), and stored within buffer */
    { size_t const sizeCheck = fwrite(buffer, 1, 4, foutput);
      if (sizeCheck != 4) EXM_THROW(50, "Pass-through write error"); }

    while (readFromInput) {
        readFromInput = fread(buffer, 1, blockSize, finput);
        storedSkips = FIO_fwriteSparse(foutput, buffer, readFromInput, storedSkips);
    }

    FIO_fwriteSparseEnd(foutput, storedSkips);
    return 0;
}


/** FIO_decompressSrcFile() :
    Decompression `srcFileName` into `ress.dstFile`
    @return : 0 : OK
              1 : operation not started
*/
static int FIO_decompressSrcFile(dRess_t ress, const char* srcFileName)
{
    unsigned long long filesize = 0;
    FILE* const dstFile = ress.dstFile;
    FILE* srcFile;

    if (UTIL_isDirectory(srcFileName)) {
        DISPLAYLEVEL(1, "zstd: %s is a directory -- ignored \n", srcFileName);
        return 1;
    }
    srcFile = FIO_openSrcFile(srcFileName);
    if (srcFile==0) return 1;

    /* for each frame */
    for ( ; ; ) {
        /* check magic number -> version */
        size_t const toRead = 4;
        size_t const sizeCheck = fread(ress.srcBuffer, (size_t)1, toRead, srcFile);
        if (sizeCheck==0) break;   /* no more input */
        if (sizeCheck != toRead) EXM_THROW(31, "zstd: %s read error : cannot read header", srcFileName);
        {   U32 const magic = MEM_readLE32(ress.srcBuffer);
#if defined(ZSTD_LEGACY_SUPPORT) && (ZSTD_LEGACY_SUPPORT>=1)
            if (ZSTD_isLegacy(magic)) {
                filesize += FIO_decompressLegacyFrame(dstFile, srcFile, ress.dictBuffer, ress.dictBufferSize, magic);
                continue;
            }
#endif
            if (((magic & 0xFFFFFFF0U) != ZSTD_MAGIC_SKIPPABLE_START) && (magic != ZSTD_MAGICNUMBER)) {
                if (g_overwrite)   /* -df : pass-through mode */
                    return FIO_passThrough(dstFile, srcFile, ress.srcBuffer, ress.srcBufferSize);
                else {
                    DISPLAYLEVEL(1, "zstd: %s: not in zstd format \n", srcFileName);
                    return 1;
        }   }   }
        filesize += FIO_decompressFrame(ress, dstFile, srcFile, toRead);
    }

    /* Final Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "%-20.20s: %llu bytes \n", srcFileName, filesize);

    /* Close */
    fclose(srcFile);
    if (g_removeSrcFile) remove(srcFileName);
    return 0;
}


/** FIO_decompressFile_extRess() :
    decompress `srcFileName` into `dstFileName`
    @return : 0 : OK
              1 : operation aborted (src not available, dst already taken, etc.)
*/
static int FIO_decompressDstFile(dRess_t ress,
                                      const char* dstFileName, const char* srcFileName)
{
    int result;
    ress.dstFile = FIO_openDstFile(dstFileName);
    if (ress.dstFile==0) return 1;

    result = FIO_decompressSrcFile(ress, srcFileName);

    if (fclose(ress.dstFile)) EXM_THROW(38, "Write error : cannot properly close %s", dstFileName);
    if (result != 0) remove(dstFileName);
    return result;
}


int FIO_decompressFilename(const char* dstFileName, const char* srcFileName,
                           const char* dictFileName)
{
    int missingFiles = 0;
    dRess_t ress = FIO_createDResources(dictFileName);

    missingFiles += FIO_decompressDstFile(ress, dstFileName, srcFileName);

    FIO_freeDResources(ress);
    return missingFiles;
}


#define MAXSUFFIXSIZE 8
int FIO_decompressMultipleFilenames(const char** srcNamesTable, unsigned nbFiles,
                                    const char* suffix,
                                    const char* dictFileName)
{
    int skippedFiles = 0;
    int missingFiles = 0;
    dRess_t ress = FIO_createDResources(dictFileName);

    if (!strcmp(suffix, stdoutmark) || !strcmp(suffix, nulmark)) {
        unsigned u;
        ress.dstFile = FIO_openDstFile(suffix);
        if (ress.dstFile == 0) EXM_THROW(71, "cannot open %s", suffix);
        for (u=0; u<nbFiles; u++)
            missingFiles += FIO_decompressSrcFile(ress, srcNamesTable[u]);
        if (fclose(ress.dstFile)) EXM_THROW(39, "Write error : cannot properly close %s", stdoutmark);
    } else {
        size_t const suffixSize = suffix ? strlen(suffix) : 0;
        size_t dfnSize = FNSPACE;
        unsigned u;
        char* dstFileName = (char*)malloc(FNSPACE);
        if (dstFileName==NULL) EXM_THROW(70, "not enough memory for dstFileName");
        for (u=0; u<nbFiles; u++) {   /* create dstFileName */
            const char* const srcFileName = srcNamesTable[u];
            size_t const sfnSize = strlen(srcFileName);
            const char* const suffixPtr = srcFileName + sfnSize - suffixSize;
            if (dfnSize+suffixSize <= sfnSize+1) {
                free(dstFileName);
                dfnSize = sfnSize + 20;
                dstFileName = (char*)malloc(dfnSize);
                if (dstFileName==NULL) EXM_THROW(71, "not enough memory for dstFileName");
            }
            if (sfnSize <= suffixSize || strcmp(suffixPtr, suffix) != 0) {
                DISPLAYLEVEL(1, "zstd: %s: unknown suffix (%4s expected) -- ignored \n", srcFileName, suffix);
                skippedFiles++;
                continue;
            }
            memcpy(dstFileName, srcFileName, sfnSize - suffixSize);
            dstFileName[sfnSize-suffixSize] = '\0';

            missingFiles += FIO_decompressDstFile(ress, dstFileName, srcFileName);
        }
        free(dstFileName);
    }

    FIO_freeDResources(ress);
    return missingFiles + skippedFiles;
}

#endif /* #ifndef ZSTD_NODECOMPRESS */
