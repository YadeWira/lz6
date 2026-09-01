/*
LZ6 auto-framing library
Copyright (C) 2011-2015, Yann Collet.

BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

* Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

You can contact the author at :
- LZ6 source repository : https://github.com/inikep/lz6
- LZ6 public forum : https://groups.google.com/forum/#!forum/lz6c
*/

/* LZ6F is a stand-alone API to create LZ6-compressed Frames
*  in full conformance with specification v1.5.0
*  All related operations, including memory management, are handled by the library.
* */


/**************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#endif


/**************************************
*  Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(s)   calloc(1,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy, memmove */
#include <stdio.h>
#define MEM_INIT       memset


/**************************************
*  Includes
**************************************/
#include "lz6frame_static.h"
#include "lz6.h"
#include "lz6hc.h"
#include "entropy/lz6seq.h"
#include "xxhash.h"


/**************************************
*  Basic Types
**************************************/
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
# include <stdint.h>
typedef  uint8_t BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef  int32_t S32;
typedef uint64_t U64;
#else
typedef unsigned char       BYTE;
typedef unsigned short      U16;
typedef unsigned int        U32;
typedef   signed int        S32;
typedef unsigned long long  U64;
#endif


/**************************************
*  Constants
**************************************/
#define KB *(1<<10)
#define MB *(1<<20)
#define GB *(1<<30)

#define _1BIT  0x01
#define _2BITS 0x03
#define _3BITS 0x07
#define _4BITS 0x0F
#define _8BITS 0xFF

#define LZ6F_MAGIC_SKIPPABLE_START 0x184D2A50U
#define LZ6F_MAGICNUMBER 0x184D2206U
#define LZ6F_BLOCKUNCOMPRESSED_FLAG 0x80000000U
#define LZ6F_BLOCKSEQ_FLAG 0x40000000U      /* block coded by the seq engine */
#define LZ6F_BLOCKSIZEID_DEFAULT LZ6F_max64KB
#define LZ6F_DICT_SIZE (1 << 22)

static const size_t minFHSize = 7;
static const size_t maxFHSize = 15;
static const size_t BHSize = 4;
static const int    minHClevel = 1;
#define LZ6F_CCTX_SEQ 3   /* lz6CtxLevel sentinel: seq engine (no LZ/HC stream) */


/**************************************
*  Structures and local types
**************************************/
typedef struct LZ6F_cctx_s
{
    LZ6F_preferences_t prefs;
    U32    version;
    U32    cStage;
    size_t maxBlockSize;
    size_t maxBufferSize;
    BYTE*  tmpBuff;
    BYTE*  tmpIn;
    size_t tmpInSize;
    U64    totalInSize;
    XXH32_state_t xxh;
    void*  lz6CtxPtr;
    U32    lz6CtxLevel;     /* 0: none;  1: LZ6_stream_t;  2: LZ6_streamHC_t;  3: seq engine */
    void*  lightStream;     /* LZ6_stream_t for the light-LZ fallback on seq frames */
} LZ6F_cctx_t;

typedef struct LZ6F_dctx_s
{
    LZ6F_frameInfo_t frameInfo;
    U32    version;
    U32    dStage;
    U64    frameRemainingSize;
    size_t maxBlockSize;
    size_t maxBufferSize;
    const BYTE* srcExpect;
    BYTE*  tmpIn;
    size_t tmpInSize;
    size_t tmpInTarget;
    BYTE*  tmpOutBuffer;
    const BYTE*  dict;
    size_t dictSize;
    BYTE*  tmpOut;
    size_t tmpOutSize;
    size_t tmpOutStart;
    XXH32_state_t xxh;
    BYTE   header[16];
    unsigned curBlockSeq;   /* current compressed block is seq-coded (bit30) */
} LZ6F_dctx_t;


/**************************************
*  Error management
**************************************/
#define LZ6F_GENERATE_STRING(STRING) #STRING,
static const char* LZ6F_errorStrings[] = { LZ6F_LIST_ERRORS(LZ6F_GENERATE_STRING) };


unsigned LZ6F_isError(LZ6F_errorCode_t code)
{
    return (code > (LZ6F_errorCode_t)(-LZ6F_ERROR_maxCode));
}

const char* LZ6F_getErrorName(LZ6F_errorCode_t code)
{
    static const char* codeError = "Unspecified error code";
    if (LZ6F_isError(code)) return LZ6F_errorStrings[-(int)(code)];
    return codeError;
}


/**************************************
*  Private functions
**************************************/
static size_t LZ6F_getBlockSize(unsigned blockSizeID)
{
    static const size_t blockSizes[7] = { 64 KB, 256 KB, 1 MB, 4 MB, 16 MB, 64 MB, 256 MB };

    if (blockSizeID == 0) blockSizeID = LZ6F_BLOCKSIZEID_DEFAULT;
    blockSizeID -= 1;
    if (blockSizeID >= 7) return (size_t)-LZ6F_ERROR_maxBlockSize_invalid;

  //  printf("LZ6F_getBlockSize %d %d\n", blockSizeID+1, (int)blockSizes[blockSizeID]);
    return blockSizes[blockSizeID];
}


/* unoptimized version; solves endianess & alignment issues */
static U32 LZ6F_readLE32 (const BYTE* srcPtr)
{
    U32 value32 = srcPtr[0];
    value32 += (srcPtr[1]<<8);
    value32 += (srcPtr[2]<<16);
    value32 += ((U32)srcPtr[3])<<24;
    return value32;
}

static void LZ6F_writeLE32 (BYTE* dstPtr, U32 value32)
{
    dstPtr[0] = (BYTE)value32;
    dstPtr[1] = (BYTE)(value32 >> 8);
    dstPtr[2] = (BYTE)(value32 >> 16);
    dstPtr[3] = (BYTE)(value32 >> 24);
}

static U64 LZ6F_readLE64 (const BYTE* srcPtr)
{
    U64 value64 = srcPtr[0];
    value64 += ((U64)srcPtr[1]<<8);
    value64 += ((U64)srcPtr[2]<<16);
    value64 += ((U64)srcPtr[3]<<24);
    value64 += ((U64)srcPtr[4]<<32);
    value64 += ((U64)srcPtr[5]<<40);
    value64 += ((U64)srcPtr[6]<<48);
    value64 += ((U64)srcPtr[7]<<56);
    return value64;
}

static void LZ6F_writeLE64 (BYTE* dstPtr, U64 value64)
{
    dstPtr[0] = (BYTE)value64;
    dstPtr[1] = (BYTE)(value64 >> 8);
    dstPtr[2] = (BYTE)(value64 >> 16);
    dstPtr[3] = (BYTE)(value64 >> 24);
    dstPtr[4] = (BYTE)(value64 >> 32);
    dstPtr[5] = (BYTE)(value64 >> 40);
    dstPtr[6] = (BYTE)(value64 >> 48);
    dstPtr[7] = (BYTE)(value64 >> 56);
}


static BYTE LZ6F_headerChecksum (const void* header, size_t length)
{
    U32 xxh = XXH32(header, length, 0);
    return (BYTE)(xxh >> 8);
}


/**************************************
*  Simple compression functions
**************************************/
static LZ6F_blockSizeID_t LZ6F_optimalBSID(const LZ6F_blockSizeID_t requestedBSID, const size_t srcSize)
{
    LZ6F_blockSizeID_t proposedBSID = LZ6F_max64KB;
    size_t maxBlockSize = 64 KB;
    while (requestedBSID > proposedBSID)
    {
        if (srcSize <= maxBlockSize)
            return proposedBSID;
        proposedBSID = (LZ6F_blockSizeID_t)((int)proposedBSID + 1);
        maxBlockSize <<= 2;
    }
    return requestedBSID;
}


void LZ6F_freeStream(LZ6F_cctx_t* cctxPtr)
{
    if (cctxPtr->lz6CtxLevel == 1)
        LZ6_freeStream((LZ6_stream_t*)cctxPtr->lz6CtxPtr);
    else if (cctxPtr->lz6CtxLevel == 2)
        LZ6_freeStreamHC((LZ6_streamHC_t*)cctxPtr->lz6CtxPtr);
    if (cctxPtr->lightStream) {
        LZ6_freeStream((LZ6_stream_t*)cctxPtr->lightStream);
        cctxPtr->lightStream = NULL;
    }
    cctxPtr->lz6CtxLevel = 0;
}


size_t LZ6F_compressFrameBound(size_t srcSize, const LZ6F_preferences_t* preferencesPtr)
{
    LZ6F_preferences_t prefs;
    size_t headerSize;
    size_t streamSize;

    if (preferencesPtr!=NULL) prefs = *preferencesPtr;
    else memset(&prefs, 0, sizeof(prefs));

    prefs.frameInfo.blockSizeID = LZ6F_optimalBSID(prefs.frameInfo.blockSizeID, srcSize);
    prefs.autoFlush = 1;

    headerSize = maxFHSize;      /* header size, including magic number and frame content size*/
    streamSize = LZ6F_compressBound(srcSize, &prefs);

    return headerSize + streamSize;
}


