/*
   LZ6 - Fast LZ compression algorithm
   Copyright (C) 2011-2015, Yann Collet.
   Copyright (C) 2015, Przemyslaw Skibinski <inikep@gmail.com>

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



/**************************************
*  Includes
**************************************/
#include "lz6common.h"
#include "lz6.h"
#include <stdio.h>


/**************************************
*  Local Constants
**************************************/
#define LZ6_HASHLOG   (LZ6_MEMORY_USAGE-2)
#define HASH_SIZE_U32 (1 << LZ6_HASHLOG)       /* required as macro for static allocation */

static const int LZ6_64Klimit = ((64 KB) + (MFLIMIT-1));
static const U32 LZ6_skipTrigger = 6;  /* Increase this value ==> compression run slower on incompressible data */


/**************************************
*  Local Structures and types
**************************************/
typedef struct {
    U32 hashTable[HASH_SIZE_U32];
    U32 currentOffset;
    U32 initCheck;
    const BYTE* dictionary;
    BYTE* bufferStart;   /* obsolete, used for slideInputBuffer */
    U32 dictSize;
} LZ6_stream_t_internal;

typedef enum { notLimited = 0, limitedOutput = 1 } limitedOutput_directive;
typedef enum { byPtr, byU32, byU16 } tableType_t;

typedef enum { noDict = 0, withPrefix64k, usingExtDict } dict_directive;
typedef enum { noDictIssue = 0, dictSmall } dictIssue_directive;

typedef enum { endOnOutputSize = 0, endOnInputSize = 1 } endCondition_directive;
typedef enum { full = 0, partial = 1 } earlyEnd_directive;


/**************************************
*  Local Utils
**************************************/
int LZ6_versionNumber (void) { return LZ6_VERSION_NUMBER; }
int LZ6_compressBound(int isize)  { return LZ6_COMPRESSBOUND(isize); }
int LZ6_sizeofState() { return LZ6_STREAMSIZE; }



/********************************
*  Compression functions
********************************/

static U32 LZ6_hashSequence(U32 sequence, tableType_t const tableType)
{
    if (tableType == byU16)
        return (((sequence) * prime4bytes) >> ((32)-(LZ6_HASHLOG+1)));
    else
        return (((sequence) * prime4bytes) >> ((32)-LZ6_HASHLOG));
}

static U32 LZ6_hashSequence64(size_t sequence, tableType_t const tableType)
{
    const U32 hashLog = (tableType == byU16) ? LZ6_HASHLOG+1 : LZ6_HASHLOG;
    const U32 hashMask = (1<<hashLog) - 1;
    return ((sequence * prime5bytes) >> (40 - hashLog)) & hashMask;
}

static U32 LZ6_hashSequenceT(size_t sequence, tableType_t const tableType)
{
    if (MEM_64bits())
        return LZ6_hashSequence64(sequence, tableType);
    return LZ6_hashSequence((U32)sequence, tableType);
}

static U32 LZ6_hashPosition(const void* p, tableType_t tableType) { return LZ6_hashSequenceT(MEM_read_ARCH(p), tableType); }

static void LZ6_putPositionOnHash(const BYTE* p, U32 h, void* tableBase, tableType_t const tableType, const BYTE* srcBase)
{
    switch (tableType)
    {
    case byPtr: { const BYTE** hashTable = (const BYTE**)tableBase; hashTable[h] = p; return; }
    case byU32: { U32* hashTable = (U32*) tableBase; hashTable[h] = (U32)(p-srcBase); return; }
    case byU16: { U16* hashTable = (U16*) tableBase; hashTable[h] = (U16)(p-srcBase); return; }
    }
}

static void LZ6_putPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ6_hashPosition(p, tableType);
    LZ6_putPositionOnHash(p, h, tableBase, tableType, srcBase);
}

static const BYTE* LZ6_getPositionOnHash(U32 h, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    if (tableType == byPtr) { const BYTE** hashTable = (const BYTE**) tableBase; return hashTable[h]; }
    if (tableType == byU32) { U32* hashTable = (U32*) tableBase; return hashTable[h] + srcBase; }
    { U16* hashTable = (U16*) tableBase; return hashTable[h] + srcBase; }   /* default, to ensure a return */
}

static const BYTE* LZ6_getPosition(const BYTE* p, void* tableBase, tableType_t tableType, const BYTE* srcBase)
{
    U32 h = LZ6_hashPosition(p, tableType);
    return LZ6_getPositionOnHash(h, tableBase, tableType, srcBase);
}

