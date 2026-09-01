/*
  LZ6io.c - LZ6 File/Stream Interface
  Copyright (C) Yann Collet 2011-2015

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
  - LZ6 source repository : https://github.com/inikep/lz6
  - LZ6 public forum : https://groups.google.com/forum/#!forum/lz6c
*/
/*
  Note : this is stand-alone program.
  It is not part of LZ6 compression library, it is a user code of the LZ6 library.
  - The license of LZ6 library is BSD.
  - The license of xxHash library is BSD.
  - The license of this source file is GPLv2.
*/

/**************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define _CRT_SECURE_NO_WARNINGS
#  define _CRT_SECURE_NO_DEPRECATE     /* VS2005 */
#  pragma warning(disable : 4127)      /* disable: C4127: conditional expression is constant */
#endif

#define _LARGE_FILES           /* Large file support on 32-bits AIX */
#define _FILE_OFFSET_BITS 64   /* Large file support on 32-bits unix */


/******************************
*  OS-specific Includes
******************************/
#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(_WIN32)
#  if defined(_POSIX_C_SOURCE) || defined(_POSIX_SOURCE) || defined(_MSC_VER)
#    include <fcntl.h>   /* _O_BINARY */
#    include <io.h>      /* _setmode, _fileno, _get_osfhandle */
#    include <windows.h> /* DeviceIoControl, HANDLE, FSCTL_SET_SPARSE */
#    define SET_BINARY_MODE(file) { int unused=_setmode(_fileno(file), _O_BINARY); (void)unused; }
#    define SET_SPARSE_FILE_MODE(file) { DWORD dw; DeviceIoControl((HANDLE) _get_osfhandle(_fileno(file)), FSCTL_SET_SPARSE, 0, 0, 0, 0, &dw, 0); }
#  else
#    define _POSIX_SOURCE 1          /* enable %llu with MinGW on Windows */ 
#    define SET_BINARY_MODE(file)
#    define SET_SPARSE_FILE_MODE(file)
#  endif
#else
#  define SET_BINARY_MODE(file)
#  define SET_SPARSE_FILE_MODE(file)
#endif


/*****************************
*  Includes
*****************************/
#include <stdio.h>     /* fprintf, fopen, fread, stdin, stdout, fflush, getchar */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strcmp, strlen */
#include <time.h>      /* clock */
#include <sys/types.h> /* stat64 */
#include <sys/stat.h>  /* stat64 */
#include <math.h>       /* log2 */
#include "lz6io.h"
#include "lz6.h"       /* still required for legacy format */
#include "lz6hc.h"     /* still required for legacy format */
#include "entropy/lz6seq.h"   /* legacy --seq envelope decoder */
#include "lz6frame.h"



#if !defined(S_ISREG)
#  define S_ISREG(x) (((x) & S_IFMT) == S_IFREG)
#endif


/*****************************
*  Constants
*****************************/
#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define MAGICNUMBER_SIZE    4
#define LZ6IO_MAGICNUMBER   0x184D2206U
#define LZ6IO_MAGICNUMBER_SEQ 0x184D2207U   /* seq-coded frame */
#define LZ6IO_MAGICNUMBER_LEGACY_SEQ 0x53365A4CU  /* "LZ6S" — legacy --seq envelope */
#define LZ6IO_SKIPPABLE0    0x184D2A50U
#define LZ6IO_SKIPPABLEMASK 0xFFFFFFF0U

#define CACHELINE 64
#define MIN_STREAM_BUFSIZE (192 KB)
#define LZ6IO_BLOCKSIZEID_DEFAULT 5

#define sizeT sizeof(size_t)
#define maskT (sizeT - 1)


/**************************************
*  Macros
**************************************/
#define DISPLAY(...)         fprintf(stderr, __VA_ARGS__)
#define DISPLAYLEVEL(l, ...) if (g_displayLevel>=l) { DISPLAY(__VA_ARGS__); }
static int g_displayLevel = 0;   /* 0 : no display  ; 1: errors  ; 2 : + result + interaction + warnings ; 3 : + progression; 4 : + information */

#define DISPLAYUPDATE(l, ...) if (g_displayLevel>=l) { \
            if ((LZ6IO_GetMilliSpan(g_time) > refreshRate) || (g_displayLevel>=4)) \
            { g_time = clock(); DISPLAY(__VA_ARGS__); \
            if (g_displayLevel>=4) fflush(stdout); } }
static const unsigned refreshRate = 150;
static clock_t g_time = 0;


/**************************************
*  Local Parameters
**************************************/
static int g_overwrite = 1;
static int g_blockSizeId = LZ6IO_BLOCKSIZEID_DEFAULT;
static int g_blockChecksum = 0;
static int g_streamChecksum = 1;
static int g_blockIndependence = 1;
static int g_sparseFileSupport = 1;
static int g_contentSizeFlag = 0;
static int g_blockCodec = 0;   /* 0 = LZ/HC frame (magic ...06), 1 = seq frame (magic ...07) */

static const int minBlockSizeID = 1;
static const int maxBlockSizeID = 7;


/**************************************
*  Exceptions
***************************************/
#define DEBUG 0
#define DEBUGOUTPUT(...) if (DEBUG) DISPLAY(__VA_ARGS__);
#define EXM_THROW(error, ...)                                             \
{                                                                         \
    DEBUGOUTPUT("Error defined at %s, line %i : \n", __FILE__, __LINE__); \
    DISPLAYLEVEL(1, "Error %i : ", error);                                \
    DISPLAYLEVEL(1, __VA_ARGS__);                                         \
    DISPLAYLEVEL(1, "\n");                                                \
    exit(error);                                                          \
}