/* LZ6F_compressFrame()
* Compress an entire srcBuffer into a valid LZ6 frame, as defined by specification v1.5.0, in a single step.
* The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
* You can get the minimum value of dstMaxSize by using LZ6F_compressFrameBound()
* If this condition is not respected, LZ6F_compressFrame() will fail (result is an errorCode)
* The LZ6F_preferences_t structure is optional : you can provide NULL as argument. All preferences will then be set to default.
* The result of the function is the number of bytes written into dstBuffer.
* The function outputs an error code if it fails (can be tested using LZ6F_isError())
*/
size_t LZ6F_compressFrame(void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ6F_preferences_t* preferencesPtr)
{
    LZ6F_cctx_t cctxI;
    LZ6F_preferences_t prefs;
    LZ6F_compressOptions_t options;
    LZ6F_errorCode_t errorCode;
    BYTE* const dstStart = (BYTE*) dstBuffer;
    BYTE* dstPtr = dstStart;
    BYTE* const dstEnd = dstStart + dstMaxSize;

    memset(&cctxI, 0, sizeof(cctxI));   /* works because no allocation */
    memset(&options, 0, sizeof(options));

    cctxI.version = LZ6F_VERSION;
    cctxI.lz6CtxLevel = 0;        /* stack cctx: init the ctx fields freeStream reads */
    cctxI.lz6CtxPtr = NULL;
    cctxI.maxBufferSize = 5 MB;   /* mess with real buffer size to prevent allocation; works because autoflush==1 & stableSrc==1 */

    if (preferencesPtr!=NULL)
        prefs = *preferencesPtr;
    else
        memset(&prefs, 0, sizeof(prefs));
    if (prefs.frameInfo.contentSize != 0)
        prefs.frameInfo.contentSize = (U64)srcSize;   /* auto-correct content size if selected (!=0) */

    prefs.frameInfo.blockSizeID = LZ6F_optimalBSID(prefs.frameInfo.blockSizeID, srcSize);
    prefs.autoFlush = 1;
    if (srcSize <= LZ6F_getBlockSize(prefs.frameInfo.blockSizeID))
        prefs.frameInfo.blockMode = LZ6F_blockIndependent;   /* no need for linked blocks */

    options.stableSrc = 1;

    if (dstMaxSize < LZ6F_compressFrameBound(srcSize, &prefs))
        return (size_t)-LZ6F_ERROR_dstMaxSize_tooSmall;

    errorCode = LZ6F_compressBegin(&cctxI, dstBuffer, dstMaxSize, &prefs);  /* write header */
    if (LZ6F_isError(errorCode)) return errorCode;
    dstPtr += errorCode;   /* header size */

    errorCode = LZ6F_compressUpdate(&cctxI, dstPtr, dstEnd-dstPtr, srcBuffer, srcSize, &options);
    if (LZ6F_isError(errorCode)) return errorCode;
    dstPtr += errorCode;

    errorCode = LZ6F_compressEnd(&cctxI, dstPtr, dstEnd-dstPtr, &options);   /* flush last block, and generate suffix */
    if (LZ6F_isError(errorCode)) return errorCode;
    dstPtr += errorCode;

    LZ6F_freeStream(&cctxI);

    return (dstPtr - dstStart);
}


/***********************************
*  Advanced compression functions
***********************************/

/* LZ6F_createCompressionContext() :
* The first thing to do is to create a compressionContext object, which will be used in all compression operations.
* This is achieved using LZ6F_createCompressionContext(), which takes as argument a version and an LZ6F_preferences_t structure.
* The version provided MUST be LZ6F_VERSION. It is intended to track potential version differences between different binaries.
* The function will provide a pointer to an allocated LZ6F_compressionContext_t object.
* If the result LZ6F_errorCode_t is not OK_NoError, there was an error during context creation.
* Object can release its memory using LZ6F_freeCompressionContext();
*/
LZ6F_errorCode_t LZ6F_createCompressionContext(LZ6F_compressionContext_t* LZ6F_compressionContextPtr, unsigned version)
{
    LZ6F_cctx_t* cctxPtr;

    cctxPtr = (LZ6F_cctx_t*)ALLOCATOR(sizeof(LZ6F_cctx_t));
    if (cctxPtr==NULL) return (LZ6F_errorCode_t)(-LZ6F_ERROR_allocation_failed);

    cctxPtr->version = version;
    cctxPtr->cStage = 0;   /* Next stage : write header */

    *LZ6F_compressionContextPtr = (LZ6F_compressionContext_t)cctxPtr;

    return LZ6F_OK_NoError;
}


LZ6F_errorCode_t LZ6F_freeCompressionContext(LZ6F_compressionContext_t LZ6F_compressionContext)
{
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)LZ6F_compressionContext;

    if (cctxPtr != NULL)   /* null pointers can be safely provided to this function, like free() */
    {
        LZ6F_freeStream(cctxPtr);
        FREEMEM(cctxPtr->tmpBuff);
        FREEMEM(LZ6F_compressionContext);
    }

    return LZ6F_OK_NoError;
}


/* LZ6F_compressBegin() :
* will write the frame header into dstBuffer.
* dstBuffer must be large enough to accommodate a header (dstMaxSize). Maximum header size is LZ6F_MAXHEADERFRAME_SIZE bytes.
* The result of the function is the number of bytes written into dstBuffer for the header
* or an error code (can be tested using LZ6F_isError())
*/
size_t LZ6F_compressBegin(LZ6F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const LZ6F_preferences_t* preferencesPtr)
{
    LZ6F_preferences_t prefNull;
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)compressionContext;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    BYTE* headerStart;
    size_t requiredBuffSize;

    if (dstMaxSize < maxFHSize) return (size_t)-LZ6F_ERROR_dstMaxSize_tooSmall;
    if (cctxPtr->cStage != 0) return (size_t)-LZ6F_ERROR_GENERIC;
    memset(&prefNull, 0, sizeof(prefNull));
    if (preferencesPtr == NULL) preferencesPtr = &prefNull;
    cctxPtr->prefs = *preferencesPtr;
    cctxPtr->prefs.frameInfo.blockMode = LZ6F_blockIndependent;

    /* Resolve the block size first: the HC context sizes its window/chain table
       to the max block, and that window is fixed for the context's lifetime. */
    if (cctxPtr->prefs.frameInfo.blockSizeID == 0) cctxPtr->prefs.frameInfo.blockSizeID = LZ6F_BLOCKSIZEID_DEFAULT;
    {
        size_t const newMaxBlockSize = LZ6F_getBlockSize(cctxPtr->prefs.frameInfo.blockSizeID);

        /* ctx Management */
        if (cctxPtr->prefs.frameInfo.blockCodec == LZ6F_blockCodec_seq)
        {
            /* seq engine: one-shot per block, no cross-block stream. The
             * seq wrapper reads the level from the cctx, so lz6CtxPtr
             * points back at it (never freed — freeStream only owns 1/2).
             * A light LZ stream is kept alive for the weak-seq fallback:
             * blocks where seq gains < 25% code with the fast LZ codec
             * instead and decode at memcpy speed. */
            LZ6F_freeStream(cctxPtr);
            cctxPtr->lz6CtxLevel = LZ6F_CCTX_SEQ;
            cctxPtr->lz6CtxPtr = cctxPtr;
            if (!cctxPtr->lightStream)
                cctxPtr->lightStream = (void*)LZ6_createStream();
        }
        else
        {
            U32 tableID = (cctxPtr->prefs.compressionLevel < minHClevel) ? 1 : 2;  /* 0:nothing ; 1:LZ6_createStream ; 2:LZ6_createStreamHC */
            /* recreate on level change, or (for HC) when the block size changed, since the window is sized to it */
            if ((cctxPtr->lz6CtxLevel != tableID) ||
                ((tableID == 2) && (newMaxBlockSize != cctxPtr->maxBlockSize)))
            {
                LZ6F_freeStream(cctxPtr);

                cctxPtr->lz6CtxLevel = tableID;
                if (cctxPtr->lz6CtxLevel == 1)
                    cctxPtr->lz6CtxPtr = (void*)LZ6_createStream();
                else
                    cctxPtr->lz6CtxPtr = (void*)LZ6_createStreamHC_sized(cctxPtr->prefs.compressionLevel, newMaxBlockSize);
            }
        }

        /* Buffer Management */
        cctxPtr->maxBlockSize = newMaxBlockSize;
    }

    requiredBuffSize = cctxPtr->maxBlockSize + ((cctxPtr->prefs.frameInfo.blockMode == LZ6F_blockLinked) * 2 * LZ6F_DICT_SIZE);
    if (preferencesPtr->autoFlush)
        requiredBuffSize = (cctxPtr->prefs.frameInfo.blockMode == LZ6F_blockLinked) * LZ6F_DICT_SIZE;   /* just needs dict */

    if (cctxPtr->maxBufferSize < requiredBuffSize)
    {
        cctxPtr->maxBufferSize = requiredBuffSize;
        FREEMEM(cctxPtr->tmpBuff);
        cctxPtr->tmpBuff = (BYTE*)ALLOCATOR(requiredBuffSize);
        if (cctxPtr->tmpBuff == NULL) return (size_t)-LZ6F_ERROR_allocation_failed;
    }
    cctxPtr->tmpIn = cctxPtr->tmpBuff;
    cctxPtr->tmpInSize = 0;
    XXH32_reset(&(cctxPtr->xxh), 0);
    if (cctxPtr->lz6CtxLevel == 1)
        LZ6_resetStream((LZ6_stream_t*)(cctxPtr->lz6CtxPtr));
    else if (cctxPtr->lz6CtxLevel == 2)
        LZ6_resetStreamHC((LZ6_streamHC_t*)(cctxPtr->lz6CtxPtr));

    /* Magic Number — single magic: the block codec rides on per-block
     * header flags (bit30 = seq), so frames may mix block types */
    LZ6F_writeLE32(dstPtr, LZ6F_MAGICNUMBER);
    dstPtr += 4;
    headerStart = dstPtr;

    /* FLG Byte */
    *dstPtr++ = (BYTE)(((1 & _2BITS) << 6)    /* Version('01') */
        + ((cctxPtr->prefs.frameInfo.blockMode & _1BIT ) << 5)    /* Block mode */
        + ((cctxPtr->prefs.frameInfo.contentChecksumFlag & _1BIT ) << 2)   /* Frame checksum */
        + ((cctxPtr->prefs.frameInfo.contentSize > 0) << 3));   /* Frame content size */
    /* BD Byte */
    *dstPtr++ = (BYTE)((cctxPtr->prefs.frameInfo.blockSizeID & _3BITS) << 4);
    /* Optional Frame content size field */
    if (cctxPtr->prefs.frameInfo.contentSize)
    {
        LZ6F_writeLE64(dstPtr, cctxPtr->prefs.frameInfo.contentSize);
        dstPtr += 8;
        cctxPtr->totalInSize = 0;
    }
    /* CRC Byte */
    *dstPtr = LZ6F_headerChecksum(headerStart, dstPtr - headerStart);
    dstPtr++;

    cctxPtr->cStage = 1;   /* header written, now request input data block */

    return (dstPtr - dstStart);
}