FORCE_INLINE int LZ6_compress_generic(
                 void* const ctx,
                 const char* const source,
                 char* const dest,
                 const int inputSize,
                 const int maxOutputSize,
                 const limitedOutput_directive outputLimited,
                 const tableType_t tableType,
                 const dict_directive dict,
                 const dictIssue_directive dictIssue,
                 const U32 acceleration)
{
    LZ6_stream_t_internal* const dictPtr = (LZ6_stream_t_internal*)ctx;

    const BYTE* ip = (const BYTE*) source;
    const BYTE* base;
    const BYTE* lowLimit;
    const BYTE* const lowRefLimit = ip - dictPtr->dictSize;
    const BYTE* const dictionary = dictPtr->dictionary;
    const BYTE* const dictEnd = dictionary + dictPtr->dictSize;
    const size_t dictDelta = dictEnd - (const BYTE*)source;
    const BYTE* anchor = (const BYTE*) source;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;

    BYTE* op = (BYTE*) dest;
    BYTE* const olimit = op + maxOutputSize;

    U32 forwardH, last_off=1;
    size_t refDelta=0;

    /* Init conditions */
    if ((U32)inputSize > (U32)LZ6_MAX_INPUT_SIZE) return 0;   /* Unsupported input size, too large (or negative) */
    switch(dict)
    {
    case noDict:
    default:
        base = (const BYTE*)source;
        lowLimit = (const BYTE*)source;
        break;
    case withPrefix64k:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source - dictPtr->dictSize;
        break;
    case usingExtDict:
        base = (const BYTE*)source - dictPtr->currentOffset;
        lowLimit = (const BYTE*)source;
        break;
    }
    if ((tableType == byU16) && (inputSize>=LZ6_64Klimit)) return 0;   /* Size too large (not within 64K limit) */
    if (inputSize<LZ6_minLength) goto _last_literals;                  /* Input too small, no compression (all literals) */

    /* First Byte */
    LZ6_putPosition(ip, ctx, tableType, base);
    ip++; forwardH = LZ6_hashPosition(ip, tableType);

    /* Main Loop */
    for ( ; ; )
    {
        const BYTE* match;
        BYTE* token;
        {
            const BYTE* forwardIp = ip;
            unsigned step = 1;
            unsigned searchMatchNb = acceleration << LZ6_skipTrigger;

            /* Find a match */
            do {
                U32 h = forwardH;
                ip = forwardIp;
                forwardIp += step;
                step = (searchMatchNb++ >> LZ6_skipTrigger);

                if (unlikely(forwardIp > mflimit)) goto _last_literals;

                match = LZ6_getPositionOnHash(h, ctx, tableType, base);
                if (dict==usingExtDict)
                {
                    if (match<(const BYTE*)source)
                    {
                        refDelta = dictDelta;
                        lowLimit = dictionary;
                    }
                    else
                    {
                        refDelta = 0;
                        lowLimit = (const BYTE*)source;
                    }
                }
                forwardH = LZ6_hashPosition(forwardIp, tableType);
                LZ6_putPositionOnHash(ip, h, ctx, tableType, base);

            } while ( ((dictIssue==dictSmall) ? (match < lowRefLimit) : 0)
                || ((tableType==byU16) ? 0 : (match + MAX_DISTANCE < ip))
                || (MEM_read32(match+refDelta) != MEM_read32(ip)) );
        }

        /* Catch up */
        while ((ip>anchor) && (match+refDelta > lowLimit) && (unlikely(ip[-1]==match[refDelta-1]))) { ip--; match--; }

        {
            /* Encode Literal length */
            unsigned litLength = (unsigned)(ip - anchor);
            token = op++;
            if ((outputLimited) && (unlikely(op + litLength + (2 + 1 + LASTLITERALS) + (litLength/255) > olimit)))
                return 0;   /* Check output limit */

            if (ip-match >= LZ6_SHORT_OFFSET_DISTANCE && ip-match < LZ6_MID_OFFSET_DISTANCE && (U32)(ip-match) != last_off)
            {
                if (litLength>=RUN_MASK)
                {
                    int len = (int)litLength-RUN_MASK;
                    *token=(RUN_MASK<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }
            else
            {
                if (litLength>=RUN_MASK2)
                {
                    int len = (int)litLength-RUN_MASK2;
                    *token=(RUN_MASK2<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }

            /* Copy Literals */
            MEM_wildCopy(op, anchor, op+litLength);
            op+=litLength;
        }

_next_match:
		{
			/* Encode Offset */
			U32 offset = (U32)(ip-match);
			if (offset == last_off)
			{
				*token+=(3<<ML_RUN_BITS2);
			}
			else
			if (ip-match < LZ6_SHORT_OFFSET_DISTANCE)
			{
				*token+=(BYTE)((4+(offset>>8))<<ML_RUN_BITS2);
				*op++=(BYTE)offset;
			}
			else
			if (ip-match < LZ6_MID_OFFSET_DISTANCE)
			{
				MEM_writeLE16(op, (U16)offset); op+=2;
			}
			else
			{
				*token+=(2<<ML_RUN_BITS2);
				MEM_writeLE24(op, (U32)offset); op+=3;
			}
			last_off = offset;
		}

        /* Encode MatchLength */
        {
            size_t matchLength;

            if ((dict==usingExtDict) && (lowLimit==dictionary))
            {
                const BYTE* limit;
                match += refDelta;
                limit = ip + (dictEnd-match);
                if (limit > matchlimit) limit = matchlimit;
                matchLength = MEM_count(ip+MINMATCH, match+MINMATCH, limit);
                ip += MINMATCH + matchLength;
                if (ip==limit)
                {
                    size_t more = MEM_count(ip, (const BYTE*)source, matchlimit);
                    matchLength += more;
                    ip += more;
                }
            }
            else
            {
                matchLength = MEM_count(ip+MINMATCH, match+MINMATCH, matchlimit);
                ip += MINMATCH + matchLength;
            }

            if ((outputLimited) && (unlikely(op + (1 + LASTLITERALS) + (matchLength>>8) > olimit)))
                return 0;    /* Check output limit */
            if (matchLength>=ML_MASK)
            {
                *token += ML_MASK;
                matchLength -= ML_MASK;
                for (; matchLength >= 510 ; matchLength-=510) { *op++ = 255; *op++ = 255; }
                if (matchLength >= 255) { matchLength-=255; *op++ = 255; }
                *op++ = (BYTE)matchLength;
            }
            else *token += (BYTE)(matchLength);
        }

        anchor = ip;

        /* Test end of chunk */
        if (ip > mflimit) break;

        /* Fill table */
        LZ6_putPosition(ip-2, ctx, tableType, base);

        /* Test next position */
        match = LZ6_getPosition(ip, ctx, tableType, base);
        if (dict==usingExtDict)
        {
            if (match<(const BYTE*)source)
            {
                refDelta = dictDelta;
                lowLimit = dictionary;
            }
            else
            {
                refDelta = 0;
                lowLimit = (const BYTE*)source;
            }
        }
        LZ6_putPosition(ip, ctx, tableType, base);
        if ( ((dictIssue==dictSmall) ? (match>=lowRefLimit) : 1)
            && (match+MAX_DISTANCE>=ip)
            && (MEM_read32(match+refDelta)==MEM_read32(ip)) )
        { token=op++; *token=0; goto _next_match; }

        /* Prepare next loop */
        forwardH = LZ6_hashPosition(++ip, tableType);
    }

_last_literals:
    /* Encode Last Literals */
    {
        const size_t lastRun = (size_t)(iend - anchor);
        if ((outputLimited) && ((op - (BYTE*)dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize))
            return 0;   /* Check output limit */
        if (lastRun >= RUN_MASK)
        {
            size_t accumulator = lastRun - RUN_MASK;
            *op++ = RUN_MASK << ML_BITS;
            for(; accumulator >= 255 ; accumulator-=255) *op++ = 255;
            *op++ = (BYTE) accumulator;
        }
        else
        {
            *op++ = (BYTE)(lastRun<<ML_BITS);
        }
        memcpy(op, anchor, lastRun);
        op += lastRun;
    }

    /* End */
    return (int) (((char*)op)-dest);
}


int LZ6_compress_fast_extState(void* state, const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ6_resetStream((LZ6_stream_t*)state);
    if (acceleration < 1) acceleration = ACCELERATION_DEFAULT;

    if (maxOutputSize >= LZ6_compressBound(inputSize))
    {
        if (inputSize < LZ6_64Klimit)
            return LZ6_compress_generic(state, source, dest, inputSize, 0, notLimited, byU16,                        noDict, noDictIssue, acceleration);
        else
            return LZ6_compress_generic(state, source, dest, inputSize, 0, notLimited, MEM_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
    }
    else
    {
        if (inputSize < LZ6_64Klimit)
            return LZ6_compress_generic(state, source, dest, inputSize, maxOutputSize, limitedOutput, byU16,                        noDict, noDictIssue, acceleration);
        else
            return LZ6_compress_generic(state, source, dest, inputSize, maxOutputSize, limitedOutput, MEM_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
    }
}


int LZ6_compress_fast(const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
#if (HEAPMODE)
    void* ctxPtr = ALLOCATOR(1, sizeof(LZ6_stream_t));   /* malloc-calloc always properly aligned */
#else
    LZ6_stream_t ctx;
    void* ctxPtr = &ctx;
#endif

    int result = LZ6_compress_fast_extState(ctxPtr, source, dest, inputSize, maxOutputSize, acceleration);

#if (HEAPMODE)
    FREEMEM(ctxPtr);
#endif
    return result;
}


int LZ6_compress_default(const char* source, char* dest, int inputSize, int maxOutputSize)
{
    return LZ6_compress_fast(source, dest, inputSize, maxOutputSize, 1);
}


/* hidden debug function */
/* strangely enough, gcc generates faster code when this function is uncommented, even if unused */
int LZ6_compress_fast_force(const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ6_stream_t ctx;

    LZ6_resetStream(&ctx);

    if (inputSize < LZ6_64Klimit)
        return LZ6_compress_generic(&ctx, source, dest, inputSize, maxOutputSize, limitedOutput, byU16,                        noDict, noDictIssue, acceleration);
    else
        return LZ6_compress_generic(&ctx, source, dest, inputSize, maxOutputSize, limitedOutput, MEM_64bits() ? byU32 : byPtr, noDict, noDictIssue, acceleration);
}


/********************************
*  destSize variant
********************************/

static int LZ6_compress_destSize_generic(
                       void* const ctx,
                 const char* const src,
                       char* const dst,
                       int*  const srcSizePtr,
                 const int targetDstSize,
                 const tableType_t tableType)
{
    const BYTE* ip = (const BYTE*) src;
    const BYTE* base = (const BYTE*) src;
    const BYTE* lowLimit = (const BYTE*) src;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + *srcSizePtr;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;

    BYTE* op = (BYTE*) dst;
    BYTE* const oend = op + targetDstSize;
    BYTE* const oMaxLit = op + targetDstSize - 2 /* offset */ - 8 /* because 8+MINMATCH==MFLIMIT */ - 1 /* token */;
    BYTE* const oMaxMatch = op + targetDstSize - (LASTLITERALS + 1 /* token */);
    BYTE* const oMaxSeq = oMaxLit - 1 /* token */;

    U32 forwardH, last_off=1;


    /* Init conditions */
    if (targetDstSize < 1) return 0;                                     /* Impossible to store anything */
    if ((U32)*srcSizePtr > (U32)LZ6_MAX_INPUT_SIZE) return 0;            /* Unsupported input size, too large (or negative) */
    if ((tableType == byU16) && (*srcSizePtr>=LZ6_64Klimit)) return 0;   /* Size too large (not within 64K limit) */
    if (*srcSizePtr<LZ6_minLength) goto _last_literals;                  /* Input too small, no compression (all literals) */

    /* First Byte */
    *srcSizePtr = 0;
    LZ6_putPosition(ip, ctx, tableType, base);
    ip++; forwardH = LZ6_hashPosition(ip, tableType);

    /* Main Loop */
    for ( ; ; )
    {
        const BYTE* match;
        BYTE* token;
        {
            const BYTE* forwardIp = ip;
            unsigned step = 1;
            unsigned searchMatchNb = 1 << LZ6_skipTrigger;

            /* Find a match */
            do {
                U32 h = forwardH;
                ip = forwardIp;
                forwardIp += step;
                step = (searchMatchNb++ >> LZ6_skipTrigger);

                if (unlikely(forwardIp > mflimit))
                    goto _last_literals;

                match = LZ6_getPositionOnHash(h, ctx, tableType, base);
                forwardH = LZ6_hashPosition(forwardIp, tableType);
                LZ6_putPositionOnHash(ip, h, ctx, tableType, base);

            } while ( ((tableType==byU16) ? 0 : (match + MAX_DISTANCE < ip))
                || (MEM_read32(match) != MEM_read32(ip)) );
        }

        /* Catch up */
        while ((ip>anchor) && (match > lowLimit) && (unlikely(ip[-1]==match[-1]))) { ip--; match--; }

        {
            /* Encode Literal length */
            unsigned litLength = (unsigned)(ip - anchor);
            token = op++;
            if (op + ((litLength+240)/255) + litLength > oMaxLit)
            {
                /* Not enough space for a last match */
                op--;
                goto _last_literals;
            }

            if ((U32)(ip-match) >= LZ6_SHORT_OFFSET_DISTANCE && (U32)(ip-match) < LZ6_MID_OFFSET_DISTANCE && (U32)(ip-match) != last_off)
            {
                if (litLength>=RUN_MASK)
                {
                    int len = (int)litLength-RUN_MASK;
                    *token=(RUN_MASK<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }
            else
            {
                if (litLength>=RUN_MASK2)
                {
                    int len = (int)litLength-RUN_MASK2;
                    *token=(RUN_MASK2<<ML_BITS);
                    for(; len >= 255 ; len-=255) *op++ = 255;
                    *op++ = (BYTE)len;
                }
                else *token = (BYTE)(litLength<<ML_BITS);
            }

            /* Copy Literals */
            MEM_wildCopy(op, anchor, op+litLength);
            op += litLength;
        }

_next_match:
		{
			U32 offset = (U32)(ip-match);
			/* Encode Offset */
			if (offset == last_off)
			{
				*token+=(3<<ML_RUN_BITS2);          
			}
			else
			if (ip-match < LZ6_SHORT_OFFSET_DISTANCE)
			{
				*token+=(BYTE)((4+(offset>>8))<<ML_RUN_BITS2);
				*op++=(BYTE)offset;
			}
			else
			if (ip-match < LZ6_MID_OFFSET_DISTANCE)
			{
				MEM_writeLE16(op, (U16)offset); op+=2;
			}
			else
			{
				*token+=(2<<ML_RUN_BITS2);
				MEM_writeLE24(op, (U32)offset); op+=3;
			}
			last_off = offset;
		}

        /* Encode MatchLength */
        {
            size_t matchLength;

            matchLength = MEM_count(ip+MINMATCH, match+MINMATCH, matchlimit);

            if (op + ((matchLength+240)/255) > oMaxMatch)
            {
                /* Match description too long : reduce it */
                matchLength = (15-1) + (oMaxMatch-op) * 255;
            }
            ip += MINMATCH + matchLength;

            if (matchLength>=ML_MASK)
            {
                *token += ML_MASK;
                matchLength -= ML_MASK;
                while (matchLength >= 255) { matchLength-=255; *op++ = 255; }
                *op++ = (BYTE)matchLength;
            }
            else *token += (BYTE)(matchLength);
        }

        anchor = ip;

        /* Test end of block */
        if (ip > mflimit) break;
        if (op > oMaxSeq) break;

        /* Fill table */
        LZ6_putPosition(ip-2, ctx, tableType, base);

        /* Test next position */
        match = LZ6_getPosition(ip, ctx, tableType, base);
        LZ6_putPosition(ip, ctx, tableType, base);
        if ( (match+MAX_DISTANCE>=ip)
            && (MEM_read32(match)==MEM_read32(ip)) )
        { token=op++; *token=0; goto _next_match; }

        /* Prepare next loop */
        forwardH = LZ6_hashPosition(++ip, tableType);
    }

_last_literals:
    /* Encode Last Literals */
    {
        size_t lastRunSize = (size_t)(iend - anchor);
        if (op + 1 /* token */ + ((lastRunSize+240)/255) /* litLength */ + lastRunSize /* literals */ > oend)
        {
            /* adapt lastRunSize to fill 'dst' */
            lastRunSize  = (oend-op) - 1;
            lastRunSize -= (lastRunSize+240)/255;
        }
        ip = anchor + lastRunSize;

        if (lastRunSize >= RUN_MASK)
        {
            size_t accumulator = lastRunSize - RUN_MASK;
            *op++ = RUN_MASK << ML_BITS;
            for(; accumulator >= 255 ; accumulator-=255) *op++ = 255;
            *op++ = (BYTE) accumulator;
        }
        else
        {
            *op++ = (BYTE)(lastRunSize<<ML_BITS);
        }
        memcpy(op, anchor, lastRunSize);
        op += lastRunSize;
    }

    /* End */
    *srcSizePtr = (int) (((const char*)ip)-src);
    return (int) (((char*)op)-dst);
}


static int LZ6_compress_destSize_extState (void* state, const char* src, char* dst, int* srcSizePtr, int targetDstSize)
{
    LZ6_resetStream((LZ6_stream_t*)state);

    if (targetDstSize >= LZ6_compressBound(*srcSizePtr))   /* compression success is guaranteed */
    {
        return LZ6_compress_fast_extState(state, src, dst, *srcSizePtr, targetDstSize, 1);
    }
    else
    {
        if (*srcSizePtr < LZ6_64Klimit)
            return LZ6_compress_destSize_generic(state, src, dst, srcSizePtr, targetDstSize, byU16);
        else
            return LZ6_compress_destSize_generic(state, src, dst, srcSizePtr, targetDstSize, MEM_64bits() ? byU32 : byPtr);
    }
}


int LZ6_compress_destSize(const char* src, char* dst, int* srcSizePtr, int targetDstSize)
{
#if (HEAPMODE)
    void* ctx = ALLOCATOR(1, sizeof(LZ6_stream_t));   /* malloc-calloc always properly aligned */
#else
    LZ6_stream_t ctxBody;
    void* ctx = &ctxBody;
#endif

    int result = LZ6_compress_destSize_extState(ctx, src, dst, srcSizePtr, targetDstSize);

#if (HEAPMODE)
    FREEMEM(ctx);
#endif
    return result;
}



/********************************
*  Streaming functions
********************************/

LZ6_stream_t* LZ6_createStream(void)
{
    LZ6_stream_t* lz6s = (LZ6_stream_t*)ALLOCATOR(8, LZ6_STREAMSIZE_U64);
    LZ6_STATIC_ASSERT(LZ6_STREAMSIZE >= sizeof(LZ6_stream_t_internal));    /* A compilation error here means LZ6_STREAMSIZE is not large enough */
    LZ6_resetStream(lz6s);
    return lz6s;
}

void LZ6_resetStream (LZ6_stream_t* LZ6_stream)
{
    MEM_INIT(LZ6_stream, 0, sizeof(LZ6_stream_t));
}

int LZ6_freeStream (LZ6_stream_t* LZ6_stream)
{
    FREEMEM(LZ6_stream);
    return (0);
}


#define HASH_UNIT sizeof(size_t)
int LZ6_loadDict (LZ6_stream_t* LZ6_dict, const char* dictionary, int dictSize)
{
    LZ6_stream_t_internal* dict = (LZ6_stream_t_internal*) LZ6_dict;
    const BYTE* p = (const BYTE*)dictionary;
    const BYTE* const dictEnd = p + dictSize;
    const BYTE* base;

    if ((dict->initCheck) || (dict->currentOffset > 1 GB))  /* Uninitialized structure, or reuse overflow */
        LZ6_resetStream(LZ6_dict);

 /*   if (dictSize < (int)HASH_UNIT)
    {
        dict->dictionary = NULL;
        dict->dictSize = 0;
        return 0;
    }*/

    if ((dictEnd - p) > LZ6_DICT_SIZE) p = dictEnd - LZ6_DICT_SIZE;
    dict->currentOffset += LZ6_DICT_SIZE;
    base = p - dict->currentOffset;
    dict->dictionary = p;
    dict->dictSize = (U32)(dictEnd - p);
    dict->currentOffset += dict->dictSize;

    while (p <= dictEnd-HASH_UNIT)
    {
        LZ6_putPosition(p, dict->hashTable, byU32, base);
        p+=3;
    }

    return dict->dictSize;
}


static void LZ6_renormDictT(LZ6_stream_t_internal* LZ6_dict, const BYTE* src)
{
    if ((LZ6_dict->currentOffset > 0x80000000) ||
        ((size_t)LZ6_dict->currentOffset > (size_t)src))   /* address space overflow */
    {
        /* rescale hash table */
        U32 delta = LZ6_dict->currentOffset - LZ6_DICT_SIZE;
        const BYTE* dictEnd = LZ6_dict->dictionary + LZ6_dict->dictSize;
        int i;
        for (i=0; i<HASH_SIZE_U32; i++)
        {
            if (LZ6_dict->hashTable[i] < delta) LZ6_dict->hashTable[i]=0;
            else LZ6_dict->hashTable[i] -= delta;
        }
        LZ6_dict->currentOffset = LZ6_DICT_SIZE;
        if (LZ6_dict->dictSize > LZ6_DICT_SIZE) LZ6_dict->dictSize = LZ6_DICT_SIZE;
        LZ6_dict->dictionary = dictEnd - LZ6_dict->dictSize;
    }
}


int LZ6_compress_fast_continue (LZ6_stream_t* LZ6_stream, const char* source, char* dest, int inputSize, int maxOutputSize, int acceleration)
{
    LZ6_stream_t_internal* streamPtr = (LZ6_stream_t_internal*)LZ6_stream;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = (const BYTE*) source;
    if (streamPtr->initCheck) return 0;   /* Uninitialized structure detected */
    if ((streamPtr->dictSize>0) && (smallest>dictEnd)) smallest = dictEnd;
    LZ6_renormDictT(streamPtr, smallest);
    if (acceleration < 1) acceleration = ACCELERATION_DEFAULT;

    /* Check overlapping input/dictionary space */
    {
        const BYTE* sourceEnd = (const BYTE*) source + inputSize;
        if ((sourceEnd > streamPtr->dictionary) && (sourceEnd < dictEnd))
        {
            streamPtr->dictSize = (U32)(dictEnd - sourceEnd);
            if (streamPtr->dictSize > LZ6_DICT_SIZE) streamPtr->dictSize = LZ6_DICT_SIZE;
            if (streamPtr->dictSize < 4) streamPtr->dictSize = 0;
            streamPtr->dictionary = dictEnd - streamPtr->dictSize;
        }
    }

    /* prefix mode : source data follows dictionary */
    if (dictEnd == (const BYTE*)source)
    {
        int result;
        if ((streamPtr->dictSize < LZ6_DICT_SIZE) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ6_compress_generic(LZ6_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, withPrefix64k, dictSmall, acceleration);
        else
            result = LZ6_compress_generic(LZ6_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, withPrefix64k, noDictIssue, acceleration);
        streamPtr->dictSize += (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }

    /* external dictionary mode */
    {
        int result;
        if ((streamPtr->dictSize < LZ6_DICT_SIZE) && (streamPtr->dictSize < streamPtr->currentOffset))
            result = LZ6_compress_generic(LZ6_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, usingExtDict, dictSmall, acceleration);
        else
            result = LZ6_compress_generic(LZ6_stream, source, dest, inputSize, maxOutputSize, limitedOutput, byU32, usingExtDict, noDictIssue, acceleration);
        streamPtr->dictionary = (const BYTE*)source;
        streamPtr->dictSize = (U32)inputSize;
        streamPtr->currentOffset += (U32)inputSize;
        return result;
    }
}


/* Hidden debug function, to force external dictionary mode */
int LZ6_compress_forceExtDict (LZ6_stream_t* LZ6_dict, const char* source, char* dest, int inputSize)
{
    LZ6_stream_t_internal* streamPtr = (LZ6_stream_t_internal*)LZ6_dict;
    int result;
    const BYTE* const dictEnd = streamPtr->dictionary + streamPtr->dictSize;

    const BYTE* smallest = dictEnd;
    if (smallest > (const BYTE*) source) smallest = (const BYTE*) source;
    LZ6_renormDictT((LZ6_stream_t_internal*)LZ6_dict, smallest);

    result = LZ6_compress_generic(LZ6_dict, source, dest, inputSize, 0, notLimited, byU32, usingExtDict, noDictIssue, 1);

    streamPtr->dictionary = (const BYTE*)source;
    streamPtr->dictSize = (U32)inputSize;
    streamPtr->currentOffset += (U32)inputSize;

    return result;
}


int LZ6_saveDict (LZ6_stream_t* LZ6_dict, char* safeBuffer, int dictSize)
{
    LZ6_stream_t_internal* dict = (LZ6_stream_t_internal*) LZ6_dict;
	const BYTE* previousDictEnd = dict->dictionary + dict->dictSize;
	if (!dict->dictionary)
        return 0;

    if ((U32)dictSize > LZ6_DICT_SIZE) dictSize = LZ6_DICT_SIZE;   /* useless to define a dictionary > LZ6_DICT_SIZE */
    if ((U32)dictSize > dict->dictSize) dictSize = dict->dictSize;

    memmove(safeBuffer, previousDictEnd - dictSize, dictSize);

    dict->dictionary = (const BYTE*)safeBuffer;
    dict->dictSize = (U32)dictSize;

    return dictSize;
}



/*******************************
*  Decompression functions
*******************************/
/*
 * This generic decompression function cover all use cases.
 * It shall be instantiated several times, using different sets of directives
 * Note that it is essential this generic function is really inlined,
 * in order to remove useless branches during compilation optimization.
 */
FORCE_INLINE int LZ6_decompress_generic(
                 const char* const source,
                 char* const dest,
                 int inputSize,
                 int outputSize,         /* If endOnInput==endOnInputSize, this value is the max size of Output Buffer. */

                 int endOnInput,         /* endOnOutputSize, endOnInputSize */
                 int partialDecoding,    /* full, partial */
                 int targetOutputSize,   /* only used if partialDecoding==partial */
                 int dict,               /* noDict, withPrefix64k, usingExtDict */
                 const BYTE* const lowPrefix,  /* == dest if dict == noDict */
                 const BYTE* const dictStart,  /* only if dict==usingExtDict */
                 const size_t dictSize         /* note : = 0 if noDict */
                 )
{
    /* Local Variables */
    const BYTE* ip = (const BYTE*) source;
    const BYTE* const iend = ip + inputSize;

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + outputSize;
    BYTE* cpy;
    BYTE* oexit = op + targetOutputSize;
    const BYTE* const lowLimit = lowPrefix - dictSize;

    const BYTE* const dictEnd = (const BYTE*)dictStart + dictSize;
    const unsigned dec32table[] = {4, 1, 2, 1, 4, 4, 4, 4};
    const int dec64table[] = {0, 0, 0, -1, 0, 1, 2, 3};

    const int safeDecode = (endOnInput==endOnInputSize);
    const int checkOffset = ((safeDecode) && (dictSize < (int)(LZ6_DICT_SIZE)));

    size_t last_off = 1;

    /* Special cases */
    if ((partialDecoding) && (oexit> oend-MFLIMIT)) oexit = oend-MFLIMIT;                         /* targetOutputSize too high => decode everything */
    if ((endOnInput) && (unlikely(outputSize==0))) return ((inputSize==1) && (*ip==0)) ? 0 : -1;  /* Empty output buffer */
    if ((!endOnInput) && (unlikely(outputSize==0))) return (*ip==0?1:-1);


    /* Main Loop */
    while (1)
    {
        unsigned token;
        size_t length;
        const BYTE* match;
        size_t offset;

        /* get literal length */
        token = *ip++;
        if (token>>6)
        {
            if ((length=(token>>ML_BITS)&RUN_MASK2) == RUN_MASK2)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    length += s;
                }
                while (likely((endOnInput)?ip<iend-RUN_MASK2:1) && (s==255));
                if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)(op))) goto _output_error;   /* overflow detection */
                if ((safeDecode) && unlikely((size_t)(ip+length)<(size_t)(ip))) goto _output_error;   /* overflow detection */
            }
        }
        else
        {
            if ((length=(token>>ML_BITS)&RUN_MASK) == RUN_MASK)
            {
                unsigned s;
                do
                {
                    s = *ip++;
                    length += s;
                }
                while (likely((endOnInput)?ip<iend-RUN_MASK:1) && (s==255));
                if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)(op))) goto _output_error;   /* overflow detection */
                if ((safeDecode) && unlikely((size_t)(ip+length)<(size_t)(ip))) goto _output_error;   /* overflow detection */
            }
        }

        /* copy literals */
        cpy = op+length;
        if (((endOnInput) && ((cpy>(partialDecoding?oexit:oend-WILDCOPYLENGTH)) || (ip+length>iend-(0+1+LASTLITERALS))) )
            || ((!endOnInput) && (cpy>oend-WILDCOPYLENGTH)))
        {
            if (partialDecoding)
            {
                if (cpy > oend) goto _output_error;                           /* Error : write attempt beyond end of output buffer */
                if ((endOnInput) && (ip+length > iend)) goto _output_error;   /* Error : read attempt beyond end of input buffer */
            }
            else
            {
                if ((!endOnInput) && (cpy != oend)) goto _output_error;       /* Error : block decoding must stop exactly there */
                if ((endOnInput) && ((ip+length != iend) || (cpy > oend))) goto _output_error;   /* Error : input must be consumed */
            }
            memcpy(op, ip, length);
            ip += length;
            op += length;
            break;     /* Necessarily EOF, due to parsing restrictions */
        }
        MEM_wildCopy(op, ip, cpy);
        ip += length; op = cpy;

        /* get offset */
#if 0
        switch (token>>6)
        {
            default: offset = *ip + (((token>>ML_RUN_BITS2)&3)<<8); ip++; break;
            case 0: offset = MEM_readLE16(ip); ip+=2; break;
            case 1:
                if ((token>>5) == 3)
                    offset = last_off;
                else // (token>>ML_RUN_BITS2) == 2
                {    offset = MEM_readLE24(ip); ip+=3; }
                break;
        }
#else 
        if (token>>7)
        {
            offset = *ip + (((token>>ML_RUN_BITS2)&3)<<8); ip++;
        }
        else 
        if ((token>>ML_RUN_BITS) == 0)
        {
            offset = MEM_readLE16(ip); ip+=2;
        }
        else
        if ((token>>ML_RUN_BITS2) == 2)
        {
            offset = MEM_readLE24(ip); ip+=3;
        }
        else // (token>>ML_RUN_BITS2) == 3
        {
            offset = last_off;
        }
#endif

        last_off = offset;
        match = op - offset;
        if ((checkOffset) && (unlikely(match < lowLimit))) goto _output_error;   /* Error : offset outside buffers */

        /* get matchlength */
        length = token & ML_MASK;
        if (length == ML_MASK)
        {
            unsigned s;
            do
            {
                if ((endOnInput) && (ip > iend-LASTLITERALS)) goto _output_error;
                s = *ip++;
                length += s;
            } while (s==255);
            if ((safeDecode) && unlikely((size_t)(op+length)<(size_t)op)) goto _output_error;   /* overflow detection */
        }
        length += MINMATCH;

        /* check external dictionary */
        if ((dict==usingExtDict) && (match < lowPrefix))
        {
            if (unlikely(op+length > oend-LASTLITERALS)) goto _output_error;   /* doesn't respect parsing restriction */

            if (length <= (size_t)(lowPrefix-match))
            {
                /* match can be copied as a single segment from external dictionary */
                match = dictEnd - (lowPrefix-match);
                memmove(op, match, length); op += length;
            }
            else
            {
                /* match encompass external dictionary and current block */
                size_t copySize = (size_t)(lowPrefix-match);
                memcpy(op, dictEnd - copySize, copySize);
                op += copySize;
                copySize = length - copySize;
                if (copySize > (size_t)(op-lowPrefix))   /* overlap copy */
                {
                    BYTE* const endOfMatch = op + copySize;
                    const BYTE* copyFrom = lowPrefix;
                    while (op < endOfMatch) *op++ = *copyFrom++;
                }
                else
                {
                    memcpy(op, lowPrefix, copySize);
                    op += copySize;
                }
            }
            continue;
        }

        /* copy match within block */
        cpy = op + length;
        if (unlikely(offset<8))
        {
            const int dec64 = dec64table[offset];
            op[0] = match[0];
            op[1] = match[1];
            op[2] = match[2];
            op[3] = match[3];
            match += dec32table[offset];
            memcpy(op+4, match, 4);
            match -= dec64;
        } else { MEM_copy8(op, match); match+=8; }
        op += 8;

        if (unlikely(cpy>oend-(16-MINMATCH)))
        {
            BYTE* const oCopyLimit = oend-(WILDCOPYLENGTH-1);
            if (cpy > oend-LASTLITERALS) goto _output_error;    /* Error : last LASTLITERALS bytes must be literals (uncompressed) */
            if (op < oCopyLimit)
            {
                MEM_wildCopy(op, match, oCopyLimit);
                match += oCopyLimit - op;
                op = oCopyLimit;
            }
            while (op<cpy) *op++ = *match++;
        }
        else
            MEM_wildCopy(op, match, cpy);
        op=cpy;   /* correction */
    }

    /* end of decoding */
    if (endOnInput)
       return (int) (((char*)op)-dest);     /* Nb of output bytes decoded */
    else
       return (int) (((const char*)ip)-source);   /* Nb of input bytes read */

    /* Overflow error detected */
_output_error:
    return (int) (-(((const char*)ip)-source))-1;
}


int LZ6_decompress_safe(const char* source, char* dest, int compressedSize, int maxDecompressedSize)
{
    return LZ6_decompress_generic(source, dest, compressedSize, maxDecompressedSize, endOnInputSize, full, 0, noDict, (BYTE*)dest, NULL, 0);
}

int LZ6_decompress_safe_partial(const char* source, char* dest, int compressedSize, int targetOutputSize, int maxDecompressedSize)
{
    return LZ6_decompress_generic(source, dest, compressedSize, maxDecompressedSize, endOnInputSize, partial, targetOutputSize, noDict, (BYTE*)dest, NULL, 0);
}

int LZ6_decompress_fast(const char* source, char* dest, int originalSize)
{
    return LZ6_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, withPrefix64k, (BYTE*)(dest - LZ6_DICT_SIZE), NULL, LZ6_DICT_SIZE);
}


/* streaming decompression functions */

typedef struct
{
    const BYTE* externalDict;
    size_t extDictSize;
    const BYTE* prefixEnd;
    size_t prefixSize;
} LZ6_streamDecode_t_internal;

/*
 * If you prefer dynamic allocation methods,
 * LZ6_createStreamDecode()
 * provides a pointer (void*) towards an initialized LZ6_streamDecode_t structure.
 */
LZ6_streamDecode_t* LZ6_createStreamDecode(void)
{
    LZ6_streamDecode_t* lz6s = (LZ6_streamDecode_t*) ALLOCATOR(1, sizeof(LZ6_streamDecode_t));
    return lz6s;
}

int LZ6_freeStreamDecode (LZ6_streamDecode_t* LZ6_stream)
{
    FREEMEM(LZ6_stream);
    return 0;
}

/*
 * LZ6_setStreamDecode
 * Use this function to instruct where to find the dictionary
 * This function is not necessary if previous data is still available where it was decoded.
 * Loading a size of 0 is allowed (same effect as no dictionary).
 * Return : 1 if OK, 0 if error
 */
int LZ6_setStreamDecode (LZ6_streamDecode_t* LZ6_streamDecode, const char* dictionary, int dictSize)
{
    LZ6_streamDecode_t_internal* lz6sd = (LZ6_streamDecode_t_internal*) LZ6_streamDecode;
    lz6sd->prefixSize = (size_t) dictSize;
    lz6sd->prefixEnd = (const BYTE*) dictionary + dictSize;
    lz6sd->externalDict = NULL;
    lz6sd->extDictSize  = 0;
    return 1;
}

/*
*_continue() :
    These decoding functions allow decompression of multiple blocks in "streaming" mode.
    Previously decoded blocks must still be available at the memory position where they were decoded.
    If it's not possible, save the relevant part of decoded data into a safe buffer,
    and indicate where it stands using LZ6_setStreamDecode()
*/
int LZ6_decompress_safe_continue (LZ6_streamDecode_t* LZ6_streamDecode, const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    LZ6_streamDecode_t_internal* lz6sd = (LZ6_streamDecode_t_internal*) LZ6_streamDecode;
    int result;

    if (lz6sd->prefixEnd == (BYTE*)dest)
    {
        result = LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize,
                                        endOnInputSize, full, 0,
                                        usingExtDict, lz6sd->prefixEnd - lz6sd->prefixSize, lz6sd->externalDict, lz6sd->extDictSize);
        if (result <= 0) return result;
        lz6sd->prefixSize += result;
        lz6sd->prefixEnd  += result;
    }
    else
    {
        lz6sd->extDictSize = lz6sd->prefixSize;
        lz6sd->externalDict = lz6sd->prefixEnd - lz6sd->extDictSize;
        result = LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize,
                                        endOnInputSize, full, 0,
                                        usingExtDict, (BYTE*)dest, lz6sd->externalDict, lz6sd->extDictSize);
        if (result <= 0) return result;
        lz6sd->prefixSize = result;
        lz6sd->prefixEnd  = (BYTE*)dest + result;
    }

    return result;
}