/**************************************
*  Version modifiers
**************************************/
#define EXTENDED_ARGUMENTS
#define EXTENDED_HELP
#define EXTENDED_FORMAT
#define DEFAULT_DECOMPRESSOR LZ6IO_decompressLZ6F


/* ************************************************** */
/* ****************** Parameters ******************** */
/* ************************************************** */

/* Default setting : overwrite = 1; return : overwrite mode (0/1) */
int LZ6IO_setOverwrite(int yes)
{
   g_overwrite = (yes!=0);
   return g_overwrite;
}

/* blockSizeID : valid values : 1-7 */
int LZ6IO_setBlockSizeID(int bsid)
{
    static const int blockSizeTable[] = { 64 KB, 256 KB, 1 MB, 4 MB, 16 MB, 64 MB, 256 MB };
    if ((bsid < minBlockSizeID) || (bsid > maxBlockSizeID)) return -1;
    g_blockSizeId = bsid;
    return blockSizeTable[g_blockSizeId-minBlockSizeID];
}

/* blockCodec : 0 = LZ/HC frame, 1 = seq frame */
int LZ6IO_setBlockCodec(int codec)
{
    g_blockCodec = (codec != 0);
    return g_blockCodec;
}

int LZ6IO_setBlockMode(LZ6IO_blockMode_t blockMode)
{
    g_blockIndependence = (blockMode == LZ6IO_blockIndependent);
    return g_blockIndependence;
}

/* Default setting : no checksum */
int LZ6IO_setBlockChecksumMode(int xxhash)
{
    g_blockChecksum = (xxhash != 0);
    return g_blockChecksum;
}

/* Default setting : checksum enabled */
int LZ6IO_setStreamChecksumMode(int xxhash)
{
    g_streamChecksum = (xxhash != 0);
    return g_streamChecksum;
}

/* Default setting : 0 (no notification) */
int LZ6IO_setNotificationLevel(int level)
{
    g_displayLevel = level;
    return g_displayLevel;
}

/* Default setting : 0 (disabled) */
int LZ6IO_setSparseFile(int enable)
{
    g_sparseFileSupport = (enable!=0);
    return g_sparseFileSupport;
}

/* Default setting : 0 (disabled) */
int LZ6IO_setContentSize(int enable)
{
    g_contentSizeFlag = (enable!=0);
    return g_contentSizeFlag;
}

static unsigned LZ6IO_GetMilliSpan(clock_t nPrevious)
{
    clock_t nCurrent = clock();
    unsigned nSpan = (unsigned)(((nCurrent - nPrevious) * 1000) / CLOCKS_PER_SEC);
    return nSpan;
}

unsigned long long LZ6IO_GetFileSize(const char* infilename)
{
    int r;
#if defined(_MSC_VER)
    struct _stat64 statbuf;
    r = _stat64(infilename, &statbuf);
#else
    struct stat statbuf;
    r = stat(infilename, &statbuf);
#endif
    if (r || !S_ISREG(statbuf.st_mode)) return 0;   /* failure, or is not a regular file */
    return (unsigned long long)statbuf.st_size;
}


/* ************************************************************************ **
** ********************** LZ6 File / Pipe compression ********************* **
** ************************************************************************ */

static int LZ6IO_GetBlockSize_FromBlockId (int id) { /*printf("LZ6IO_GetBlockSize_FromBlockId %d=%d\n", id, (1 << (14 + (2 * id)))); */ return (1 << (14 + (2 * id))); }
static int LZ6IO_isSkippableMagicNumber(unsigned int magic) { return (magic & LZ6IO_SKIPPABLEMASK) == LZ6IO_SKIPPABLE0; }


static int LZ6IO_getFiles(const char* input_filename, const char* output_filename, FILE** pfinput, FILE** pfoutput)
{

    if (!strcmp (input_filename, stdinmark))
    {
        DISPLAYLEVEL(4,"Using stdin for input\n");
        *pfinput = stdin;
        SET_BINARY_MODE(stdin);
    }
    else
    {
        *pfinput = fopen(input_filename, "rb");
    }

    if ( *pfinput==0 )
    {
        DISPLAYLEVEL(1, "Unable to access file for processing: %s\n", input_filename);
        return 1;
    }

    if (!strcmp (output_filename, stdoutmark))
    {
        DISPLAYLEVEL(4,"Using stdout for output\n");
        *pfoutput = stdout;
        SET_BINARY_MODE(stdout);
        if (g_sparseFileSupport==1)
        {
            g_sparseFileSupport = 0;
            DISPLAYLEVEL(4, "Sparse File Support is automatically disabled on stdout ; try --sparse \n");
        }
    }
    else
    {
        /* Check if destination file already exists */
        *pfoutput=0;
        if (output_filename != nulmark) *pfoutput = fopen( output_filename, "rb" );
        if (*pfoutput!=0)
        {
            fclose(*pfoutput);
            if (!g_overwrite)
            {
                int ch = 'Y';
                DISPLAYLEVEL(2, "Warning : %s already exists\n", output_filename);
                if ((g_displayLevel <= 1) || (*pfinput == stdin))
                    EXM_THROW(11, "Operation aborted : %s already exists", output_filename);   /* No interaction possible */
                DISPLAYLEVEL(2, "Overwrite ? (Y/n) : ");
                while((ch = getchar()) != '\n' && ch != EOF)   /* flush integrated */
                if ((ch!='Y') && (ch!='y')) EXM_THROW(12, "No. Operation aborted : %s already exists", output_filename);
            }
        }
        *pfoutput = fopen( output_filename, "wb" );
    }

    if (*pfoutput==0) EXM_THROW(13, "Pb opening %s", output_filename);

    return 0;
}