/* LZ6F_compressBound() : gives the size of Dst buffer given a srcSize to handle worst case situations.
*                        The LZ6F_frameInfo_t structure is optional :
*                        you can provide NULL as argument, preferences will then be set to cover worst case situations.
* */
size_t LZ6F_compressBound(size_t srcSize, const LZ6F_preferences_t* preferencesPtr)
{
    LZ6F_preferences_t prefsNull;
    memset(&prefsNull, 0, sizeof(prefsNull));
    prefsNull.frameInfo.contentChecksumFlag = LZ6F_contentChecksumEnabled;   /* worst case */
    {
        const LZ6F_preferences_t* prefsPtr = (preferencesPtr==NULL) ? &prefsNull : preferencesPtr;
        LZ6F_blockSizeID_t bid = prefsPtr->frameInfo.blockSizeID;
        size_t blockSize = LZ6F_getBlockSize(bid);
        unsigned nbBlocks = (unsigned)(srcSize / blockSize) + 1;
        size_t lastBlockSize = prefsPtr->autoFlush ? srcSize % blockSize : blockSize;
        size_t blockInfo = 4;   /* default, without block CRC option */
        size_t frameEnd = 4 + (prefsPtr->frameInfo.contentChecksumFlag*4);

        return (blockInfo * nbBlocks) + (blockSize * (nbBlocks-1)) + lastBlockSize + frameEnd;;
    }
}


typedef int (*compressFunc_t)(void* ctx, const char* src, char* dst, int srcSize, int dstSize);

/* seq blocks: adapt LZ6_decompress_seq to the frame decoder signature
 * (the dict arguments are meaningless — seq blocks are self-contained).
 * Returns -1 on failure like the LZ decoders. */
static int LZ6F_localLZ6_decompress_seq(const char* src, char* dst, int compressedSize, int maxDstCapacity, const char* dict, int dictSize)
{
    size_t const dSize = LZ6_decompress_seq(src, (size_t)compressedSize, dst, (size_t)maxDstCapacity);
    (void)dict; (void)dictSize;
    return dSize ? (int)dSize : -1;
}

/* LZ6S2: per-block codec dispatch.
 *  - seq frames (prefs.blockCodec == seq): each block is seq-coded; when the
 *    seq result gains < 25% over the raw input, the same block is retried
 *    with the fast LZ codec and the smaller output wins — those blocks
 *    decode at memcpy speed (bit30 clear).
 *  - LZ/HC frames: unchanged (bit30 never set).
 * Block header: bit31 = uncompressed escape, bit30 = seq-coded,
 * bits 29-0 = payload size (max block 256MB fits easily). */
static size_t LZ6F_compressBlock(void* dst, const void* src, size_t srcSize,
                                 compressFunc_t compress, LZ6F_cctx_t* cctxPtr)
{
    BYTE* cSizePtr = (BYTE*)dst;
    U32 cSize;

    if (cctxPtr->prefs.frameInfo.blockCodec == LZ6F_blockCodec_seq)
    {
        size_t r = LZ6_compress_seq((const char*)src, srcSize, (char*)(cSizePtr+4),
                                    (size_t)(srcSize-1), cctxPtr->prefs.compressionLevel);
        if (r)
        {
            if (r > srcSize - srcSize/4)   /* weak seq result: try light LZ */
            {
                /* the light attempt goes to a scratch buffer: clobbering
                 * the seq payload would leave the header promising a seq
                 * block over LZ bytes when the light result loses */
                BYTE* tmp = (BYTE*)malloc(srcSize);
                if (tmp)
                {
                    U32 lz = (U32)LZ6_compress_limitedOutput_withState(
                        (LZ6_stream_t*)cctxPtr->lightStream, (const char*)src,
                        (char*)tmp, (int)srcSize, (int)(srcSize-1));
                    if (lz && lz < (U32)r)
                    {
                        memcpy(cSizePtr+4, tmp, lz);
                        LZ6F_writeLE32(cSizePtr, lz);   /* bit30 clear: light block */
                        free(tmp);
                        return lz + 4;
                    }
                    free(tmp);
                }
            }
            LZ6F_writeLE32(cSizePtr, (U32)r | LZ6F_BLOCKSEQ_FLAG);
            return r + 4;
        }
        /* seq declined (incompressible/tiny): uncompressed escape */
        LZ6F_writeLE32(cSizePtr, (U32)srcSize + LZ6F_BLOCKUNCOMPRESSED_FLAG);
        memcpy(cSizePtr+4, src, srcSize);
        return srcSize + 4;
    }

    cSize = (U32)compress(cctxPtr->lz6CtxPtr, (const char*)src, (char*)(cSizePtr+4), (int)(srcSize), (int)(srcSize-1));
    LZ6F_writeLE32(cSizePtr, cSize);
    if (cSize == 0)   /* compression failed */
    {
        cSize = (U32)srcSize;
        LZ6F_writeLE32(cSizePtr, cSize + LZ6F_BLOCKUNCOMPRESSED_FLAG);
        memcpy(cSizePtr+4, src, srcSize);
    }
    return cSize + 4;
}


static int LZ6F_localLZ6_compress_limitedOutput_withState(void* ctx, const char* src, char* dst, int srcSize, int dstSize)
{
    return LZ6_compress_limitedOutput_withState(ctx, src, dst, srcSize, dstSize);
}

static int LZ6F_localLZ6_compress_limitedOutput_continue(void* ctx, const char* src, char* dst, int srcSize, int dstSize)
{
    return LZ6_compress_limitedOutput_continue((LZ6_stream_t*)ctx, src, dst, srcSize, dstSize);
}

static int LZ6F_localLZ6_compressHC_limitedOutput_continue(void* ctx, const char* src, char* dst, int srcSize, int dstSize)
{
    return LZ6_compress_HC_continue((LZ6_streamHC_t*)ctx, src, dst, srcSize, dstSize);
}

static int LZ6F_localLZ6_compress_seq(void* ctx, const char* src, char* dst, int srcSize, int dstSize)
{
    /* ctx is the cctx itself (lz6CtxLevel == LZ6F_CCTX_SEQ). Returns 0 when
     * the seq output doesn't fit dstSize (= srcSize-1): the frame then
     * stores the block uncompressed (bit31 escape), which is cheaper than
     * the seq coder's own raw escape (4+n vs 5+n) and keeps the frame's
     * compressBound contract (a block never codes larger than its input). */
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)ctx;
    size_t const cSize = LZ6_compress_seq(src, (size_t)srcSize, dst, (size_t)dstSize,
                                          cctxPtr->prefs.compressionLevel);
    return (int)cSize;
}

