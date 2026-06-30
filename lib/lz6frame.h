/*
   LZ6 auto-framing library
   Header File
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

/* LZ6F is a stand-alone API to create LZ6-compressed frames
 * conformant with specification v1.5.1.
 * All related operations, including memory management, are handled internally by the library.
 * You don't need lz6.h when using lz6frame.h.
 * */

#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

/**************************************
*  Includes
**************************************/
#include <stddef.h>   /* size_t */


/**************************************
*  Error management
**************************************/
typedef size_t LZ6F_errorCode_t;

unsigned    LZ6F_isError(LZ6F_errorCode_t code);
const char* LZ6F_getErrorName(LZ6F_errorCode_t code);   /* return error code string; useful for debugging */


/**************************************
*  Frame compression types
**************************************/
//#define LZ6F_DISABLE_OBSOLETE_ENUMS
#ifndef LZ6F_DISABLE_OBSOLETE_ENUMS
#  define LZ6F_OBSOLETE_ENUM(x) ,x
#else
#  define LZ6F_OBSOLETE_ENUM(x)
#endif

typedef enum {
    LZ6F_default=0,
    LZ6F_max64KB=1,
    LZ6F_max256KB=2,
    LZ6F_max1MB=3,
    LZ6F_max4MB=4,
    LZ6F_max16MB=5,
    LZ6F_max64MB=6,
    LZ6F_max256MB=7
    LZ6F_OBSOLETE_ENUM(max64KB = LZ6F_max64KB)
    LZ6F_OBSOLETE_ENUM(max256KB = LZ6F_max256KB)
    LZ6F_OBSOLETE_ENUM(max1MB = LZ6F_max1MB)
    LZ6F_OBSOLETE_ENUM(max4MB = LZ6F_max4MB)
} LZ6F_blockSizeID_t;

typedef enum {
    LZ6F_blockLinked=0,
    LZ6F_blockIndependent
    LZ6F_OBSOLETE_ENUM(blockLinked = LZ6F_blockLinked)
    LZ6F_OBSOLETE_ENUM(blockIndependent = LZ6F_blockIndependent)
} LZ6F_blockMode_t;

typedef enum {
    LZ6F_noContentChecksum=0,
    LZ6F_contentChecksumEnabled
    LZ6F_OBSOLETE_ENUM(noContentChecksum = LZ6F_noContentChecksum)
    LZ6F_OBSOLETE_ENUM(contentChecksumEnabled = LZ6F_contentChecksumEnabled)
} LZ6F_contentChecksum_t;

typedef enum {
    LZ6F_frame=0,
    LZ6F_skippableFrame
    LZ6F_OBSOLETE_ENUM(skippableFrame = LZ6F_skippableFrame)
} LZ6F_frameType_t;

#ifndef LZ6F_DISABLE_OBSOLETE_ENUMS
typedef LZ6F_blockSizeID_t blockSizeID_t;
typedef LZ6F_blockMode_t blockMode_t;
typedef LZ6F_frameType_t frameType_t;
typedef LZ6F_contentChecksum_t contentChecksum_t;
#endif

typedef struct {
  LZ6F_blockSizeID_t     blockSizeID;           /* max64KB, max256KB, max1MB, max4MB ; 0 == default */
  LZ6F_blockMode_t       blockMode;             /* blockLinked, blockIndependent ; 0 == default */
  LZ6F_contentChecksum_t contentChecksumFlag;   /* noContentChecksum, contentChecksumEnabled ; 0 == default  */
  LZ6F_frameType_t       frameType;             /* LZ6F_frame, skippableFrame ; 0 == default */
  unsigned long long     contentSize;           /* Size of uncompressed (original) content ; 0 == unknown */
  unsigned               reserved[2];           /* must be zero for forward compatibility */
} LZ6F_frameInfo_t;

typedef struct {
  LZ6F_frameInfo_t frameInfo;
  int      compressionLevel;       /* 0 == default (fast mode); values above 16 count as 16; values below 0 count as 0 */
  unsigned autoFlush;              /* 1 == always flush (reduce need for tmp buffer) */
  unsigned reserved[4];            /* must be zero for forward compatibility */
} LZ6F_preferences_t;


/***********************************
*  Simple compression function
***********************************/
size_t LZ6F_compressFrameBound(size_t srcSize, const LZ6F_preferences_t* preferencesPtr);

size_t LZ6F_compressFrame(void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ6F_preferences_t* preferencesPtr);
/* LZ6F_compressFrame()
 * Compress an entire srcBuffer into a valid LZ6 frame, as defined by specification v1.5.1
 * The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
 * You can get the minimum value of dstMaxSize by using LZ6F_compressFrameBound()
 * If this condition is not respected, LZ6F_compressFrame() will fail (result is an errorCode)
 * The LZ6F_preferences_t structure is optional : you can provide NULL as argument. All preferences will be set to default.
 * The result of the function is the number of bytes written into dstBuffer.
 * The function outputs an error code if it fails (can be tested using LZ6F_isError())
 */