/***************************************
*   Legacy Compression
***************************************/

/* unoptimized version; solves endianess & alignment issues */
static void LZ6IO_writeLE32 (void* p, unsigned value32)
{
    unsigned char* dstPtr = (unsigned char*)p;
    dstPtr[0] = (unsigned char)value32;
    dstPtr[1] = (unsigned char)(value32 >> 8);
    dstPtr[2] = (unsigned char)(value32 >> 16);
    dstPtr[3] = (unsigned char)(value32 >> 24);
}




/*********************************************
*  Compression using Frame format
*********************************************/

typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    LZ6F_compressionContext_t ctx;
} cRess_t;

static cRess_t LZ6IO_createCResources(void)
{
    const size_t blockSize = (size_t)LZ6IO_GetBlockSize_FromBlockId (g_blockSizeId);
    cRess_t ress;
    LZ6F_errorCode_t errorCode;

    errorCode = LZ6F_createCompressionContext(&(ress.ctx), LZ6F_VERSION);
    if (LZ6F_isError(errorCode)) EXM_THROW(30, "Allocation error : can't create LZ6F context : %s", LZ6F_getErrorName(errorCode));

    /* Allocate Memory */
    ress.srcBuffer = malloc(blockSize);
    ress.srcBufferSize = blockSize;
    ress.dstBufferSize = LZ6F_compressFrameBound(blockSize, NULL);   /* cover worst case */
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(31, "Allocation error : not enough memory");

    return ress;
}

static void LZ6IO_freeCResources(cRess_t ress)
{
    LZ6F_errorCode_t errorCode;
    free(ress.srcBuffer);
    free(ress.dstBuffer);
    errorCode = LZ6F_freeCompressionContext(ress.ctx);
    if (LZ6F_isError(errorCode)) EXM_THROW(38, "Error : can't free LZ6F context resource : %s", LZ6F_getErrorName(errorCode));
}

/*
 * LZ6IO_compressFilename_extRess()
 * result : 0 : compression completed correctly
 *          1 : missing or pb opening srcFileName
 */