static compressFunc_t LZ6F_selectCompression(LZ6F_blockMode_t blockMode, int level, LZ6F_blockCodec_t blockCodec)
{
    if (blockCodec == LZ6F_blockCodec_seq)
        return LZ6F_localLZ6_compress_seq;
    if (level < minHClevel)
    {
        if (blockMode == LZ6F_blockIndependent) return LZ6F_localLZ6_compress_limitedOutput_withState;
        return LZ6F_localLZ6_compress_limitedOutput_continue;
    }
    if (blockMode == LZ6F_blockIndependent) return LZ6_compress_HC_extStateHC;
    return LZ6F_localLZ6_compressHC_limitedOutput_continue;
}

static int LZ6F_localSaveDict(LZ6F_cctx_t* cctxPtr)
{
    if (cctxPtr->prefs.compressionLevel < minHClevel)
        return LZ6_saveDict ((LZ6_stream_t*)(cctxPtr->lz6CtxPtr), (char*)(cctxPtr->tmpBuff), 64 KB);
    return LZ6_saveDictHC ((LZ6_streamHC_t*)(cctxPtr->lz6CtxPtr), (char*)(cctxPtr->tmpBuff), 64 KB);
}

typedef enum { notDone, fromTmpBuffer, fromSrcBuffer } LZ6F_lastBlockStatus;

/* LZ6F_compressUpdate()
* LZ6F_compressUpdate() can be called repetitively to compress as much data as necessary.
* The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
* If this condition is not respected, LZ6F_compress() will fail (result is an errorCode)
* You can get the minimum value of dstMaxSize by using LZ6F_compressBound()
* The LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
* The result of the function is the number of bytes written into dstBuffer : it can be zero, meaning input data was just buffered.
* The function outputs an error code if it fails (can be tested using LZ6F_isError())
*/
size_t LZ6F_compressUpdate(LZ6F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ6F_compressOptions_t* compressOptionsPtr)
{
    LZ6F_compressOptions_t cOptionsNull;
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)compressionContext;
    size_t blockSize = cctxPtr->maxBlockSize;
    const BYTE* srcPtr = (const BYTE*)srcBuffer;
    const BYTE* const srcEnd = srcPtr + srcSize;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    LZ6F_lastBlockStatus lastBlockCompressed = notDone;
    compressFunc_t compress;


    if (cctxPtr->cStage != 1) return (size_t)-LZ6F_ERROR_GENERIC;
    if (dstMaxSize < LZ6F_compressBound(srcSize, &(cctxPtr->prefs))) return (size_t)-LZ6F_ERROR_dstMaxSize_tooSmall;
    memset(&cOptionsNull, 0, sizeof(cOptionsNull));
    if (compressOptionsPtr == NULL) compressOptionsPtr = &cOptionsNull;

    /* select compression function */
    compress = LZ6F_selectCompression(cctxPtr->prefs.frameInfo.blockMode, cctxPtr->prefs.compressionLevel, cctxPtr->prefs.frameInfo.blockCodec);

    /* complete tmp buffer */
    if (cctxPtr->tmpInSize > 0)   /* some data already within tmp buffer */
    {
        size_t sizeToCopy = blockSize - cctxPtr->tmpInSize;
        if (sizeToCopy > srcSize)
        {
            /* add src to tmpIn buffer */
            memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, srcSize);
            srcPtr = srcEnd;
            cctxPtr->tmpInSize += srcSize;
            /* still needs some CRC */
        }
        else
        {
            /* complete tmpIn block and then compress it */
            lastBlockCompressed = fromTmpBuffer;
            memcpy(cctxPtr->tmpIn + cctxPtr->tmpInSize, srcBuffer, sizeToCopy);
            srcPtr += sizeToCopy;

            dstPtr += LZ6F_compressBlock(dstPtr, cctxPtr->tmpIn, blockSize, compress, cctxPtr);

            if (cctxPtr->prefs.frameInfo.blockMode==LZ6F_blockLinked) cctxPtr->tmpIn += blockSize;
            cctxPtr->tmpInSize = 0;
        }
    }

    while ((size_t)(srcEnd - srcPtr) >= blockSize)
    {
        /* compress full block */
        lastBlockCompressed = fromSrcBuffer;
        dstPtr += LZ6F_compressBlock(dstPtr, srcPtr, blockSize, compress, cctxPtr);
        srcPtr += blockSize;
    }

    if ((cctxPtr->prefs.autoFlush) && (srcPtr < srcEnd))
    {
        /* compress remaining input < blockSize */
        lastBlockCompressed = fromSrcBuffer;
        dstPtr += LZ6F_compressBlock(dstPtr, srcPtr, srcEnd - srcPtr, compress, cctxPtr);
        srcPtr  = srcEnd;
    }

    /* preserve dictionary if necessary */
    if ((cctxPtr->prefs.frameInfo.blockMode==LZ6F_blockLinked) && (lastBlockCompressed==fromSrcBuffer))
    {
        if (compressOptionsPtr->stableSrc)
        {
            cctxPtr->tmpIn = cctxPtr->tmpBuff;
        }
        else
        {
            int realDictSize = LZ6F_localSaveDict(cctxPtr);
            if (realDictSize==0) return (size_t)-LZ6F_ERROR_GENERIC;
            cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
        }
    }

    /* keep tmpIn within limits */
    if ((cctxPtr->tmpIn + blockSize) > (cctxPtr->tmpBuff + cctxPtr->maxBufferSize)   /* necessarily LZ6F_blockLinked && lastBlockCompressed==fromTmpBuffer */
        && !(cctxPtr->prefs.autoFlush))
    {
        int realDictSize = LZ6F_localSaveDict(cctxPtr);
        cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
    }

    /* some input data left, necessarily < blockSize */
    if (srcPtr < srcEnd)
    {
        /* fill tmp buffer */
        size_t sizeToCopy = srcEnd - srcPtr;
        memcpy(cctxPtr->tmpIn, srcPtr, sizeToCopy);
        cctxPtr->tmpInSize = sizeToCopy;
    }

    if (cctxPtr->prefs.frameInfo.contentChecksumFlag == LZ6F_contentChecksumEnabled)
        XXH32_update(&(cctxPtr->xxh), srcBuffer, srcSize);

    cctxPtr->totalInSize += srcSize;
    return dstPtr - dstStart;
}


/* LZ6F_flush()
* Should you need to create compressed data immediately, without waiting for a block to be filled,
* you can call LZ6_flush(), which will immediately compress any remaining data stored within compressionContext.
* The result of the function is the number of bytes written into dstBuffer
* (it can be zero, this means there was no data left within compressionContext)
* The function outputs an error code if it fails (can be tested using LZ6F_isError())
* The LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
*/
size_t LZ6F_flush(LZ6F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const LZ6F_compressOptions_t* compressOptionsPtr)
{
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)compressionContext;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    compressFunc_t compress;


    if (cctxPtr->tmpInSize == 0) return 0;   /* nothing to flush */
    if (cctxPtr->cStage != 1) return (size_t)-LZ6F_ERROR_GENERIC;
    if (dstMaxSize < (cctxPtr->tmpInSize + 8)) return (size_t)-LZ6F_ERROR_dstMaxSize_tooSmall;   /* +8 : block header(4) + block checksum(4) */
    (void)compressOptionsPtr;   /* not yet useful */

    /* select compression function */
    compress = LZ6F_selectCompression(cctxPtr->prefs.frameInfo.blockMode, cctxPtr->prefs.compressionLevel, cctxPtr->prefs.frameInfo.blockCodec);

    /* compress tmp buffer */
    dstPtr += LZ6F_compressBlock(dstPtr, cctxPtr->tmpIn, cctxPtr->tmpInSize, compress, cctxPtr);
    if (cctxPtr->prefs.frameInfo.blockMode==LZ6F_blockLinked) cctxPtr->tmpIn += cctxPtr->tmpInSize;
    cctxPtr->tmpInSize = 0;

    /* keep tmpIn within limits */
    if ((cctxPtr->tmpIn + cctxPtr->maxBlockSize) > (cctxPtr->tmpBuff + cctxPtr->maxBufferSize))   /* necessarily LZ6F_blockLinked */
    {
        int realDictSize = LZ6F_localSaveDict(cctxPtr);
        cctxPtr->tmpIn = cctxPtr->tmpBuff + realDictSize;
    }

    return dstPtr - dstStart;
}