/**********************************
*  Advanced compression functions
**********************************/
typedef struct LZ6F_cctx_s* LZ6F_compressionContext_t;   /* must be aligned on 8-bytes */

typedef struct {
  unsigned stableSrc;    /* 1 == src content will remain available on future calls to LZ6F_compress(); avoid saving src content within tmp buffer as future dictionary */
  unsigned reserved[3];
} LZ6F_compressOptions_t;

/* Resource Management */

#define LZ6F_VERSION 100
LZ6F_errorCode_t LZ6F_createCompressionContext(LZ6F_compressionContext_t* cctxPtr, unsigned version);
LZ6F_errorCode_t LZ6F_freeCompressionContext(LZ6F_compressionContext_t cctx);
/* LZ6F_createCompressionContext() :
 * The first thing to do is to create a compressionContext object, which will be used in all compression operations.
 * This is achieved using LZ6F_createCompressionContext(), which takes as argument a version and an LZ6F_preferences_t structure.
 * The version provided MUST be LZ6F_VERSION. It is intended to track potential version differences between different binaries.
 * The function will provide a pointer to a fully allocated LZ6F_compressionContext_t object.
 * If the result LZ6F_errorCode_t is not zero, there was an error during context creation.
 * Object can release its memory using LZ6F_freeCompressionContext();
 */


/* Compression */

size_t LZ6F_compressBegin(LZ6F_compressionContext_t cctx, void* dstBuffer, size_t dstMaxSize, const LZ6F_preferences_t* prefsPtr);
/* LZ6F_compressBegin() :
 * will write the frame header into dstBuffer.
 * dstBuffer must be large enough to accommodate a header (dstMaxSize). Maximum header size is 15 bytes.
 * The LZ6F_preferences_t structure is optional : you can provide NULL as argument, all preferences will then be set to default.
 * The result of the function is the number of bytes written into dstBuffer for the header
 * or an error code (can be tested using LZ6F_isError())
 */

size_t LZ6F_compressBound(size_t srcSize, const LZ6F_preferences_t* prefsPtr);
/* LZ6F_compressBound() :
 * Provides the minimum size of Dst buffer given srcSize to handle worst case situations.
 * Different preferences can produce different results.
 * prefsPtr is optional : you can provide NULL as argument, all preferences will then be set to cover worst case.
 * This function includes frame termination cost (4 bytes, or 8 if frame checksum is enabled)
 */

size_t LZ6F_compressUpdate(LZ6F_compressionContext_t cctx, void* dstBuffer, size_t dstMaxSize, const void* srcBuffer, size_t srcSize, const LZ6F_compressOptions_t* cOptPtr);
/* LZ6F_compressUpdate()
 * LZ6F_compressUpdate() can be called repetitively to compress as much data as necessary.
 * The most important rule is that dstBuffer MUST be large enough (dstMaxSize) to ensure compression completion even in worst case.
 * You can get the minimum value of dstMaxSize by using LZ6F_compressBound().
 * If this condition is not respected, LZ6F_compress() will fail (result is an errorCode).
 * LZ6F_compressUpdate() doesn't guarantee error recovery, so you have to reset compression context when an error occurs.
 * The LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
 * The result of the function is the number of bytes written into dstBuffer : it can be zero, meaning input data was just buffered.
 * The function outputs an error code if it fails (can be tested using LZ6F_isError())
 */

size_t LZ6F_flush(LZ6F_compressionContext_t cctx, void* dstBuffer, size_t dstMaxSize, const LZ6F_compressOptions_t* cOptPtr);
/* LZ6F_flush()
 * Should you need to generate compressed data immediately, without waiting for the current block to be filled,
 * you can call LZ6_flush(), which will immediately compress any remaining data buffered within cctx.
 * Note that dstMaxSize must be large enough to ensure the operation will be successful.
 * LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
 * The result of the function is the number of bytes written into dstBuffer
 * (it can be zero, this means there was no data left within cctx)
 * The function outputs an error code if it fails (can be tested using LZ6F_isError())
 */

size_t LZ6F_compressEnd(LZ6F_compressionContext_t cctx, void* dstBuffer, size_t dstMaxSize, const LZ6F_compressOptions_t* cOptPtr);
/* LZ6F_compressEnd()
 * When you want to properly finish the compressed frame, just call LZ6F_compressEnd().
 * It will flush whatever data remained within compressionContext (like LZ6_flush())
 * but also properly finalize the frame, with an endMark and a checksum.
 * The result of the function is the number of bytes written into dstBuffer (necessarily >= 4 (endMark), or 8 if optional frame checksum is enabled)
 * The function outputs an error code if it fails (can be tested using LZ6F_isError())
 * The LZ6F_compressOptions_t structure is optional : you can provide NULL as argument.
 * A successful call to LZ6F_compressEnd() makes cctx available again for next compression task.
 */