static int LZ6IO_compressFilename_extRess(cRess_t ress, const char* srcFileName, const char* dstFileName, int compressionLevel)
{
    unsigned long long filesize = 0;
    unsigned long long compressedfilesize = 0;
    FILE* srcFile;
    FILE* dstFile;
    void* const srcBuffer = ress.srcBuffer;
    void* const dstBuffer = ress.dstBuffer;
    const size_t dstBufferSize = ress.dstBufferSize;
    const size_t blockSize = (size_t)LZ6IO_GetBlockSize_FromBlockId (g_blockSizeId);
    size_t sizeCheck, headerSize, readSize;
    LZ6F_compressionContext_t ctx = ress.ctx;   /* just a pointer */
    LZ6F_preferences_t prefs;


    /* Init */
    memset(&prefs, 0, sizeof(prefs));

    /* File check */
    if (LZ6IO_getFiles(srcFileName, dstFileName, &srcFile, &dstFile)) return 1;

    /* Set compression parameters */
    prefs.autoFlush = 1;
    prefs.compressionLevel = compressionLevel;
    prefs.frameInfo.blockMode = (LZ6F_blockMode_t)g_blockIndependence;
    prefs.frameInfo.blockSizeID = (LZ6F_blockSizeID_t)g_blockSizeId;
    prefs.frameInfo.contentChecksumFlag = (LZ6F_contentChecksum_t)g_streamChecksum;
    prefs.frameInfo.blockCodec = (LZ6F_blockCodec_t)g_blockCodec;
    if (g_contentSizeFlag)
    {
      unsigned long long fileSize = LZ6IO_GetFileSize(srcFileName);
      prefs.frameInfo.contentSize = fileSize;   /* == 0 if input == stdin */
      if (fileSize==0)
          DISPLAYLEVEL(3, "Warning : cannot determine uncompressed frame content size \n");
    }

    /* LZ6S3 seq frames: content-segmented blocks. Read small windows,
     * track the byte-entropy profile, and close a block (compressUpdate)
     * whenever the profile shifts — each segment then gets its own
     * literal mode / transform, like per-file coding inside one frame. */
    size_t const windowSize = (g_blockCodec == 1 && blockSize > 256 KB) ? 256 KB : blockSize;
    readSize  = fread(srcBuffer, (size_t)1, windowSize, srcFile);
    filesize += readSize;

    /* single-block file (LZ/HC frames only; seq always segments) */
    if (g_blockCodec != 1 && readSize < blockSize)
    {
        /* Compress in single pass */
        size_t cSize = LZ6F_compressFrame(dstBuffer, dstBufferSize, srcBuffer, readSize, &prefs);
        if (LZ6F_isError(cSize)) EXM_THROW(34, "Compression failed : %s", LZ6F_getErrorName(cSize));
        compressedfilesize += cSize;
        DISPLAYUPDATE(2, "\rRead : %u MB   ==> %.2f%%   ",
                      (unsigned)(filesize>>20), (double)compressedfilesize/(filesize+!filesize)*100);   /* avoid division by zero */

        /* Write Block */
        sizeCheck = fwrite(dstBuffer, 1, cSize, dstFile);
        if (sizeCheck!=cSize) EXM_THROW(35, "Write error : cannot write compressed block");
    }

    else

    /* multiple-blocks file */
    {
        /* Write Archive Header */
        headerSize = LZ6F_compressBegin(ctx, dstBuffer, dstBufferSize, &prefs);
        if (LZ6F_isError(headerSize)) EXM_THROW(32, "File header generation failed : %s", LZ6F_getErrorName(headerSize));
        sizeCheck = fwrite(dstBuffer, 1, headerSize, dstFile);
        if (sizeCheck!=headerSize) EXM_THROW(33, "Write error : cannot write header");
        compressedfilesize += headerSize;

        /* Main Loop */
        if (g_blockCodec == 1)
        {
            /* content-segmented: windows of windowSize bytes; a block is
             * closed when a window's byte-entropy diverges from the open
             * segment's profile (or the max block size is reached) */
            size_t const minSeg = (size_t)4 MB < (size_t)blockSize ? (size_t)4 MB : blockSize;
            unsigned char* const segBuf = (unsigned char*)srcBuffer;
            double threshold = 1.0;    /* H0 cut: |wH0 - sH0|. An H1
                                          (order-1) signal was tried and
                                          REJECTED: per-window H1 varies
                                          within mozilla as much as across
                                          file boundaries, so the spurious
                                          cuts cost more than the catches */
            if (getenv("LZ6_SEG_THRESHOLD")) threshold = atof(getenv("LZ6_SEG_THRESHOLD"));
            unsigned s_hist[256] = {0}, w_hist[256];
            double s_H = 0.0; int s_has = 0;
            size_t segLen = readSize, outSize;

            #define SEG_ENTROPY(hist, cnt, outH) do { \
                unsigned _h[256]; memcpy(_h, hist, sizeof(_h)); \
                double _H = 0; for (int b = 0; b < 256; b++) { \
                    if (!_h[b]) continue; \
                    double pr = (double)_h[b] / (double)(cnt); \
                    _H -= pr * log2(pr); } \
                outH = _H; } while (0)

            /* profile the open segment (the first window) */
            {
                unsigned hh[256] = {0};
                for (size_t i = 0; i < segLen; i++) hh[segBuf[i]]++;
                memcpy(s_hist, hh, sizeof(s_hist));
                SEG_ENTROPY(s_hist, segLen, s_H);
                s_has = 1;
            }

            while (segLen > 0)
            {
                /* read one more window into the open segment */
                size_t room = blockSize - segLen < windowSize ? blockSize - segLen : windowSize;
                if (room == 0) room = windowSize;   /* blockSize boundary handled below */
                if (room > 0) {
                    readSize = fread(segBuf + segLen, (size_t)1, room, srcFile);
                    filesize += readSize;
                } else readSize = 0;

                if (readSize > 0) {
                    memset(w_hist, 0, sizeof(w_hist));
                    for (size_t i = 0; i < readSize; i++) w_hist[segBuf[segLen + i]]++;
                    double w_H;
                    SEG_ENTROPY(w_hist, readSize, w_H);
                    int cut = 0;
                    if (s_has && segLen >= minSeg && fabs(w_H - s_H) >= threshold) cut = 1;

                    if (cut) {
                        /* close the open segment */
                        outSize = LZ6F_compressUpdate(ctx, dstBuffer, dstBufferSize, srcBuffer, segLen, NULL);
                        if (LZ6F_isError(outSize)) EXM_THROW(34, "Compression failed : %s", LZ6F_getErrorName(outSize));
                        compressedfilesize += outSize;
                        DISPLAYUPDATE(2, "\rRead : %u MB   ==> %.2f%%   ", (unsigned)(filesize>>20), (double)compressedfilesize/(filesize+!filesize)*100);
                        sizeCheck = fwrite(dstBuffer, 1, outSize, dstFile);
                        if (sizeCheck!=outSize) EXM_THROW(35, "Write error : cannot write compressed block");
                        /* reopen with the just-read window */
                        memmove(segBuf, segBuf + segLen, readSize);
                        memcpy(s_hist, w_hist, sizeof(s_hist));
                        s_H = w_H;
                        segLen = readSize;
                        s_has = 1;
                    } else {
                        /* merge the window into the open segment */
                        for (int b = 0; b < 256; b++) s_hist[b] += w_hist[b];
                        segLen += readSize;
                        SEG_ENTROPY(s_hist, segLen, s_H);
                    }
                    if (segLen >= blockSize) {
                        outSize = LZ6F_compressUpdate(ctx, dstBuffer, dstBufferSize, srcBuffer, segLen, NULL);
                        if (LZ6F_isError(outSize)) EXM_THROW(34, "Compression failed : %s", LZ6F_getErrorName(outSize));
                        compressedfilesize += outSize;
                        sizeCheck = fwrite(dstBuffer, 1, outSize, dstFile);
                        if (sizeCheck!=outSize) EXM_THROW(35, "Write error : cannot write compressed block");
                        segLen = 0;
                        memset(s_hist, 0, sizeof(s_hist));
                        s_H = 0.0; s_has = 0;
                    }
                }

                if (readSize == 0 && feof(srcFile)) {
                    /* EOF: flush the open segment as the final block */
                    if (segLen > 0) {
                        outSize = LZ6F_compressUpdate(ctx, dstBuffer, dstBufferSize, srcBuffer, segLen, NULL);
                        if (LZ6F_isError(outSize)) EXM_THROW(34, "Compression failed : %s", LZ6F_getErrorName(outSize));
                        compressedfilesize += outSize;
                        sizeCheck = fwrite(dstBuffer, 1, outSize, dstFile);
                        if (sizeCheck!=outSize) EXM_THROW(35, "Write error : cannot write compressed block");
                        segLen = 0;
                    }
                    break;
                }
            }
        }
        else
        while (readSize>0)
        {
            size_t outSize;

            /* Compress Block */
            outSize = LZ6F_compressUpdate(ctx, dstBuffer, dstBufferSize, srcBuffer, readSize, NULL);
            if (LZ6F_isError(outSize)) EXM_THROW(34, "Compression failed : %s", LZ6F_getErrorName(outSize));
            compressedfilesize += outSize;
            DISPLAYUPDATE(2, "\rRead : %u MB   ==> %.2f%%   ", (unsigned)(filesize>>20), (double)compressedfilesize/filesize*100);

            /* Write Block */
            sizeCheck = fwrite(dstBuffer, 1, outSize, dstFile);
            if (sizeCheck!=outSize) EXM_THROW(35, "Write error : cannot write compressed block");

            /* Read next block */
            readSize  = fread(srcBuffer, (size_t)1, (size_t)blockSize, srcFile);
            filesize += readSize;
        }

        /* End of Stream mark */
        headerSize = LZ6F_compressEnd(ctx, dstBuffer, dstBufferSize, NULL);
        if (LZ6F_isError(headerSize)) EXM_THROW(36, "End of file generation failed : %s", LZ6F_getErrorName(headerSize));

        sizeCheck = fwrite(dstBuffer, 1, headerSize, dstFile);
        if (sizeCheck!=headerSize) EXM_THROW(37, "Write error : cannot write end of stream");
        compressedfilesize += headerSize;
    }

    /* Release files */
    fclose (srcFile);
    fclose (dstFile);

    /* Final Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Compressed %llu bytes into %llu bytes ==> %.2f%%\n",
        filesize, compressedfilesize, (double)compressedfilesize/(filesize + !filesize)*100);   /* avoid division by zero */

    return 0;
}