/* LZ6F_compressEnd()
* When you want to properly finish the compressed frame, just call LZ6F_compressEnd().
* It will flush whatever data remained within compressionContext (like LZ6_flush())
* but also properly finalize the frame, with an endMark and a checksum.
* The result of the function is the number of bytes written into dstBuffer (necessarily >= 4 (endMark size))
* The function outputs an error code if it fails (can be tested using LZ6F_isError())
* The LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
* compressionContext can then be used again, starting with LZ6F_compressBegin(). The preferences will remain the same.
*/
size_t LZ6F_compressEnd(LZ6F_compressionContext_t compressionContext, void* dstBuffer, size_t dstMaxSize, const LZ6F_compressOptions_t* compressOptionsPtr)
{
    LZ6F_cctx_t* cctxPtr = (LZ6F_cctx_t*)compressionContext;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* dstPtr = dstStart;
    size_t errorCode;

    errorCode = LZ6F_flush(compressionContext, dstBuffer, dstMaxSize, compressOptionsPtr);
    if (LZ6F_isError(errorCode)) return errorCode;
    dstPtr += errorCode;

    LZ6F_writeLE32(dstPtr, 0);
    dstPtr+=4;   /* endMark */

    if (cctxPtr->prefs.frameInfo.contentChecksumFlag == LZ6F_contentChecksumEnabled)
    {
        U32 xxh = XXH32_digest(&(cctxPtr->xxh));
        LZ6F_writeLE32(dstPtr, xxh);
        dstPtr+=4;   /* content Checksum */
    }

    cctxPtr->cStage = 0;   /* state is now re-usable (with identical preferences) */

    if (cctxPtr->prefs.frameInfo.contentSize)
    {
        if (cctxPtr->prefs.frameInfo.contentSize != cctxPtr->totalInSize)
            return (size_t)-LZ6F_ERROR_frameSize_wrong;
    }

    return dstPtr - dstStart;
}


/**********************************
*  Decompression functions
**********************************/

/* Resource management */

/* LZ6F_createDecompressionContext() :
* The first thing to do is to create a decompressionContext object, which will be used in all decompression operations.
* This is achieved using LZ6F_createDecompressionContext().
* The function will provide a pointer to a fully allocated and initialized LZ6F_decompressionContext object.
* If the result LZ6F_errorCode_t is not zero, there was an error during context creation.
* Object can release its memory using LZ6F_freeDecompressionContext();
*/
LZ6F_errorCode_t LZ6F_createDecompressionContext(LZ6F_decompressionContext_t* LZ6F_decompressionContextPtr, unsigned versionNumber)
{
    LZ6F_dctx_t* dctxPtr;

    dctxPtr = (LZ6F_dctx_t*)ALLOCATOR(sizeof(LZ6F_dctx_t));
    if (dctxPtr==NULL) return (LZ6F_errorCode_t)-LZ6F_ERROR_GENERIC;

    dctxPtr->version = versionNumber;
    *LZ6F_decompressionContextPtr = (LZ6F_decompressionContext_t)dctxPtr;
    return LZ6F_OK_NoError;
}

LZ6F_errorCode_t LZ6F_freeDecompressionContext(LZ6F_decompressionContext_t LZ6F_decompressionContext)
{
    LZ6F_errorCode_t result = LZ6F_OK_NoError;
    LZ6F_dctx_t* dctxPtr = (LZ6F_dctx_t*)LZ6F_decompressionContext;
    if (dctxPtr != NULL)   /* can accept NULL input, like free() */
    {
      result = (LZ6F_errorCode_t)dctxPtr->dStage;
      FREEMEM(dctxPtr->tmpIn);
      FREEMEM(dctxPtr->tmpOutBuffer);
      FREEMEM(dctxPtr);
    }
    return result;
}


/* ******************************************************************** */
/* ********************* Decompression ******************************** */
/* ******************************************************************** */

typedef enum { dstage_getHeader=0, dstage_storeHeader,
    dstage_getCBlockSize, dstage_storeCBlockSize,
    dstage_copyDirect,
    dstage_getCBlock, dstage_storeCBlock,
    dstage_decodeCBlock, dstage_decodeCBlock_intoDst,
    dstage_decodeCBlock_intoTmp, dstage_flushOut,
    dstage_getSuffix, dstage_storeSuffix,
    dstage_getSFrameSize, dstage_storeSFrameSize,
    dstage_skipSkippable
} dStage_t;


/* LZ6F_decodeHeader
   return : nb Bytes read from srcVoidPtr (necessarily <= srcSize)
            or an error code (testable with LZ6F_isError())
   output : set internal values of dctx, such as
            dctxPtr->frameInfo and dctxPtr->dStage.
   input  : srcVoidPtr points at the **beginning of the frame**
*/
static size_t LZ6F_decodeHeader(LZ6F_dctx_t* dctxPtr, const void* srcVoidPtr, size_t srcSize)
{
    BYTE FLG, BD, HC;
    unsigned version, blockMode, blockChecksumFlag, contentSizeFlag, contentChecksumFlag, blockSizeID;
    size_t bufferNeeded;
    size_t frameHeaderSize;
    const BYTE* srcPtr = (const BYTE*)srcVoidPtr;

    /* need to decode header to get frameInfo */
    if (srcSize < minFHSize) return (size_t)-LZ6F_ERROR_frameHeader_incomplete;   /* minimal frame header size */
    memset(&(dctxPtr->frameInfo), 0, sizeof(dctxPtr->frameInfo));

    /* special case : skippable frames */
    if ((LZ6F_readLE32(srcPtr) & 0xFFFFFFF0U) == LZ6F_MAGIC_SKIPPABLE_START)
    {
        dctxPtr->frameInfo.frameType = LZ6F_skippableFrame;
        if (srcVoidPtr == (void*)(dctxPtr->header))
        {
            dctxPtr->tmpInSize = srcSize;
            dctxPtr->tmpInTarget = 8;
            dctxPtr->dStage = dstage_storeSFrameSize;
            return srcSize;
        }
        else
        {
            dctxPtr->dStage = dstage_getSFrameSize;
            return 4;
        }
    }

    /* control magic number — single magic: the block codec rides on the
     * per-block header flag (bit30), so frames may mix block types */
    if (LZ6F_readLE32(srcPtr) != LZ6F_MAGICNUMBER) return (size_t)-LZ6F_ERROR_frameType_unknown;
    dctxPtr->frameInfo.frameType = LZ6F_frame;
    dctxPtr->curBlockSeq = 0;

    /* Flags */
    FLG = srcPtr[4];
    version = (FLG>>6) & _2BITS;
    blockMode = (FLG>>5) & _1BIT;
    blockChecksumFlag = (FLG>>4) & _1BIT;
    contentSizeFlag = (FLG>>3) & _1BIT;
    contentChecksumFlag = (FLG>>2) & _1BIT;

    /* Frame Header Size */
    frameHeaderSize = contentSizeFlag ? maxFHSize : minFHSize;

    if (srcSize < frameHeaderSize)
    {
        /* not enough input to fully decode frame header */
        if (srcPtr != dctxPtr->header)
            memcpy(dctxPtr->header, srcPtr, srcSize);
        dctxPtr->tmpInSize = srcSize;
        dctxPtr->tmpInTarget = frameHeaderSize;
        dctxPtr->dStage = dstage_storeHeader;
        return srcSize;
    }

    BD = srcPtr[5];
    blockSizeID = (BD>>4) & _3BITS;

    /* validate */
    if (version != 1) return (size_t)-LZ6F_ERROR_headerVersion_wrong;        /* Version Number, only supported value */
    if (blockChecksumFlag != 0) return (size_t)-LZ6F_ERROR_blockChecksum_unsupported; /* Not supported for the time being */
    if (((FLG>>0)&_2BITS) != 0) return (size_t)-LZ6F_ERROR_reservedFlag_set; /* Reserved bits */
    if (((BD>>7)&_1BIT) != 0) return (size_t)-LZ6F_ERROR_reservedFlag_set;   /* Reserved bit */
    if (blockSizeID < 1) return (size_t)-LZ6F_ERROR_maxBlockSize_invalid;    /* 1-7 only supported values for the time being */
    if (((BD>>0)&_4BITS) != 0) return (size_t)-LZ6F_ERROR_reservedFlag_set;  /* Reserved bits */

    /* check */
    HC = LZ6F_headerChecksum(srcPtr+4, frameHeaderSize-5);
    if (HC != srcPtr[frameHeaderSize-1]) return (size_t)-LZ6F_ERROR_headerChecksum_invalid;   /* Bad header checksum error */

    /* save */
    dctxPtr->frameInfo.blockMode = (LZ6F_blockMode_t)blockMode;
    dctxPtr->frameInfo.contentChecksumFlag = (LZ6F_contentChecksum_t)contentChecksumFlag;
    dctxPtr->frameInfo.blockSizeID = (LZ6F_blockSizeID_t)blockSizeID;
    dctxPtr->maxBlockSize = LZ6F_getBlockSize(blockSizeID);
    if (contentSizeFlag)
        dctxPtr->frameRemainingSize = dctxPtr->frameInfo.contentSize = LZ6F_readLE64(srcPtr+6);

    /* init */
    if (contentChecksumFlag) XXH32_reset(&(dctxPtr->xxh), 0);

    /* alloc */
    bufferNeeded = dctxPtr->maxBlockSize + ((dctxPtr->frameInfo.blockMode==LZ6F_blockLinked) * 2 * LZ6F_DICT_SIZE);
    if (bufferNeeded > dctxPtr->maxBufferSize)   /* tmp buffers too small */
    {
        FREEMEM(dctxPtr->tmpIn);
        FREEMEM(dctxPtr->tmpOutBuffer);
        dctxPtr->maxBufferSize = bufferNeeded;
        dctxPtr->tmpIn = (BYTE*)ALLOCATOR(dctxPtr->maxBlockSize);
        if (dctxPtr->tmpIn == NULL) return (size_t)-LZ6F_ERROR_GENERIC;
        dctxPtr->tmpOutBuffer= (BYTE*)ALLOCATOR(dctxPtr->maxBufferSize);
        if (dctxPtr->tmpOutBuffer== NULL) return (size_t)-LZ6F_ERROR_GENERIC;
    }
    dctxPtr->tmpInSize = 0;
    dctxPtr->tmpInTarget = 0;
    dctxPtr->dict = dctxPtr->tmpOutBuffer;
    dctxPtr->dictSize = 0;
    dctxPtr->tmpOut = dctxPtr->tmpOutBuffer;
    dctxPtr->tmpOutStart = 0;
    dctxPtr->tmpOutSize = 0;

    dctxPtr->dStage = dstage_getCBlockSize;

    return frameHeaderSize;
}