int LZ6_decompress_fast_continue (LZ6_streamDecode_t* LZ6_streamDecode, const char* source, char* dest, int originalSize)
{
    LZ6_streamDecode_t_internal* lz6sd = (LZ6_streamDecode_t_internal*) LZ6_streamDecode;
    int result;

    if (lz6sd->prefixEnd == (BYTE*)dest)
    {
        result = LZ6_decompress_generic(source, dest, 0, originalSize,
                                        endOnOutputSize, full, 0,
                                        usingExtDict, lz6sd->prefixEnd - lz6sd->prefixSize, lz6sd->externalDict, lz6sd->extDictSize);
        if (result <= 0) return result;
        lz6sd->prefixSize += originalSize;
        lz6sd->prefixEnd  += originalSize;
    }
    else
    {
        lz6sd->extDictSize = lz6sd->prefixSize;
        lz6sd->externalDict = (BYTE*)dest - lz6sd->extDictSize;
        result = LZ6_decompress_generic(source, dest, 0, originalSize,
                                        endOnOutputSize, full, 0,
                                        usingExtDict, (BYTE*)dest, lz6sd->externalDict, lz6sd->extDictSize);
        if (result <= 0) return result;
        lz6sd->prefixSize = originalSize;
        lz6sd->prefixEnd  = (BYTE*)dest + originalSize;
    }

    return result;
}