int LZ6IO_compressFilename(const char* srcFileName, const char* dstFileName, int compressionLevel)
{
    clock_t start, end;
    cRess_t ress;
    int issueWithSrcFile = 0;

    /* Init */
    start = clock();
    ress = LZ6IO_createCResources();

    /* Compress File */
    issueWithSrcFile += LZ6IO_compressFilename_extRess(ress, srcFileName, dstFileName, compressionLevel);

    /* Free resources */
    LZ6IO_freeCResources(ress);

    /* Final Status */
    end = clock();
    {
        double seconds = (double)(end - start) / CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Completed in %.2f sec \n", seconds);
    }

    return issueWithSrcFile;
}


#define FNSPACE 30
int LZ6IO_compressMultipleFilenames(const char** inFileNamesTable, int ifntSize, const char* suffix, int compressionLevel)
{
    int i;
    int missed_files = 0;
    char* dstFileName = (char*)malloc(FNSPACE);
    size_t ofnSize = FNSPACE;
    const size_t suffixSize = strlen(suffix);
    cRess_t ress;

    /* init */
    ress = LZ6IO_createCResources();

    /* loop on each file */
    for (i=0; i<ifntSize; i++)
    {
        size_t ifnSize = strlen(inFileNamesTable[i]);
        if (ofnSize <= ifnSize+suffixSize+1) { free(dstFileName); ofnSize = ifnSize + 20; dstFileName = (char*)malloc(ofnSize); }
        strcpy(dstFileName, inFileNamesTable[i]);
        strcat(dstFileName, suffix);

        missed_files += LZ6IO_compressFilename_extRess(ress, inFileNamesTable[i], dstFileName, compressionLevel);
    }

    /* Close & Free */
    LZ6IO_freeCResources(ress);
    free(dstFileName);

    return missed_files;
}


/* ********************************************************************* */
/* ********************** LZ6 file-stream Decompression **************** */
/* ********************************************************************* */

static unsigned LZ6IO_readLE32 (const void* s)
{
    const unsigned char* srcPtr = (const unsigned char*)s;
    unsigned value32 = srcPtr[0];
    value32 += (srcPtr[1]<<8);
    value32 += (srcPtr[2]<<16);
    value32 += ((unsigned)srcPtr[3])<<24;
    return value32;
}