/* LZ6F_getFrameInfo()
* This function decodes frame header information, such as blockSize.
* It is optional : you could start by calling directly LZ6F_decompress() instead.
* The objective is to extract header information without starting decompression, typically for allocation purposes.
* LZ6F_getFrameInfo() can also be used *after* starting decompression, on a valid LZ6F_decompressionContext_t.
* The number of bytes read from srcBuffer will be provided within *srcSizePtr (necessarily <= original value).
* You are expected to resume decompression from where it stopped (srcBuffer + *srcSizePtr)
* The function result is an hint of the better srcSize to use for next call to LZ6F_decompress,
* or an error code which can be tested using LZ6F_isError().
*/
LZ6F_errorCode_t LZ6F_getFrameInfo(LZ6F_decompressionContext_t dCtx, LZ6F_frameInfo_t* frameInfoPtr,
                                   const void* srcBuffer, size_t* srcSizePtr)
{
    LZ6F_dctx_t* dctxPtr = (LZ6F_dctx_t*)dCtx;

    if (dctxPtr->dStage > dstage_storeHeader)   /* note : requires dstage_* header related to be at beginning of enum */
    {
        size_t o=0, i=0;
        /* frameInfo already decoded */
        *srcSizePtr = 0;
        *frameInfoPtr = dctxPtr->frameInfo;
        return LZ6F_decompress(dCtx, NULL, &o, NULL, &i, NULL);
    }
    else
    {
        size_t o=0;
        size_t nextSrcSize = LZ6F_decompress(dCtx, NULL, &o, srcBuffer, srcSizePtr, NULL);
        if (dctxPtr->dStage <= dstage_storeHeader)   /* note : requires dstage_* header related to be at beginning of enum */
            return (size_t)-LZ6F_ERROR_frameHeader_incomplete;
        *frameInfoPtr = dctxPtr->frameInfo;
        return nextSrcSize;
    }
}


/* trivial redirector, for common prototype */
static int LZ6F_decompress_safe (const char* source, char* dest, int compressedSize, int maxDecompressedSize, const char* dictStart, int dictSize)
{
    (void)dictStart; (void)dictSize;
    return LZ6_decompress_safe (source, dest, compressedSize, maxDecompressedSize);
}


static void LZ6F_updateDict(LZ6F_dctx_t* dctxPtr, const BYTE* dstPtr, size_t dstSize, const BYTE* dstPtr0, unsigned withinTmp)
{
    if (dctxPtr->dictSize==0)
        dctxPtr->dict = (const BYTE*)dstPtr;   /* priority to dictionary continuity */

    if (dctxPtr->dict + dctxPtr->dictSize == dstPtr)   /* dictionary continuity */
    {
        dctxPtr->dictSize += dstSize;
        return;
    }

    if (dstPtr - dstPtr0 + dstSize >= 64 KB)   /* dstBuffer large enough to become dictionary */
    {
        dctxPtr->dict = (const BYTE*)dstPtr0;
        dctxPtr->dictSize = dstPtr - dstPtr0 + dstSize;
        return;
    }

    if ((withinTmp) && (dctxPtr->dict == dctxPtr->tmpOutBuffer))
    {
        /* assumption : dctxPtr->dict + dctxPtr->dictSize == dctxPtr->tmpOut + dctxPtr->tmpOutStart */
        dctxPtr->dictSize += dstSize;
        return;
    }

    if (withinTmp) /* copy relevant dict portion in front of tmpOut within tmpOutBuffer */
    {
        size_t preserveSize = dctxPtr->tmpOut - dctxPtr->tmpOutBuffer;
        size_t copySize = 64 KB - dctxPtr->tmpOutSize;
        const BYTE* oldDictEnd = dctxPtr->dict + dctxPtr->dictSize - dctxPtr->tmpOutStart;
        if (dctxPtr->tmpOutSize > 64 KB) copySize = 0;
        if (copySize > preserveSize) copySize = preserveSize;

        memcpy(dctxPtr->tmpOutBuffer + preserveSize - copySize, oldDictEnd - copySize, copySize);

        dctxPtr->dict = dctxPtr->tmpOutBuffer;
        dctxPtr->dictSize = preserveSize + dctxPtr->tmpOutStart + dstSize;
        return;
    }

    if (dctxPtr->dict == dctxPtr->tmpOutBuffer)     /* copy dst into tmp to complete dict */
    {
        if (dctxPtr->dictSize + dstSize > dctxPtr->maxBufferSize)   /* tmp buffer not large enough */
        {
            size_t preserveSize = 64 KB - dstSize;   /* note : dstSize < 64 KB */
            memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - preserveSize, preserveSize);
            dctxPtr->dictSize = preserveSize;
        }
        memcpy(dctxPtr->tmpOutBuffer + dctxPtr->dictSize, dstPtr, dstSize);
        dctxPtr->dictSize += dstSize;
        return;
    }

    /* join dict & dest into tmp */
    {
        size_t preserveSize = 64 KB - dstSize;   /* note : dstSize < 64 KB */
        if (preserveSize > dctxPtr->dictSize) preserveSize = dctxPtr->dictSize;
        memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - preserveSize, preserveSize);
        memcpy(dctxPtr->tmpOutBuffer + preserveSize, dstPtr, dstSize);
        dctxPtr->dict = dctxPtr->tmpOutBuffer;
        dctxPtr->dictSize = preserveSize + dstSize;
    }
}