/*
Advanced decoding functions :
*_usingDict() :
    These decoding functions work the same as "_continue" ones,
    the dictionary must be explicitly provided within parameters
*/

FORCE_INLINE int LZ6_decompress_usingDict_generic(const char* source, char* dest, int compressedSize, int maxOutputSize, int safe, const char* dictStart, int dictSize)
{
    if (dictSize==0)
        return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, noDict, (BYTE*)dest, NULL, 0);
    if (dictStart+dictSize == dest)
    {
        if (dictSize >= (int)(LZ6_DICT_SIZE - 1))
            return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, withPrefix64k, (BYTE*)dest-LZ6_DICT_SIZE, NULL, 0);
        return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, noDict, (BYTE*)dest-dictSize, NULL, 0);
    }
    return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, safe, full, 0, usingExtDict, (BYTE*)dest, (const BYTE*)dictStart, dictSize);
}

int LZ6_decompress_safe_usingDict(const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize)
{
    return LZ6_decompress_usingDict_generic(source, dest, compressedSize, maxOutputSize, 1, dictStart, dictSize);
}

int LZ6_decompress_fast_usingDict(const char* source, char* dest, int originalSize, const char* dictStart, int dictSize)
{
    return LZ6_decompress_usingDict_generic(source, dest, 0, originalSize, 0, dictStart, dictSize);
}