static unsigned LZ6IO_fwriteSparse(FILE* file, const void* buffer, size_t bufferSize, unsigned storedSkips)
{
    const size_t* const bufferT = (const size_t*)buffer;   /* Buffer is supposed malloc'ed, hence aligned on size_t */
    const size_t* ptrT = bufferT;
    size_t  bufferSizeT = bufferSize / sizeT;
    const size_t* const bufferTEnd = bufferT + bufferSizeT;
    static const size_t segmentSizeT = (32 KB) / sizeT;

    if (!g_sparseFileSupport)   /* normal write */
    {
        size_t sizeCheck = fwrite(buffer, 1, bufferSize, file);
        if (sizeCheck != bufferSize) EXM_THROW(70, "Write error : cannot write decoded block");
        return 0;
    }

    /* avoid int overflow */
    if (storedSkips > 1 GB)
    {
        int seekResult = fseek(file, 1 GB, SEEK_CUR);
        if (seekResult != 0) EXM_THROW(71, "1 GB skip error (sparse file support)");
        storedSkips -= 1 GB;
    }

    while (ptrT < bufferTEnd)
    {
        size_t seg0SizeT = segmentSizeT;
        size_t nb0T;
        int seekResult;

        /* count leading zeros */
        if (seg0SizeT > bufferSizeT) seg0SizeT = bufferSizeT;
        bufferSizeT -= seg0SizeT;
        for (nb0T=0; (nb0T < seg0SizeT) && (ptrT[nb0T] == 0); nb0T++) ;
        storedSkips += (unsigned)(nb0T * sizeT);

        if (nb0T != seg0SizeT)   /* not all 0s */
        {
            size_t sizeCheck;
            seekResult = fseek(file, storedSkips, SEEK_CUR);
            if (seekResult) EXM_THROW(72, "Sparse skip error ; try --no-sparse");
            storedSkips = 0;
            seg0SizeT -= nb0T;
            ptrT += nb0T;
            sizeCheck = fwrite(ptrT, sizeT, seg0SizeT, file);
            if (sizeCheck != seg0SizeT) EXM_THROW(73, "Write error : cannot write decoded block");
        }
        ptrT += seg0SizeT;
    }

    if (bufferSize & maskT)   /* size not multiple of sizeT : implies end of block */
    {
        const char* const restStart = (const char*)bufferTEnd;
        const char* restPtr = restStart;
        size_t  restSize =  bufferSize & maskT;
        const char* const restEnd = restStart + restSize;
        for (; (restPtr < restEnd) && (*restPtr == 0); restPtr++) ;
        storedSkips += (unsigned) (restPtr - restStart);
        if (restPtr != restEnd)
        {
            size_t sizeCheck;
            int seekResult = fseek(file, storedSkips, SEEK_CUR);
            if (seekResult) EXM_THROW(74, "Sparse skip error ; try --no-sparse");
            storedSkips = 0;
            sizeCheck = fwrite(restPtr, 1, restEnd - restPtr, file);
            if (sizeCheck != (size_t)(restEnd - restPtr)) EXM_THROW(75, "Write error : cannot write decoded end of block");
        }
    }

    return storedSkips;
}

static void LZ6IO_fwriteSparseEnd(FILE* file, unsigned storedSkips)
{
    char lastZeroByte[1] = { 0 };

    if (storedSkips>0)   /* implies g_sparseFileSupport */
    {
        int seekResult;
        size_t sizeCheck;
        storedSkips --;
        seekResult = fseek(file, storedSkips, SEEK_CUR);
        if (seekResult != 0) EXM_THROW(69, "Final skip error (sparse file)\n");
        sizeCheck = fwrite(lastZeroByte, 1, 1, file);
        if (sizeCheck != 1) EXM_THROW(69, "Write error : cannot write last zero\n");
    }
}



typedef struct {
    void*  srcBuffer;
    size_t srcBufferSize;
    void*  dstBuffer;
    size_t dstBufferSize;
    LZ6F_decompressionContext_t dCtx;
} dRess_t;

static const size_t LZ6IO_dBufferSize = 64 KB;

static dRess_t LZ6IO_createDResources(void)
{
    dRess_t ress;
    LZ6F_errorCode_t errorCode;

    /* init */
    errorCode = LZ6F_createDecompressionContext(&ress.dCtx, LZ6F_VERSION);
    if (LZ6F_isError(errorCode)) EXM_THROW(60, "Can't create LZ6F context : %s", LZ6F_getErrorName(errorCode));

    /* Allocate Memory */
    ress.srcBufferSize = LZ6IO_dBufferSize;
    ress.srcBuffer = malloc(ress.srcBufferSize);
    ress.dstBufferSize = LZ6IO_dBufferSize;
    ress.dstBuffer = malloc(ress.dstBufferSize);
    if (!ress.srcBuffer || !ress.dstBuffer) EXM_THROW(61, "Allocation error : not enough memory");

    return ress;
}

static void LZ6IO_freeDResources(dRess_t ress)
{
    LZ6F_errorCode_t errorCode = LZ6F_freeDecompressionContext(ress.dCtx);
    if (LZ6F_isError(errorCode)) EXM_THROW(69, "Error : can't free LZ6F context resource : %s", LZ6F_getErrorName(errorCode));
    free(ress.srcBuffer);
    free(ress.dstBuffer);
}