/***********************************
*  Decompression functions
***********************************/

typedef struct LZ6F_dctx_s* LZ6F_decompressionContext_t;   /* must be aligned on 8-bytes */

typedef struct {
  unsigned stableDst;       /* guarantee that decompressed data will still be there on next function calls (avoid storage into tmp buffers) */
  unsigned reserved[3];
} LZ6F_decompressOptions_t;


/* Resource management */

LZ6F_errorCode_t LZ6F_createDecompressionContext(LZ6F_decompressionContext_t* dctxPtr, unsigned version);
LZ6F_errorCode_t LZ6F_freeDecompressionContext(LZ6F_decompressionContext_t dctx);
/* LZ6F_createDecompressionContext() :
 * The first thing to do is to create an LZ6F_decompressionContext_t object, which will be used in all decompression operations.
 * This is achieved using LZ6F_createDecompressionContext().
 * The version provided MUST be LZ6F_VERSION. It is intended to track potential breaking differences between different versions.
 * The function will provide a pointer to a fully allocated and initialized LZ6F_decompressionContext_t object.
 * The result is an errorCode, which can be tested using LZ6F_isError().
 * dctx memory can be released using LZ6F_freeDecompressionContext();
 * The result of LZ6F_freeDecompressionContext() is indicative of the current state of decompressionContext when being released.
 * That is, it should be == 0 if decompression has been completed fully and correctly.
 */


/* Decompression */

size_t LZ6F_getFrameInfo(LZ6F_decompressionContext_t dctx,
                         LZ6F_frameInfo_t* frameInfoPtr,
                         const void* srcBuffer, size_t* srcSizePtr);
/* LZ6F_getFrameInfo()
 * This function decodes frame header information (such as max blockSize, frame checksum, etc.).
 * Its usage is optional. The objective is to extract frame header information, typically for allocation purposes.
 * A header size is variable and can be from 7 to 15 bytes. It's also possible to input more bytes than that. 
 * The number of bytes read from srcBuffer will be updated within *srcSizePtr (necessarily <= original value).
 * (note that LZ6F_getFrameInfo() can also be used anytime *after* starting decompression, in this case 0 input byte is enough)
 * Frame header info is *copied into* an already allocated LZ6F_frameInfo_t structure.
 * The function result is an hint about how many srcSize bytes LZ6F_decompress() expects for next call,
 *                        or an error code which can be tested using LZ6F_isError()
 *                        (typically, when there is not enough src bytes to fully decode the frame header)
 * Decompression is expected to resume from where it stopped (srcBuffer + *srcSizePtr)
 */

size_t LZ6F_decompress(LZ6F_decompressionContext_t dctx,
                       void* dstBuffer, size_t* dstSizePtr,
                       const void* srcBuffer, size_t* srcSizePtr,
                       const LZ6F_decompressOptions_t* dOptPtr);
/* LZ6F_decompress()
 * Call this function repetitively to regenerate data compressed within srcBuffer.
 * The function will attempt to decode *srcSizePtr bytes from srcBuffer, into dstBuffer of maximum size *dstSizePtr.
 *
 * The number of bytes regenerated into dstBuffer will be provided within *dstSizePtr (necessarily <= original value).
 *
 * The number of bytes read from srcBuffer will be provided within *srcSizePtr (necessarily <= original value).
 * If number of bytes read is < number of bytes provided, then decompression operation is not completed.
 * It typically happens when dstBuffer is not large enough to contain all decoded data.
 * LZ6F_decompress() must be called again, starting from where it stopped (srcBuffer + *srcSizePtr)
 * The function will check this condition, and refuse to continue if it is not respected.
 *
 * dstBuffer is supposed to be flushed between each call to the function, since its content will be overwritten.
 * dst arguments can be changed at will with each consecutive call to the function.
 *
 * The function result is an hint of how many srcSize bytes LZ6F_decompress() expects for next call.
 * Schematically, it's the size of the current (or remaining) compressed block + header of next block.
 * Respecting the hint provides some boost to performance, since it does skip intermediate buffers.
 * This is just a hint, you can always provide any srcSize you want.
 * When a frame is fully decoded, the function result will be 0 (no more data expected).
 * If decompression failed, function result is an error code, which can be tested using LZ6F_isError().
 *
 * After a frame is fully decoded, dctx can be used again to decompress another frame.
 */


#if defined (__cplusplus)
}
#endif