/* debug function */
int LZ6_decompress_safe_forceExtDict(const char* source, char* dest, int compressedSize, int maxOutputSize, const char* dictStart, int dictSize)
{
    return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, usingExtDict, (BYTE*)dest, (const BYTE*)dictStart, dictSize);
}


/***************************************************
*  Obsolete Functions
***************************************************/
/* obsolete compression functions */
int LZ6_compress_limitedOutput(const char* source, char* dest, int inputSize, int maxOutputSize) { return LZ6_compress_default(source, dest, inputSize, maxOutputSize); }
int LZ6_compress(const char* source, char* dest, int inputSize) { return LZ6_compress_default(source, dest, inputSize, LZ6_compressBound(inputSize)); }
int LZ6_compress_limitedOutput_withState (void* state, const char* src, char* dst, int srcSize, int dstSize) { return LZ6_compress_fast_extState(state, src, dst, srcSize, dstSize, 1); }
int LZ6_compress_withState (void* state, const char* src, char* dst, int srcSize) { return LZ6_compress_fast_extState(state, src, dst, srcSize, LZ6_compressBound(srcSize), 1); }
int LZ6_compress_limitedOutput_continue (LZ6_stream_t* LZ6_stream, const char* src, char* dst, int srcSize, int maxDstSize) { return LZ6_compress_fast_continue(LZ6_stream, src, dst, srcSize, maxDstSize, 1); }
int LZ6_compress_continue (LZ6_stream_t* LZ6_stream, const char* source, char* dest, int inputSize) { return LZ6_compress_fast_continue(LZ6_stream, source, dest, inputSize, LZ6_compressBound(inputSize), 1); }