static unsigned long long LZ6IO_decompressLZ6F(dRess_t ress, FILE* srcFile, FILE* dstFile, unsigned magicNumber)
{
    unsigned long long filesize = 0;
    LZ6F_errorCode_t nextToLoad;
    unsigned storedSkips = 0;

    /* Init feed with magic number (already consumed from FILE*  sFile) */
    {
        size_t inSize = MAGICNUMBER_SIZE;
        size_t outSize= 0;
        LZ6IO_writeLE32(ress.srcBuffer, magicNumber);
        nextToLoad = LZ6F_decompress(ress.dCtx, ress.dstBuffer, &outSize, ress.srcBuffer, &inSize, NULL);
        if (LZ6F_isError(nextToLoad)) EXM_THROW(62, "Header error : %s", LZ6F_getErrorName(nextToLoad));
    }

    /* Main Loop */
    for (;nextToLoad;)
    {
        size_t readSize;
        size_t pos = 0;
        size_t decodedBytes = ress.dstBufferSize;

        /* Read input */
        if (nextToLoad > ress.srcBufferSize) nextToLoad = ress.srcBufferSize;
        readSize = fread(ress.srcBuffer, 1, nextToLoad, srcFile);
        if (!readSize)
            break;   /* empty file or stream */

        while ((pos < readSize) || (decodedBytes == ress.dstBufferSize))   /* still to read, or still to flush */
        {
            /* Decode Input (at least partially) */
            size_t remaining = readSize - pos;
            decodedBytes = ress.dstBufferSize;
            nextToLoad = LZ6F_decompress(ress.dCtx, ress.dstBuffer, &decodedBytes, (char*)(ress.srcBuffer)+pos, &remaining, NULL);
            if (LZ6F_isError(nextToLoad)) EXM_THROW(66, "Decompression error : %s", LZ6F_getErrorName(nextToLoad));
            pos += remaining;

            if (decodedBytes)
            {
                /* Write Block */
                filesize += decodedBytes;
                DISPLAYUPDATE(2, "\rDecompressed : %u MB  ", (unsigned)(filesize>>20));
                storedSkips = LZ6IO_fwriteSparse(dstFile, ress.dstBuffer, decodedBytes, storedSkips);
            }

            if (!nextToLoad) break;
        }
    }

    LZ6IO_fwriteSparseEnd(dstFile, storedSkips);

    if (nextToLoad!=0)
        EXM_THROW(67, "Unfinished stream");

    return filesize;
}


#define PTSIZE  (64 KB)
#define PTSIZET (PTSIZE / sizeof(size_t))
static unsigned long long LZ6IO_passThrough(FILE* finput, FILE* foutput, unsigned char MNstore[MAGICNUMBER_SIZE])
{
	size_t buffer[PTSIZET];
    size_t read = 1, sizeCheck;
    unsigned long long total = MAGICNUMBER_SIZE;
    unsigned storedSkips = 0;

    sizeCheck = fwrite(MNstore, 1, MAGICNUMBER_SIZE, foutput);
    if (sizeCheck != MAGICNUMBER_SIZE) EXM_THROW(50, "Pass-through write error");

    while (read)
    {
        read = fread(buffer, 1, PTSIZE, finput);
        total += read;
        storedSkips = LZ6IO_fwriteSparse(foutput, buffer, read, storedSkips);
    }

    LZ6IO_fwriteSparseEnd(foutput, storedSkips);
    return total;
}


#define ENDOFSTREAM ((unsigned long long)-1)
static unsigned g_magicRead = 0;

/* Legacy --seq envelope from pre frame-integration builds ("LZ6S1" magic):
 * per chunk u32le csize | u32le usize | payload, 8 zero bytes terminate.
 * decode-only compatibility shim — nothing produces this format anymore.
 * magic4 holds the 4 magic bytes already consumed from finput. */
#define LZ6IO_LEGACY_SEQ_CHUNK_MAX ((size_t)256 << 20)
static unsigned long long LZ6IO_decompressLegacySeq(FILE* finput, FILE* foutput, const unsigned char* magic4)
{
    unsigned char magic[5];
    unsigned long long filesize = 0;
    memcpy(magic, magic4, 4);
    {
        int c = fgetc(finput);
        if (c == EOF) EXM_THROW(40, "Unrecognized header : Magic Number unreadable");
        magic[4] = (unsigned char)c;
    }
    if (memcmp(magic, "LZ6S1", 5) != 0) EXM_THROW(44, "Unrecognized header : file cannot be decoded");
    DISPLAYLEVEL(4, "Detected legacy --seq envelope \n");

    for (;;)
    {
        unsigned char hdr[8];
        size_t csize, usize;
        unsigned char *in, *out;
        size_t dSize;

        if (fread(hdr, 1, 8, finput) != 8) EXM_THROW(44, "Unrecognized header : truncated legacy seq frame");
        csize = (size_t)hdr[0] | ((size_t)hdr[1] << 8) | ((size_t)hdr[2] << 16) | ((size_t)hdr[3] << 24);
        usize = (size_t)hdr[4] | ((size_t)hdr[5] << 8) | ((size_t)hdr[6] << 16) | ((size_t)hdr[7] << 24);
        if (csize == 0) break;   /* terminator */
        if (csize > LZ6IO_LEGACY_SEQ_CHUNK_MAX + (LZ6IO_LEGACY_SEQ_CHUNK_MAX >> 1) + 65536 || usize > LZ6IO_LEGACY_SEQ_CHUNK_MAX)
            EXM_THROW(44, "Corrupt legacy seq frame header");
        in = (unsigned char*)malloc(csize ? csize : 1);
        out = (unsigned char*)malloc(usize ? usize + 65536 : 1);
        if (!in || !out) EXM_THROW(45, "Allocation error : not enough memory");
        if (fread(in, 1, csize, finput) != csize) { free(in); free(out); EXM_THROW(44, "Unrecognized header : truncated legacy seq chunk"); }
        dSize = LZ6_decompress_seq((const char*)in, csize, (char*)out, usize + 65536);
        if (!dSize || dSize != usize) { free(in); free(out); EXM_THROW(46, "Legacy seq decompression failed"); }
        if (fwrite(out, 1, dSize, foutput) != dSize) { free(in); free(out); EXM_THROW(47, "Write error : cannot write decoded block"); }
        filesize += dSize;
        free(in); free(out);
    }
    return filesize;
}

