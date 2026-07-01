/*
   LZ6 HC - High Compression Mode of LZ6
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
#pragma once


#if defined (__cplusplus)
extern "C" {
#endif

/*****************************
*  Includes
*****************************/
#include <stddef.h>   /* size_t */


/**************************************
*  Block Compression
**************************************/
int LZ6_compress_HC (const char* src, char* dst, int srcSize, int maxDstSize, int compressionLevel);
/*
LZ6_compress_HC :
    Destination buffer 'dst' must be already allocated.
    Compression completion is guaranteed if 'dst' buffer is sized to handle worst circumstances (data not compressible)
    Worst size evaluation is provided by function LZ6_compressBound() (see "lz6.h")
      srcSize  : Max supported value is LZ6_MAX_INPUT_SIZE (see "lz6.h")
      compressionLevel : Recommended values are between 4 and 9, although any value between 0 and LZ6HC_MAX_CLEVEL (equal to 15) will work.
                         0 means "use default value" (see lz6hc.c).
                         Values >LZ6HC_MAX_CLEVEL behave the same as LZ6HC_MAX_CLEVEL.
      return : the number of bytes written into buffer 'dst'
            or 0 if compression fails.
*/


/* Note :
   Decompression functions are provided within LZ6 source code (see "lz6.h") (BSD license)
*/

typedef struct LZ6HC_Data_s LZ6HC_Data_Structure;

int LZ6_alloc_mem_HC(LZ6HC_Data_Structure* statePtr, int compressionLevel);
int LZ6_alloc_mem_HC_sized(LZ6HC_Data_Structure* statePtr, int compressionLevel, size_t maxSrcSize);
void LZ6_free_mem_HC(LZ6HC_Data_Structure* statePtr);

int LZ6_sizeofStateHC(void);
int LZ6_compress_HC_extStateHC(void* state, const char* src, char* dst, int srcSize, int maxDstSize);
/*
LZ6_compress_HC_extStateHC() :
   Use this function if you prefer to manually allocate memory for compression tables.
   To know how much memory must be allocated for the compression tables, use :
      int LZ6_sizeofStateHC();

   Allocated memory must be aligned on 8-bytes boundaries (which a normal malloc() will do properly).

   The allocated memory can then be provided to the compression functions using 'void* state' parameter.
   LZ6_compress_HC_extStateHC() is equivalent to previously described function.
   It just uses externally allocated memory for stateHC.
*/


/**************************************
*  Streaming Compression
**************************************/
#define LZ6_STREAMHCSIZE        262192
#define LZ6_STREAMHCSIZE_SIZET (LZ6_STREAMHCSIZE / sizeof(size_t))
typedef struct { size_t table[LZ6_STREAMHCSIZE_SIZET]; } LZ6_streamHC_t;
/*
  LZ6_streamHC_t
  This structure allows static allocation of LZ6 HC streaming state.
  State must then be initialized using LZ6_resetStreamHC() before first use.

  Static allocation should only be used in combination with static linking.
  If you want to use LZ6 as a DLL, please use construction functions below, which are future-proof.
*/


LZ6_streamHC_t* LZ6_createStreamHC(int compressionLevel);
LZ6_streamHC_t* LZ6_createStreamHC_sized(int compressionLevel, size_t maxBlockSize);
int             LZ6_freeStreamHC (LZ6_streamHC_t* streamHCPtr);
/*
  These functions create and release memory for LZ6 HC streaming state.
  Newly created states are already initialized.
  Existing state space can be re-used anytime using LZ6_resetStreamHC().
  If you use LZ6 as a DLL, use these functions instead of static structure allocation,
  to avoid size mismatch between different versions.
*/

void LZ6_resetStreamHC (LZ6_streamHC_t* streamHCPtr);
int  LZ6_loadDictHC (LZ6_streamHC_t* streamHCPtr, const char* dictionary, int dictSize);

int LZ6_compress_HC_continue (LZ6_streamHC_t* streamHCPtr, const char* src, char* dst, int srcSize, int maxDstSize);

int LZ6_saveDictHC (LZ6_streamHC_t* streamHCPtr, char* safeBuffer, int maxDictSize);

/*
  These functions compress data in successive blocks of any size, using previous blocks as dictionary.
  One key assumption is that previous blocks (up to 64 KB) remain read-accessible while compressing next blocks.
  There is an exception for ring buffers, which can be smaller 64 KB.
  Such case is automatically detected and correctly handled by LZ6_compress_HC_continue().

  Before starting compression, state must be properly initialized, using LZ6_resetStreamHC().
  A first "fictional block" can then be designated as initial dictionary, using LZ6_loadDictHC() (Optional).

  Then, use LZ6_compress_HC_continue() to compress each successive block.
  It works like LZ6_compress_HC(), but use previous memory blocks as dictionary to improve compression.
  Previous memory blocks (including initial dictionary when present) must remain accessible and unmodified during compression.
  As a reminder, size 'dst' buffer to handle worst cases, using LZ6_compressBound(), to ensure success of compression operation.

  If, for any reason, previous data blocks can't be preserved unmodified in memory during next compression block,
  you must save it to a safer memory space, using LZ6_saveDictHC().
  Return value of LZ6_saveDictHC() is the size of dictionary effectively saved into 'safeBuffer'.
*/



/**************************************
*  Deprecated Functions
**************************************/
/* Deprecate Warnings */
/* Should these warnings messages be a problem,
   it is generally possible to disable them,
   with -Wno-deprecated-declarations for gcc
   or _CRT_SECURE_NO_WARNINGS in Visual for example.
   You can also define LZ6_DEPRECATE_WARNING_DEFBLOCK. */
#ifndef LZ6_DEPRECATE_WARNING_DEFBLOCK
#  define LZ6_DEPRECATE_WARNING_DEFBLOCK
#  if (LZ6_GCC_VERSION >= 405) || defined(__clang__)
#    define LZ6_DEPRECATED(message) __attribute__((deprecated(message)))
#  elif (LZ6_GCC_VERSION >= 301)
#    define LZ6_DEPRECATED(message) __attribute__((deprecated))
#  elif defined(_MSC_VER)
#    define LZ6_DEPRECATED(message) __declspec(deprecated(message))
#  else
#    pragma message("WARNING: You need to implement LZ6_DEPRECATED for this compiler")
#    define LZ6_DEPRECATED(message)
#  endif
#endif // LZ6_DEPRECATE_WARNING_DEFBLOCK

/* compression functions */
/* these functions are planned to trigger warning messages by r132 approximately */
int LZ6_compressHC                (const char* source, char* dest, int inputSize);
int LZ6_compressHC_limitedOutput  (const char* source, char* dest, int inputSize, int maxOutputSize);
int LZ6_compressHC_continue               (LZ6_streamHC_t* LZ6_streamHCPtr, const char* source, char* dest, int inputSize);
int LZ6_compressHC_limitedOutput_continue (LZ6_streamHC_t* LZ6_streamHCPtr, const char* source, char* dest, int inputSize, int maxOutputSize);
int LZ6_compressHC_withStateHC               (void* state, const char* source, char* dest, int inputSize);
int LZ6_compressHC_limitedOutput_withStateHC (void* state, const char* source, char* dest, int inputSize, int maxOutputSize); 

#if defined (__cplusplus)
}
#endif