/*
These function names are deprecated and should no longer be used.
They are only provided here for compatibility with older user programs.
- LZ6_uncompress is totally equivalent to LZ6_decompress_fast
- LZ6_uncompress_unknownOutputSize is totally equivalent to LZ6_decompress_safe
*/
int LZ6_uncompress (const char* source, char* dest, int outputSize) { return LZ6_decompress_fast(source, dest, outputSize); }
int LZ6_uncompress_unknownOutputSize (const char* source, char* dest, int isize, int maxOutputSize) { return LZ6_decompress_safe(source, dest, isize, maxOutputSize); }


/* Obsolete Streaming functions */

int LZ6_sizeofStreamState() { return LZ6_STREAMSIZE; }

static void LZ6_init(LZ6_stream_t_internal* lz6ds, BYTE* base)
{
    MEM_INIT(lz6ds, 0, LZ6_STREAMSIZE);
    lz6ds->bufferStart = base;
}

int LZ6_resetStreamState(void* state, char* inputBuffer)
{
    if ((((size_t)state) & 3) != 0) return 1;   /* Error : pointer is not aligned on 4-bytes boundary */
    LZ6_init((LZ6_stream_t_internal*)state, (BYTE*)inputBuffer);
    return 0;
}

void* LZ6_create (char* inputBuffer)
{
    void* lz6ds = ALLOCATOR(8, LZ6_STREAMSIZE_U64);
    LZ6_init ((LZ6_stream_t_internal*)lz6ds, (BYTE*)inputBuffer);
    return lz6ds;
}

char* LZ6_slideInputBuffer (void* LZ6_Data)
{
    LZ6_stream_t_internal* ctx = (LZ6_stream_t_internal*)LZ6_Data;
    int dictSize = LZ6_saveDict((LZ6_stream_t*)LZ6_Data, (char*)ctx->bufferStart, LZ6_DICT_SIZE);
    return (char*)(ctx->bufferStart + dictSize);
}

/* Obsolete streaming decompression functions */

int LZ6_decompress_safe_withPrefix64k(const char* source, char* dest, int compressedSize, int maxOutputSize)
{
    return LZ6_decompress_generic(source, dest, compressedSize, maxOutputSize, endOnInputSize, full, 0, withPrefix64k, (BYTE*)dest - LZ6_DICT_SIZE, NULL, LZ6_DICT_SIZE);
}

int LZ6_decompress_fast_withPrefix64k(const char* source, char* dest, int originalSize)
{
    return LZ6_decompress_generic(source, dest, 0, originalSize, endOnOutputSize, full, 0, withPrefix64k, (BYTE*)dest - LZ6_DICT_SIZE, NULL, LZ6_DICT_SIZE);
}