static unsigned long long selectDecoder(dRess_t ress, FILE* finput, FILE* foutput)
{
    unsigned char MNstore[MAGICNUMBER_SIZE];
    unsigned magicNumber, size;
    int errorNb;
    size_t nbReadBytes;
    static unsigned nbCalls = 0;

    /* init */
    nbCalls++;

    /* Check Archive Header */
    if (g_magicRead)
    {
      magicNumber = g_magicRead;
      g_magicRead = 0;
    }
    else
    {
      nbReadBytes = fread(MNstore, 1, MAGICNUMBER_SIZE, finput);
      if (nbReadBytes==0) return ENDOFSTREAM;                  /* EOF */
      if (nbReadBytes != MAGICNUMBER_SIZE) EXM_THROW(40, "Unrecognized header : Magic Number unreadable");
      magicNumber = LZ6IO_readLE32(MNstore);   /* Little Endian format */
    }
    if (LZ6IO_isSkippableMagicNumber(magicNumber)) magicNumber = LZ6IO_SKIPPABLE0;  /* fold skippable magic numbers */

    switch(magicNumber)
    {
    case LZ6IO_MAGICNUMBER:
    case LZ6IO_MAGICNUMBER_SEQ:
        return LZ6IO_decompressLZ6F(ress, finput, foutput, magicNumber);
    case LZ6IO_MAGICNUMBER_LEGACY_SEQ:
        return LZ6IO_decompressLegacySeq(finput, foutput, MNstore);
    case LZ6IO_SKIPPABLE0:
        DISPLAYLEVEL(4, "Skipping detected skippable area \n");
        nbReadBytes = fread(MNstore, 1, 4, finput);
        if (nbReadBytes != 4) EXM_THROW(42, "Stream error : skippable size unreadable");
        size = LZ6IO_readLE32(MNstore);     /* Little Endian format */
        errorNb = fseek(finput, size, SEEK_CUR);
        if (errorNb != 0) EXM_THROW(43, "Stream error : cannot skip skippable area");
        return selectDecoder(ress, finput, foutput);
    EXTENDED_FORMAT;
    default:
        if (nbCalls == 1)   /* just started */
        {
            if (g_overwrite)
                return LZ6IO_passThrough(finput, foutput, MNstore);
            EXM_THROW(44,"Unrecognized header : file cannot be decoded");   /* Wrong magic number at the beginning of 1st stream */
        }
        DISPLAYLEVEL(2, "Stream followed by unrecognized data\n");
        return ENDOFSTREAM;
    }
}


static int LZ6IO_decompressFile_extRess(dRess_t ress, const char* input_filename, const char* output_filename)
{
    unsigned long long filesize = 0, decodedSize=0;
    FILE* finput;
    FILE* foutput;


    /* Init */
    if (LZ6IO_getFiles(input_filename, output_filename, &finput, &foutput))
        return 1;

    /* sparse file */
    if (g_sparseFileSupport) { SET_SPARSE_FILE_MODE(foutput); }

    /* Loop over multiple streams */
    do
    {
        decodedSize = selectDecoder(ress, finput, foutput);
        if (decodedSize != ENDOFSTREAM)
            filesize += decodedSize;
    } while (decodedSize != ENDOFSTREAM);

    /* Final Status */
    DISPLAYLEVEL(2, "\r%79s\r", "");
    DISPLAYLEVEL(2, "Successfully decoded %llu bytes \n", filesize);

    /* Close */
    fclose(finput);
    fclose(foutput);

    return 0;
}


int LZ6IO_decompressFilename(const char* input_filename, const char* output_filename)
{
    dRess_t ress;
    clock_t start, end;
    int missingFiles = 0;

    start = clock();

    ress = LZ6IO_createDResources();
    missingFiles += LZ6IO_decompressFile_extRess(ress, input_filename, output_filename);
    LZ6IO_freeDResources(ress);

    end = clock();
    if (end==start) end=start+1;
    {
        double seconds = (double)(end - start)/CLOCKS_PER_SEC;
        DISPLAYLEVEL(4, "Done in %.2f sec  \n", seconds);
    }

    return missingFiles;
}


#define MAXSUFFIXSIZE 8
int LZ6IO_decompressMultipleFilenames(const char** inFileNamesTable, int ifntSize, const char* suffix)
{
    int i;
    int skippedFiles = 0;
    int missingFiles = 0;
    char* outFileName = (char*)malloc(FNSPACE);
    size_t ofnSize = FNSPACE;
    const size_t suffixSize = strlen(suffix);
    const char* suffixPtr;
    dRess_t ress;

	if (outFileName==NULL) exit(1);   /* not enough memory */
    ress = LZ6IO_createDResources();

    for (i=0; i<ifntSize; i++)
    {
        size_t ifnSize = strlen(inFileNamesTable[i]);
        suffixPtr = inFileNamesTable[i] + ifnSize - suffixSize;
        if (ofnSize <= ifnSize-suffixSize+1) { free(outFileName); ofnSize = ifnSize + 20; outFileName = (char*)malloc(ofnSize); if (outFileName==NULL) exit(1); }
        if (ifnSize <= suffixSize  ||  strcmp(suffixPtr, suffix) != 0)
        {
            DISPLAYLEVEL(1, "File extension doesn't match expected LZ6_EXTENSION (%4s); will not process file: %s\n", suffix, inFileNamesTable[i]);
            skippedFiles++;
            continue;
        }
        memcpy(outFileName, inFileNamesTable[i], ifnSize - suffixSize);
        outFileName[ifnSize-suffixSize] = '\0';

        missingFiles += LZ6IO_decompressFile_extRess(ress, inFileNamesTable[i], outFileName);
    }

    LZ6IO_freeDResources(ress);
    free(outFileName);
    return missingFiles + skippedFiles;
}