/* LZ6F_decompress()
* Call this function repetitively to regenerate data compressed within srcBuffer.
* The function will attempt to decode *srcSizePtr from srcBuffer, into dstBuffer of maximum size *dstSizePtr.
*
* The number of bytes regenerated into dstBuffer will be provided within *dstSizePtr (necessarily <= original value).
*
* The number of bytes effectively read from srcBuffer will be provided within *srcSizePtr (necessarily <= original value).
* If the number of bytes read is < number of bytes provided, then the decompression operation is not complete.
* You will have to call it again, continuing from where it stopped.
*
* The function result is an hint of the better srcSize to use for next call to LZ6F_decompress.
* Basically, it's the size of the current (or remaining) compressed block + header of next block.
* Respecting the hint provides some boost to performance, since it allows less buffer shuffling.
* Note that this is just a hint, you can always provide any srcSize you want.
* When a frame is fully decoded, the function result will be 0.
* If decompression failed, function result is an error code which can be tested using LZ6F_isError().
*/
size_t LZ6F_decompress(LZ6F_decompressionContext_t decompressionContext,
                       void* dstBuffer, size_t* dstSizePtr,
                       const void* srcBuffer, size_t* srcSizePtr,
                       const LZ6F_decompressOptions_t* decompressOptionsPtr)
{
    LZ6F_dctx_t* dctxPtr = (LZ6F_dctx_t*)decompressionContext;
    LZ6F_decompressOptions_t optionsNull;
    const BYTE* const srcStart = (const BYTE*)srcBuffer;
    const BYTE* const srcEnd = srcStart + *srcSizePtr;
    const BYTE* srcPtr = srcStart;
    BYTE* const dstStart = (BYTE*)dstBuffer;
    BYTE* const dstEnd = dstStart + *dstSizePtr;
    BYTE* dstPtr = dstStart;
    const BYTE* selectedIn = NULL;
    unsigned doAnotherStage = 1;
    size_t nextSrcSizeHint = 1;


    memset(&optionsNull, 0, sizeof(optionsNull));
    if (decompressOptionsPtr==NULL) decompressOptionsPtr = &optionsNull;
    *srcSizePtr = 0;
    *dstSizePtr = 0;

    /* expect to continue decoding src buffer where it left previously */
    if (dctxPtr->srcExpect != NULL)
    {
        if (srcStart != dctxPtr->srcExpect) return (size_t)-LZ6F_ERROR_srcPtr_wrong;
    }

    /* programmed as a state machine */

    while (doAnotherStage)
    {

        switch(dctxPtr->dStage)
        {

        case dstage_getHeader:
            {
                if ((size_t)(srcEnd-srcPtr) >= maxFHSize)   /* enough to decode - shortcut */
                {
                    LZ6F_errorCode_t errorCode = LZ6F_decodeHeader(dctxPtr, srcPtr, srcEnd-srcPtr);
                    if (LZ6F_isError(errorCode)) return errorCode;
                    srcPtr += errorCode;
                    break;
                }
                dctxPtr->tmpInSize = 0;
                dctxPtr->tmpInTarget = minFHSize;   /* minimum to attempt decode */
                dctxPtr->dStage = dstage_storeHeader;
                __attribute__((fallthrough));  /* header too small, store into tmpIn */
            }

        case dstage_storeHeader:
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy =  srcEnd - srcPtr;
                memcpy(dctxPtr->header + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget)
                {
                    nextSrcSizeHint = (dctxPtr->tmpInTarget - dctxPtr->tmpInSize) + BHSize;   /* rest of header + nextBlockHeader */
                    doAnotherStage = 0;   /* not enough src data, ask for some more */
                    break;
                }
                {
                    LZ6F_errorCode_t errorCode = LZ6F_decodeHeader(dctxPtr, dctxPtr->header, dctxPtr->tmpInTarget);
                    if (LZ6F_isError(errorCode)) return errorCode;
                }
                break;
            }

        case dstage_getCBlockSize:
            {
                if ((size_t)(srcEnd - srcPtr) >= BHSize)
                {
                    selectedIn = srcPtr;
                    srcPtr += BHSize;
                }
                else
                {
                /* not enough input to read cBlockSize field */
                    dctxPtr->tmpInSize = 0;
                    dctxPtr->dStage = dstage_storeCBlockSize;
                }
            }

            if (dctxPtr->dStage == dstage_storeCBlockSize)
        case dstage_storeCBlockSize:
            {
                size_t sizeToCopy = BHSize - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < BHSize) /* not enough input to get full cBlockSize; wait for more */
                {
                    nextSrcSizeHint = BHSize - dctxPtr->tmpInSize;
                    doAnotherStage  = 0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
            }

        /* case dstage_decodeCBlockSize: */   /* no more direct access, to prevent scan-build warning */
            {
                U32 const bh = LZ6F_readLE32(selectedIn);
                size_t nextCBlockSize;
                if (bh==0)   /* frameEnd signal, no more CBlock */
                {
                    dctxPtr->dStage = dstage_getSuffix;
                    break;
                }
                if (bh & LZ6F_BLOCKUNCOMPRESSED_FLAG)
                {
                    nextCBlockSize = bh & 0x7FFFFFFFU;   /* raw: bits 30-0 are size */
                    dctxPtr->curBlockSeq = 0;
                }
                else
                {
                    nextCBlockSize = bh & 0x3FFFFFFFU;   /* compressed: bit30 = seq flag */
                    dctxPtr->curBlockSeq = (bh & LZ6F_BLOCKSEQ_FLAG) ? 1 : 0;
                }
                if (nextCBlockSize==0) return (size_t)-LZ6F_ERROR_GENERIC;   /* invalid cBlockSize */
                if (nextCBlockSize > dctxPtr->maxBlockSize) return (size_t)-LZ6F_ERROR_GENERIC; /* invalid cBlockSize */
                dctxPtr->tmpInTarget = nextCBlockSize;
                if (bh & LZ6F_BLOCKUNCOMPRESSED_FLAG)
                {
                    dctxPtr->dStage = dstage_copyDirect;
                    break;
                }
                dctxPtr->dStage = dstage_getCBlock;
                if (dstPtr==dstEnd)
                {
                    nextSrcSizeHint = nextCBlockSize + BHSize;
                    doAnotherStage = 0;
                }
                break;
            }

        case dstage_copyDirect:   /* uncompressed block */
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget;
                if ((size_t)(srcEnd-srcPtr) < sizeToCopy) sizeToCopy = srcEnd - srcPtr;  /* not enough input to read full block */
                if ((size_t)(dstEnd-dstPtr) < sizeToCopy) sizeToCopy = dstEnd - dstPtr;
                memcpy(dstPtr, srcPtr, sizeToCopy);
                if (dctxPtr->frameInfo.contentChecksumFlag) XXH32_update(&(dctxPtr->xxh), srcPtr, sizeToCopy);
                if (dctxPtr->frameInfo.contentSize) dctxPtr->frameRemainingSize -= sizeToCopy;

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ6F_blockLinked)
                    LZ6F_updateDict(dctxPtr, dstPtr, sizeToCopy, dstStart, 0);

                srcPtr += sizeToCopy;
                dstPtr += sizeToCopy;
                if (sizeToCopy == dctxPtr->tmpInTarget)   /* all copied */
                {
                    dctxPtr->dStage = dstage_getCBlockSize;
                    break;
                }
                dctxPtr->tmpInTarget -= sizeToCopy;   /* still need to copy more */
                nextSrcSizeHint = dctxPtr->tmpInTarget + BHSize;
                doAnotherStage = 0;
                break;
            }

        case dstage_getCBlock:   /* entry from dstage_decodeCBlockSize */
            {
                if ((size_t)(srcEnd-srcPtr) < dctxPtr->tmpInTarget)
                {
                    dctxPtr->tmpInSize = 0;
                    dctxPtr->dStage = dstage_storeCBlock;
                    break;
                }
                selectedIn = srcPtr;
                srcPtr += dctxPtr->tmpInTarget;
                dctxPtr->dStage = dstage_decodeCBlock;
                break;
            }

        case dstage_storeCBlock:
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd-srcPtr)) sizeToCopy = srcEnd-srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                dctxPtr->tmpInSize += sizeToCopy;
                srcPtr += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget)  /* need more input */
                {
                    nextSrcSizeHint = (dctxPtr->tmpInTarget - dctxPtr->tmpInSize) + BHSize;
                    doAnotherStage=0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
                dctxPtr->dStage = dstage_decodeCBlock;
                break;
            }

        case dstage_decodeCBlock:
            {
                if ((size_t)(dstEnd-dstPtr) < dctxPtr->maxBlockSize)   /* not enough place into dst : decode into tmpOut */
                    dctxPtr->dStage = dstage_decodeCBlock_intoTmp;
                else
                    dctxPtr->dStage = dstage_decodeCBlock_intoDst;
                break;
            }

        case dstage_decodeCBlock_intoDst:
            {
                int (*decoder)(const char*, char*, int, int, const char*, int);
                int decodedSize;

                if (dctxPtr->curBlockSeq)
                    decoder = LZ6F_localLZ6_decompress_seq;
                else if (dctxPtr->frameInfo.blockMode == LZ6F_blockLinked)
                    decoder = LZ6_decompress_safe_usingDict;
                else
                    decoder = LZ6F_decompress_safe;

                decodedSize = decoder((const char*)selectedIn, (char*)dstPtr, (int)dctxPtr->tmpInTarget, (int)dctxPtr->maxBlockSize, (const char*)dctxPtr->dict, (int)dctxPtr->dictSize);
                if (decodedSize < 0) return (size_t)-LZ6F_ERROR_GENERIC;    /* decompression failed */
                if (dctxPtr->frameInfo.contentChecksumFlag) XXH32_update(&(dctxPtr->xxh), dstPtr, decodedSize);
                if (dctxPtr->frameInfo.contentSize) dctxPtr->frameRemainingSize -= decodedSize;

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ6F_blockLinked)
                    LZ6F_updateDict(dctxPtr, dstPtr, decodedSize, dstStart, 0);

                dstPtr += decodedSize;
                dctxPtr->dStage = dstage_getCBlockSize;
                break;
            }

        case dstage_decodeCBlock_intoTmp:
            {
                /* not enough place into dst : decode into tmpOut */
                int (*decoder)(const char*, char*, int, int, const char*, int);
                int decodedSize;

                if (dctxPtr->curBlockSeq)
                    decoder = LZ6F_localLZ6_decompress_seq;
                else if (dctxPtr->frameInfo.blockMode == LZ6F_blockLinked)
                    decoder = LZ6_decompress_safe_usingDict;
                else
                    decoder = LZ6F_decompress_safe;

                /* ensure enough place for tmpOut */
                if (dctxPtr->frameInfo.blockMode == LZ6F_blockLinked)
                {
                    if (dctxPtr->dict == dctxPtr->tmpOutBuffer)
                    {
                        if (dctxPtr->dictSize > 128 KB)
                        {
                            memcpy(dctxPtr->tmpOutBuffer, dctxPtr->dict + dctxPtr->dictSize - 64 KB, 64 KB);
                            dctxPtr->dictSize = 64 KB;
                        }
                        dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + dctxPtr->dictSize;
                    }
                    else   /* dict not within tmp */
                    {
                        size_t reservedDictSpace = dctxPtr->dictSize;
                        if (reservedDictSpace > 64 KB) reservedDictSpace = 64 KB;
                        dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + reservedDictSpace;
                    }
                }

                /* Decode */
                decodedSize = decoder((const char*)selectedIn, (char*)dctxPtr->tmpOut, (int)dctxPtr->tmpInTarget, (int)dctxPtr->maxBlockSize, (const char*)dctxPtr->dict, (int)dctxPtr->dictSize);
                if (decodedSize < 0) return (size_t)-LZ6F_ERROR_decompressionFailed;   /* decompression failed */
                if (dctxPtr->frameInfo.contentChecksumFlag) XXH32_update(&(dctxPtr->xxh), dctxPtr->tmpOut, decodedSize);
                if (dctxPtr->frameInfo.contentSize) dctxPtr->frameRemainingSize -= decodedSize;
                dctxPtr->tmpOutSize = decodedSize;
                dctxPtr->tmpOutStart = 0;
                dctxPtr->dStage = dstage_flushOut;
                break;
            }

        case dstage_flushOut:  /* flush decoded data from tmpOut to dstBuffer */
            {
                size_t sizeToCopy = dctxPtr->tmpOutSize - dctxPtr->tmpOutStart;
                if (sizeToCopy > (size_t)(dstEnd-dstPtr)) sizeToCopy = dstEnd-dstPtr;
                memcpy(dstPtr, dctxPtr->tmpOut + dctxPtr->tmpOutStart, sizeToCopy);

                /* dictionary management */
                if (dctxPtr->frameInfo.blockMode==LZ6F_blockLinked)
                    LZ6F_updateDict(dctxPtr, dstPtr, sizeToCopy, dstStart, 1);

                dctxPtr->tmpOutStart += sizeToCopy;
                dstPtr += sizeToCopy;

                /* end of flush ? */
                if (dctxPtr->tmpOutStart == dctxPtr->tmpOutSize)
                {
                    dctxPtr->dStage = dstage_getCBlockSize;
                    break;
                }
                nextSrcSizeHint = BHSize;
                doAnotherStage = 0;   /* still some data to flush */
                break;
            }

        case dstage_getSuffix:
            {
                size_t suffixSize = dctxPtr->frameInfo.contentChecksumFlag * 4;
                if (dctxPtr->frameRemainingSize) return (size_t)-LZ6F_ERROR_frameSize_wrong;   /* incorrect frame size decoded */
                if (suffixSize == 0)   /* frame completed */
                {
                    nextSrcSizeHint = 0;
                    dctxPtr->dStage = dstage_getHeader;
                    doAnotherStage = 0;
                    break;
                }
                if ((srcEnd - srcPtr) < 4)   /* not enough size for entire CRC */
                {
                    dctxPtr->tmpInSize = 0;
                    dctxPtr->dStage = dstage_storeSuffix;
                }
                else
                {
                    selectedIn = srcPtr;
                    srcPtr += 4;
                }
            }

            if (dctxPtr->dStage == dstage_storeSuffix)
        case dstage_storeSuffix:
            {
                size_t sizeToCopy = 4 - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->tmpIn + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < 4)  /* not enough input to read complete suffix */
                {
                    nextSrcSizeHint = 4 - dctxPtr->tmpInSize;
                    doAnotherStage=0;
                    break;
                }
                selectedIn = dctxPtr->tmpIn;
            }

        /* case dstage_checkSuffix: */   /* no direct call, to avoid scan-build warning */
            {
                U32 readCRC = LZ6F_readLE32(selectedIn);
                U32 resultCRC = XXH32_digest(&(dctxPtr->xxh));
                if (readCRC != resultCRC) return (size_t)-LZ6F_ERROR_contentChecksum_invalid;
                nextSrcSizeHint = 0;
                dctxPtr->dStage = dstage_getHeader;
                doAnotherStage = 0;
                break;
            }

        case dstage_getSFrameSize:
            {
                if ((srcEnd - srcPtr) >= 4)
                {
                    selectedIn = srcPtr;
                    srcPtr += 4;
                }
                else
                {
                /* not enough input to read cBlockSize field */
                    dctxPtr->tmpInSize = 4;
                    dctxPtr->tmpInTarget = 8;
                    dctxPtr->dStage = dstage_storeSFrameSize;
                }
            }

            if (dctxPtr->dStage == dstage_storeSFrameSize)
        case dstage_storeSFrameSize:
            {
                size_t sizeToCopy = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                if (sizeToCopy > (size_t)(srcEnd - srcPtr)) sizeToCopy = srcEnd - srcPtr;
                memcpy(dctxPtr->header + dctxPtr->tmpInSize, srcPtr, sizeToCopy);
                srcPtr += sizeToCopy;
                dctxPtr->tmpInSize += sizeToCopy;
                if (dctxPtr->tmpInSize < dctxPtr->tmpInTarget) /* not enough input to get full sBlockSize; wait for more */
                {
                    nextSrcSizeHint = dctxPtr->tmpInTarget - dctxPtr->tmpInSize;
                    doAnotherStage = 0;
                    break;
                }
                selectedIn = dctxPtr->header + 4;
            }

        /* case dstage_decodeSFrameSize: */   /* no direct access */
            {
                size_t SFrameSize = LZ6F_readLE32(selectedIn);
                dctxPtr->frameInfo.contentSize = SFrameSize;
                dctxPtr->tmpInTarget = SFrameSize;
                dctxPtr->dStage = dstage_skipSkippable;
                break;
            }

        case dstage_skipSkippable:
            {
                size_t skipSize = dctxPtr->tmpInTarget;
                if (skipSize > (size_t)(srcEnd-srcPtr)) skipSize = srcEnd-srcPtr;
                srcPtr += skipSize;
                dctxPtr->tmpInTarget -= skipSize;
                doAnotherStage = 0;
                nextSrcSizeHint = dctxPtr->tmpInTarget;
                if (nextSrcSizeHint) break;
                dctxPtr->dStage = dstage_getHeader;
                break;
            }
        }
    }

    /* preserve dictionary within tmp if necessary */
    if ( (dctxPtr->frameInfo.blockMode==LZ6F_blockLinked)
        &&(dctxPtr->dict != dctxPtr->tmpOutBuffer)
        &&(!decompressOptionsPtr->stableDst)
        &&((unsigned)(dctxPtr->dStage-1) < (unsigned)(dstage_getSuffix-1))
        )
    {
        if (dctxPtr->dStage == dstage_flushOut)
        {
            size_t preserveSize = dctxPtr->tmpOut - dctxPtr->tmpOutBuffer;
            size_t copySize = 64 KB - dctxPtr->tmpOutSize;
            const BYTE* oldDictEnd = dctxPtr->dict + dctxPtr->dictSize - dctxPtr->tmpOutStart;
            if (dctxPtr->tmpOutSize > 64 KB) copySize = 0;
            if (copySize > preserveSize) copySize = preserveSize;

            memcpy(dctxPtr->tmpOutBuffer + preserveSize - copySize, oldDictEnd - copySize, copySize);

            dctxPtr->dict = dctxPtr->tmpOutBuffer;
            dctxPtr->dictSize = preserveSize + dctxPtr->tmpOutStart;
        }
        else
        {
            size_t newDictSize = dctxPtr->dictSize;
            const BYTE* oldDictEnd = dctxPtr->dict + dctxPtr->dictSize;
            if ((newDictSize) > 64 KB) newDictSize = 64 KB;

            memcpy(dctxPtr->tmpOutBuffer, oldDictEnd - newDictSize, newDictSize);

            dctxPtr->dict = dctxPtr->tmpOutBuffer;
            dctxPtr->dictSize = newDictSize;
            dctxPtr->tmpOut = dctxPtr->tmpOutBuffer + newDictSize;
        }
    }

    /* require function to be called again from position where it stopped */
    if (srcPtr<srcEnd)
        dctxPtr->srcExpect = srcPtr;
    else
        dctxPtr->srcExpect = NULL;

    *srcSizePtr = (srcPtr - srcStart);
    *dstSizePtr = (dstPtr - dstStart);
    return nextSrcSizeHint;
}
