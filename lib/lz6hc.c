/*
    LZ6 HC - High Compression Mode of LZ6
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




/* *************************************
*  Includes
***************************************/
#define LZ6HC_INCLUDES
/* lz6hc.h first — defines LZ6HC_match_t (the public type used
   internally by LZ6HC_match_t[…] buffers). lz6common.h includes
   the same typedef under a guard so the redeclaration is silent. */
#include "lz6hc.h"
#include "lz6common.h"
#include "lz6.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Software prefetch hint. A no-op where unsupported; prefetching a wild address
   is harmless (never faults), so it needs no bounds check. */
#if defined(__GNUC__) || defined(__clang__)
#  define LZ6_PREFETCH(p)  __builtin_prefetch((const void*)(p))
#else
#  define LZ6_PREFETCH(p)  ((void)(p))
#endif


/**************************************
*  HC Compression
**************************************/


/* smallest L such that (1<<L) >= n, floored at a small minimum window */
static U32 LZ6HC_ceilLog2(size_t n)
{
    U32 L = 10;                       /* 1 KB floor: never allocate a degenerate window */
    while (((size_t)1 << L) < n) L++;
    return L;
}

/* Adapt the window (windowLog) and its chain table (contentLog) to the largest
   block this context will ever compress. windowLog defaults to the MAXD_LOG
   ceiling; shrinking it for smaller inputs keeps the chain-table allocation and
   the effective window from exceeding what the data can use. The hash table
   (hashLog) is independent of the window and is left untouched.
   NOTE: windowLog is fixed for the lifetime of a context (cross-block match
   offsets depend on it), so this must be applied once, at allocation, sized to
   the maximum block — never per-block. */
static void LZ6HC_capParamsToSize(LZ6HC_parameters* p, size_t maxSrcSize)
{
    U32 sizeLog = LZ6HC_ceilLog2(maxSrcSize);
    if (sizeLog < p->windowLog)
    {
        U32 delta = p->contentLog - p->windowLog;   /* 0 or 1 in the default table */
        p->windowLog  = sizeLog;
        p->contentLog = sizeLog + delta;
    }
}

static int LZ6_alloc_mem_HC_wl(LZ6HC_Data_Structure* ctx, int compressionLevel,
                               size_t maxSrcSize, int maxWindowLog);

/* HC with the window capped at 2^windowLog (offsets <= 2^windowLog - 1):
 * same block format, for decoders with a fixed memory budget */
int LZ6_compress_HC_window(const char* src, char* dst, int srcSize, int maxDstSize,
                           int compressionLevel, int windowLog)
{
    LZ6HC_Data_Structure state;
    int cSize;
    if (windowLog < 10 || windowLog > MAXD_LOG) return 0;
    if (!LZ6_alloc_mem_HC_wl(&state, compressionLevel, (size_t)(srcSize > 0 ? srcSize : 1), -windowLog))
        return 0;
    cSize = LZ6_compress_HC_extStateHC(&state, src, dst, srcSize, maxDstSize);
    LZ6_free_mem_HC(&state);
    return cSize;
}

/* maxWindowLog < 0 caps the window at -maxWindowLog instead of raising it */
static int LZ6_alloc_mem_HC_wl(LZ6HC_Data_Structure* ctx, int compressionLevel,
                               size_t maxSrcSize, int maxWindowLog)
{
    ctx->compressionLevel = compressionLevel;
    ctx->seqPrice = NULL;
    const int preparse = (compressionLevel == LZ6HC_SEQ_PRE_LEVEL && maxWindowLog >= 25);
    if (compressionLevel > g_maxCompressionLevel) ctx->compressionLevel = g_maxCompressionLevel;
    if (compressionLevel < 1) ctx->compressionLevel = LZ6HC_compressionLevel_default;

    /* maxWindowLog >= 25 is the seq-codec allocation (LZ6_alloc_mem_HC_seq) */
    ctx->params = (maxWindowLog >= 25 ? LZ6HC_seqParameters : LZ6HC_defaultParameters)[ctx->compressionLevel];
    if (preparse) { ctx->params = LZ6HC_seqPreParameters; ctx->compressionLevel = 0; }
#ifdef LZ6_SEQ_TUNING
    /* experiment hook: LZ6_SEQPARAMS_LEVEL=L LZ6_SEQPARAMS=H:H3:SN:SL:SUF:FS:STRAT */
    if (maxWindowLog >= 25 && getenv("LZ6_SEQPARAMS") && getenv("LZ6_SEQPARAMS_LEVEL")
        && atoi(getenv("LZ6_SEQPARAMS_LEVEL")) == ctx->compressionLevel) {
        unsigned h, h3, sn, sl, suf, fs, st;
        if (sscanf(getenv("LZ6_SEQPARAMS"), "%u:%u:%u:%u:%u:%u:%u", &h, &h3, &sn, &sl, &suf, &fs, &st) == 7) {
            ctx->params.hashLog = h; ctx->params.hashLog3 = h3; ctx->params.searchNum = sn;
            ctx->params.searchLength = sl; ctx->params.sufficientLength = suf;
            ctx->params.fullSearch = fs; ctx->params.strategy = (LZ6HC_strategy)st;
            /* the chain strategies insert every position into hashTable3:
             * a 0-bit table would shift by 32 (every real table row has >= 13) */
            if (ctx->params.strategy >= LZ6HC_price_fast && ctx->params.strategy != LZ6HC_row
                && ctx->params.hashLog3 < 8) ctx->params.hashLog3 = 8;
        }
    }
#endif
    /* seq codec: the format carries offsets up to 2^25-1 (bucket 24), so a
     * bigger window is allowed there without touching the frame codec */
    if (maxWindowLog > (int)ctx->params.windowLog)
    {
        U32 delta = ctx->params.contentLog - ctx->params.windowLog;
        ctx->params.windowLog  = (U32)maxWindowLog;
        ctx->params.contentLog = (U32)maxWindowLog + delta;
    }
    if (maxWindowLog < 0 && (U32)(-maxWindowLog) < ctx->params.windowLog)
    {
        U32 delta = ctx->params.contentLog - ctx->params.windowLog;
        ctx->params.windowLog  = (U32)(-maxWindowLog);
        ctx->params.contentLog = (U32)(-maxWindowLog) + delta;
    }
    LZ6HC_capParamsToSize(&ctx->params, maxSrcSize);

    /* density-aware hash widening (seq only, chain strategies only):
     * shallow searches (fast/price_fast/lowest_price) pick the first few
     * chain candidates, so they degrade when hash buckets saturate on
     * large inputs — L2's hashLog 13 leaves ~12k positions per bucket at
     * 100MB (widening 13->23+ recovers 2.4MB / 2.3pts on a 100MB Silesia
     * slice). Scale the main hash with the input size (density <= ~16),
     * capped at 2^23 (32MB table). Density 16 rather than 2: the table
     * is random-access, and 8x smaller made L1-3 24-28% and L4-5 ~15%
     * faster for +0.1pt on Silesia. The BT strategies (L8-15) search
     * deeper and measured WORSE with a widened table; the frame codec
     * (maxWindowLog 24) stays byte-identical. */
    if (maxWindowLog >= 25 &&
        ctx->params.strategy <= LZ6HC_lowest_price) {
        U32 sizeLog = LZ6HC_ceilLog2(maxSrcSize);
        U32 widened = (sizeLog > 26 ? 26 : sizeLog > 3 ? sizeLog : 3) - 3;
        if (widened > ctx->params.hashLog) ctx->params.hashLog = widened;
    }

    ctx->hashTable = (U32*) malloc(sizeof(U32)*(((size_t)1 << ctx->params.hashLog3)+((size_t)1 << ctx->params.hashLog)));
    if (!ctx->hashTable)
        return 0;

    ctx->hashTable3 = ctx->hashTable + ((size_t)1 << ctx->params.hashLog);

    ctx->chainTable = (U32*) malloc(sizeof(U32)*((size_t)1 << ctx->params.contentLog));
    if (!ctx->chainTable)
    {
        FREEMEM(ctx->hashTable);
        ctx->hashTable = NULL;
        return 0;
    }

    return 1;
}

int LZ6_alloc_mem_HC_sized(LZ6HC_Data_Structure* ctx, int compressionLevel, size_t maxSrcSize)
{
    return LZ6_alloc_mem_HC_wl(ctx, compressionLevel, maxSrcSize, MAXD_LOG);
}

/* Seq-codec entry: permits a 32MB window (2^25-1 offsets) when the input
 * is large enough to need it; capParamsToSize still shrinks it back down
 * to ceil-log2(srcSize) for smaller inputs. */
int LZ6_alloc_mem_HC_seq(LZ6HC_Data_Structure* ctx, int compressionLevel, size_t maxSrcSize)
{
    return LZ6_alloc_mem_HC_wl(ctx, compressionLevel, maxSrcSize, MAXD_LOG + 1);
}

/* Back-compat entry: allocate with the full MAXD_LOG window (no size cap). */
int LZ6_alloc_mem_HC(LZ6HC_Data_Structure* ctx, int compressionLevel)
{
    return LZ6_alloc_mem_HC_sized(ctx, compressionLevel, (size_t)1 << MAXD_LOG);
}

void LZ6_free_mem_HC(LZ6HC_Data_Structure* ctx)
{
    if (!ctx) return;
    if (ctx->chainTable) FREEMEM(ctx->chainTable);
    if (ctx->hashTable) FREEMEM(ctx->hashTable);    
    ctx->base = NULL;
}

/* Zero the hash/chain tables after alloc. LZ6HC_init only does this under
 * LZ6_RESET_MEM (a debug define), and the fast parser reads a table entry
 * before its first insert, so uninitialized tables make compression
 * non-deterministic. One-shot compressors should call this once after
 * LZ6_alloc_mem_HC_sized. */
void LZ6HC_reset_mem(LZ6HC_Data_Structure* ctx)
{
    if (!ctx) return;
    MEM_INIT(ctx->hashTable, 0, sizeof(U32) * (((size_t)1 << ctx->params.hashLog3) + ((size_t)1 << ctx->params.hashLog)));
    /* the fast / price_fast strategies never read the chain table: leave
     * it untouched (a large unwritten allocation costs no page faults;
     * zeroing it was ~20% of level-2 encode time on 10-50 MB inputs) */
    if (ctx->params.strategy >= LZ6HC_lowest_price && ctx->params.strategy != LZ6HC_row)
        MEM_INIT(ctx->chainTable, 0, sizeof(U32) * ((size_t)1 << ctx->params.contentLog));
}

static void LZ6HC_init (LZ6HC_Data_Structure* ctx, const BYTE* start)
{
#ifdef LZ6_RESET_MEM
    MEM_INIT((void*)ctx->hashTable, 0, sizeof(U32)*((1 << ctx->params.hashLog) + (1 << ctx->params.hashLog3)));
    if (ctx->params.strategy >= LZ6HC_lowest_price)
        MEM_INIT(ctx->chainTable, 0x01, sizeof(U32)*(1 << ctx->params.contentLog));
#else
#ifdef _DEBUG
	int i, len = sizeof(U32)*((1 << ctx->params.hashLog) + (1 << ctx->params.hashLog3));
    unsigned char* bytes = (unsigned char*)ctx->hashTable;
	srand(0);
    for (i=0; i<len; i++)
		bytes[i] = (unsigned char)rand();
#endif
#endif

    ctx->nextToUpdate = (U32)((size_t)1 << ctx->params.windowLog);
    ctx->base = start - ((size_t)1 << ctx->params.windowLog);
    ctx->end = start;
    ctx->dictBase = start - ((size_t)1 << ctx->params.windowLog);
    ctx->dictLimit = (U32)((size_t)1 << ctx->params.windowLog);
    ctx->lowLimit = (U32)((size_t)1 << ctx->params.windowLog);
    ctx->last_off = 1;
    ctx->rep_off2 = 0;
    ctx->rep_off3 = 0;
    ctx->emitSeq    = NULL;   /* default: write codeword; LZ6HC_compress_sequences wires this */
    ctx->seqPrice   = NULL;   /* default: codeword-byte prices */
    ctx->emitOpaque = NULL;
}


/* Update chains up to ip (excluded) */
FORCE_INLINE void LZ6HC_BinTree_Insert(LZ6HC_Data_Structure* ctx, const BYTE* ip)
{
#if MINMATCH == 3
    U32* HashTable3  = ctx->hashTable3;
    const BYTE* const base = ctx->base;
    const U32 target = (U32)(ip - base);
    U32 idx = ctx->nextToUpdate;
    
    while(idx < target)
    {
        HashTable3[LZ6HC_hash3Ptr(base+idx, ctx->params.hashLog3)] = idx;
        idx++;
    }

    ctx->nextToUpdate = target;
#endif 
}


/* Update chains up to "end" (excluded) */
FORCE_INLINE void LZ6HC_BinTree_InsertFull(LZ6HC_Data_Structure* ctx, const BYTE* end, const BYTE* iHighLimit)
{
    U32* chainTable = ctx->chainTable;
    U32* HashTable  = ctx->hashTable;
#if MINMATCH == 3
    U32* HashTable3  = ctx->hashTable3;
#endif 

    U32 idx = ctx->nextToUpdate;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);
    const U32 current = (U32)(end - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > idx) ? ctx->lowLimit : idx - (maxDistance - 1);
    const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match, *ip;
    int nbAttempts;
    U32 *ptr0, *ptr1, *HashPos;
    U32 matchIndex, delta0, delta1;
	size_t mlt;

    
    while(idx < current)
    {
        ip = base + idx;
        if (ip + MINMATCH > iHighLimit) return;

        HashPos = &HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];
        matchIndex = *HashPos;
#if MINMATCH == 3
        HashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)] = idx;
#endif 

        // check rest of matches
        ptr0 = &chainTable[(idx*2+1) & contentMask];
        ptr1 = &chainTable[(idx*2) & contentMask];
        delta0 = delta1 = idx - matchIndex;
        nbAttempts = ctx->params.searchNum;
        *HashPos = idx;

  //      while ((matchIndex >= dictLimit) && (matchIndex < idx) && (idx - matchIndex) < MAX_DISTANCE && nbAttempts)
        while ((matchIndex < current) && (matchIndex < idx) && (matchIndex>=lowLimit) && (nbAttempts))
        {
            nbAttempts--;
            mlt = 0;
            if (matchIndex >= dictLimit)
            {
                match = base + matchIndex;
                if (MEM_read24(match) == MEM_read24(ip))
                {
                    mlt = MINMATCH + MEM_count(ip+MINMATCH, match+MINMATCH, iHighLimit);

                    if (mlt > LZ6_OPT_NUM) break;
                }
            }
            else
            {
                match = dictBase + matchIndex;
                if (MEM_read32(match) == MEM_read32(ip))
                {
                    const BYTE* vLimit = ip + (dictLimit - matchIndex);
                    if (vLimit > iHighLimit) vLimit = iHighLimit;
                    mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                    if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                        mlt += MEM_count(ip+mlt, base+dictLimit, iHighLimit);

                    if (mlt > LZ6_OPT_NUM) break;
                }
            }
            
            if (*(ip+mlt) < *(match+mlt))
            {
                *ptr0 = delta0;
                ptr0 = &chainTable[(matchIndex*2) & contentMask];
                if (*ptr0 == (U32)-1) break;
                delta0 = *ptr0;
                delta1 += delta0;
                matchIndex -= delta0;
            }
            else
            {
                *ptr1 = delta1;
                ptr1 = &chainTable[(matchIndex*2+1) & contentMask];
                if (*ptr1 == (U32)-1) break;
                delta1 = *ptr1;
                delta0 += delta1;
                matchIndex -= delta1;
            }
        }

        *ptr0 = (U32)-1;
        *ptr1 = (U32)-1;

    //    LZ6_LOG_MATCH("%d: LZMAX_UPDATE_HASH_BINTREE hash=%d inp=%d,%d,%d,%d (%c%c%c%c)\n", (int)(inp-base), hash, inp[0], inp[1], inp[2], inp[3], inp[0], inp[1], inp[2], inp[3]);

        idx++;
    }

    ctx->nextToUpdate = current;
}


/* Update chains up to ip (excluded) */
FORCE_INLINE void LZ6HC_Insert (LZ6HC_Data_Structure* ctx, const BYTE* ip)
{
    U32* chainTable = ctx->chainTable;
    U32* HashTable  = ctx->hashTable;
#if MINMATCH == 3
    U32* HashTable3  = ctx->hashTable3;
#endif 
    const BYTE* const base = ctx->base;
    const U32 target = (U32)(ip - base);
    const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    U32 idx = ctx->nextToUpdate;

    while(idx < target)
    {
        size_t h = LZ6HC_hashPtr(base+idx, ctx->params.hashLog, ctx->params.searchLength);
        chainTable[idx & contentMask] = (U32)(idx - HashTable[h]);
//        if (chainTable[idx & contentMask] == 1) chainTable[idx & contentMask] = (U32)0x01010101;
        HashTable[h] = idx;
#if MINMATCH == 3
        HashTable3[LZ6HC_hash3Ptr(base+idx, ctx->params.hashLog3)] = idx;
#endif 
       idx++;
    }

    ctx->nextToUpdate = target;
}

    
FORCE_INLINE int LZ6HC_FindBestMatch (LZ6HC_Data_Structure* ctx,   /* Index table will be updated */
                                               const BYTE* ip, const BYTE* const iLimit,
                                               const BYTE** matchpos)
{
    U32* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    const BYTE* const base = ctx->base;
    const BYTE* const dictBase = ctx->dictBase;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);     
	const U32 current = (U32)(ip - base);
	const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
	const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    U32 matchIndex;
    const BYTE* match;
    int nbAttempts=ctx->params.searchNum;
    size_t ml=0, mlt;

    matchIndex = HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];

    match = ip - ctx->last_off;
    if (MEM_read24(match) == MEM_read24(ip))
    {
        ml = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
        *matchpos = match;
        return (int)ml;
    }

#if MINMATCH == 3
	{
		U32 matchIndex3 = ctx->hashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)];
		if (matchIndex3 < current && matchIndex3 >= lowLimit)
		{
			size_t offset = (size_t)current - matchIndex3;
			if (offset < LZ6_SHORT_OFFSET_DISTANCE)
			{
				match = ip - offset;
				if (match > base && MEM_read24(ip) == MEM_read24(match))
				{
					ml = 3;//MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
					*matchpos = match;
				}
			}
		}
	}
#endif
    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        /* Issue the chain-hop load now so its latency overlaps the candidate
           check below, and prefetch the next candidate's bytes. matchIndex only
           decreases along the chain, so this visits exactly the same indices as
           the tail-hop form => byte-identical output. (Port from
           LZ6HC_GetAllMatches, commit e29d2a7 — same trick, didn't apply here
           at that time because LZ6HC_FindBestMatch wasn't the bottleneck.) */
        U32 nextIndex = matchIndex - chainTable[matchIndex & contentMask];
        LZ6_PREFETCH(base + nextIndex);
        nbAttempts--;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;
            if (*(match+ml) == *(ip+ml) && (MEM_read32(match) == MEM_read32(ip)))
            {
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
				if (!ml || (mlt > ml && LZ6HC_better_price((ip - *matchpos), ml, (ip - match), mlt, ctx->last_off)))
//                if (mlt > ml && (LZ6_NORMAL_MATCH_COST(mlt - MINMATCH, (ip - match == ctx->last_off) ? 0 : (ip - match)) < LZ6_NORMAL_MATCH_COST(ml - MINMATCH, (ip - *matchpos == ctx->last_off) ? 0 : (ip - *matchpos)) + (LZ6_NORMAL_LIT_COST(mlt - ml))))
                { ml = mlt; *matchpos = match; }
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iLimit) vLimit = iLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iLimit);
                if (!ml || (mlt > ml && LZ6HC_better_price((ip - *matchpos), ml, (ip - match), mlt, ctx->last_off)))
             //   if (mlt > ml && (LZ6_NORMAL_MATCH_COST(mlt - MINMATCH, (ip - match == ctx->last_off) ? 0 : (ip - match)) < LZ6_NORMAL_MATCH_COST(ml - MINMATCH, (ip - *matchpos == ctx->last_off) ? 0 : (ip - *matchpos)) + (LZ6_NORMAL_LIT_COST(mlt - ml))))
                { ml = mlt; *matchpos = base + matchIndex; }   /* virtual matchpos */
            }
        }
        matchIndex = nextIndex;
    }

    return (int)ml;
}


FORCE_INLINE int LZ6HC_FindMatchFast (LZ6HC_Data_Structure* ctx, U32 matchIndex, U32 matchIndex3, /* Index table will be updated */
                                               const BYTE* ip, const BYTE* const iLimit,
                                               const BYTE** matchpos)
{
    const BYTE* const base = ctx->base;
    const BYTE* const dictBase = ctx->dictBase;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);     
	const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const BYTE* match;
    size_t ml=0, mlt;

    match = ip - ctx->last_off;
    if (MEM_read24(match) == MEM_read24(ip))
    {
        ml = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
        *matchpos = match;
        return (int)ml;
    }

#if MINMATCH == 3
	if (matchIndex3 < current && matchIndex3 >= lowLimit)
	{
		size_t offset = (size_t)current - matchIndex3;
		if (offset < LZ6_SHORT_OFFSET_DISTANCE)
		{
			match = ip - offset;
			if (match > base && MEM_read24(ip) == MEM_read24(match))
			{
				ml = 3;//MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
				*matchpos = match;
			}
		}
	}
#endif

    if ((matchIndex < current) && (matchIndex>=lowLimit))
    {
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;
            if (*(match+ml) == *(ip+ml) && (MEM_read32(match) == MEM_read32(ip)))
            {
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
                if (!ml || (mlt > ml && LZ6HC_better_price((ip - *matchpos), ml, (ip - match), mlt, ctx->last_off)))
         //       if (ml==0 || ((mlt > ml) && LZ6_NORMAL_MATCH_COST(mlt - MINMATCH, (ip - match == ctx->last_off) ? 0 : (ip - match)) < LZ6_NORMAL_MATCH_COST(ml - MINMATCH, (ip - *matchpos == ctx->last_off) ? 0 : (ip - *matchpos)) + (LZ6_NORMAL_LIT_COST(mlt - ml))))
                { ml = mlt; *matchpos = match; }
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iLimit) vLimit = iLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iLimit);
                if (!ml || (mlt > ml && LZ6HC_better_price((ip - *matchpos), ml, (ip - match), mlt, ctx->last_off)))
//                if (ml==0 || ((mlt > ml) && LZ6_NORMAL_MATCH_COST(mlt - MINMATCH, (ip - match == ctx->last_off) ? 0 : (ip - match)) < LZ6_NORMAL_MATCH_COST(ml - MINMATCH, (ip - *matchpos == ctx->last_off) ? 0 : (ip - *matchpos)) + (LZ6_NORMAL_LIT_COST(mlt - ml))))
                { ml = mlt; *matchpos = base + matchIndex; }   /* virtual matchpos */
            }
        }
    }
    
    return (int)ml;
}


FORCE_INLINE int LZ6HC_FindMatchFaster (LZ6HC_Data_Structure* ctx, U32 matchIndex,  /* Index table will be updated */
                                               const BYTE* ip, const BYTE* const iLimit,
                                               const BYTE** matchpos)
{
    const BYTE* const base = ctx->base;
    const BYTE* const dictBase = ctx->dictBase;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);     
	const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const BYTE* match;
    size_t ml=0, mlt;

    match = ip - ctx->last_off;
    if (MEM_read24(match) == MEM_read24(ip))
    {
        ml = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
        *matchpos = match;
        return (int)ml;
    }

	if (matchIndex < current && matchIndex >= lowLimit)
    {
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;
            if (*(match+ml) == *(ip+ml) && (MEM_read32(match) == MEM_read32(ip)))
            {
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
                if (mlt > ml) { ml = mlt; *matchpos = match; }
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iLimit) vLimit = iLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iLimit);
                if (mlt > ml) { ml = mlt; *matchpos = base + matchIndex; }   /* virtual matchpos */
            }
        }
    }
    
    return (int)ml;
}


FORCE_INLINE int LZ6HC_FindMatchFastest (LZ6HC_Data_Structure* ctx, U32 matchIndex,  /* Index table will be updated */
                                               const BYTE* ip, const BYTE* const iLimit,
                                               const BYTE** matchpos)
{
    const BYTE* const base = ctx->base;
    const BYTE* const dictBase = ctx->dictBase;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);     
	const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const BYTE* match;
    size_t ml=0, mlt;

	if (matchIndex < current && matchIndex >= lowLimit)
    {
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;
            if (*(match+ml) == *(ip+ml) && (MEM_read32(match) == MEM_read32(ip)))
            {
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, iLimit) + MINMATCH;
                if (mlt > ml) { ml = mlt; *matchpos = match; }
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iLimit) vLimit = iLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iLimit);
                if (mlt > ml) { ml = mlt; *matchpos = base + matchIndex; }   /* virtual matchpos */
            }
        }
    }
    
    return (int)ml;
}


FORCE_INLINE size_t LZ6HC_GetWiderMatch (
    LZ6HC_Data_Structure* ctx,
    const BYTE* const ip,
    const BYTE* const iLowLimit,
    const BYTE* const iHighLimit,
    size_t longest,
    const BYTE** matchpos,
    const BYTE** startpos)
{
    U32* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const BYTE* const lowPrefixPtr = base + dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);
	const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match;
    U32   matchIndex;
    int nbAttempts = ctx->params.searchNum;


    /* First Match */
    matchIndex = HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];

    match = ip - ctx->last_off;
    if (MEM_read24(match) == MEM_read24(ip))
    {
        size_t mlt = MEM_count(ip+MINMATCH, match+MINMATCH, iHighLimit) + MINMATCH;
        
        int back = 0;
        while ((ip+back>iLowLimit) && (match+back > lowPrefixPtr) && (ip[back-1] == match[back-1])) back--;
        mlt -= back;

        if (mlt > longest)
        {
            *matchpos = match+back;
            *startpos = ip+back;
            longest = (int)mlt;
        }
    }

#if MINMATCH == 3
	{
        U32 matchIndex3 = ctx->hashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)];
		if (matchIndex3 < current && matchIndex3 >= lowLimit)
		{
			size_t offset = (size_t)current - matchIndex3;
			if (offset < LZ6_SHORT_OFFSET_DISTANCE)
			{
				match = ip - offset;
				if (match > base && MEM_read24(ip) == MEM_read24(match))
				{
					size_t mlt = MEM_count(ip + MINMATCH, match + MINMATCH, iHighLimit) + MINMATCH;

					int back = 0;
					while ((ip + back > iLowLimit) && (match + back > lowPrefixPtr) && (ip[back - 1] == match[back - 1])) back--;
					mlt -= back;

					if (!longest || (mlt > longest && LZ6HC_better_price((ip + back - *matchpos), longest, (ip - match), mlt, ctx->last_off)))
						//          if (!longest || (mlt > longest && LZ6_NORMAL_MATCH_COST(mlt - MINMATCH, (ip - match == ctx->last_off) ? 0 : (ip - match)) < LZ6_NORMAL_MATCH_COST(longest - MINMATCH, (ip+back - *matchpos == ctx->last_off) ? 0 : (ip+back - *matchpos)) + LZ6_NORMAL_LIT_COST(mlt - longest)))
					{
						*matchpos = match + back;
						*startpos = ip + back;
						longest = (int)mlt;
					}
				}
			}
		}
	}
#endif

    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        nbAttempts--;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;

            if (MEM_read32(match) == MEM_read32(ip))
            {
                size_t mlt = MINMATCH + MEM_count(ip+MINMATCH, match+MINMATCH, iHighLimit);
                int back = 0;

                while ((ip+back>iLowLimit)
                       && (match+back > lowPrefixPtr)
                       && (ip[back-1] == match[back-1]))
                        back--;

                mlt -= back;

                if (!longest || (mlt > longest && LZ6HC_better_price((ip+back - *matchpos), longest, (ip - match), mlt, ctx->last_off)))
                {
                    longest = (int)mlt;
                    *matchpos = match+back;
                    *startpos = ip+back;
                }
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                size_t mlt;
                int back=0;
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iHighLimit);
                while ((ip+back > iLowLimit) && (matchIndex+back > lowLimit) && (ip[back-1] == match[back-1])) back--;
                mlt -= back;
                if (mlt > longest) { longest = (int)mlt; *matchpos = base + matchIndex + back; *startpos = ip+back; }
            }
        }
        matchIndex -= chainTable[matchIndex & contentMask];
    }


    return longest;
}


FORCE_INLINE int LZ6HC_GetAllMatches (
    LZ6HC_Data_Structure* ctx,
    const BYTE* const ip,
    const BYTE* const iLowLimit,
    const BYTE* const iHighLimit,
    size_t best_mlen,
    LZ6HC_match_t* matches)
{
    U32* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    U32* const HashTable3 = ctx->hashTable3;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const BYTE* const lowPrefixPtr = base + dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);
    const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match;
    U32   matchIndex;
    int nbAttempts = ctx->params.searchNum;
 //   bool fullSearch = (ctx->params.fullSearch >= 2);
    int mnum = 0;
    U32* HashPos, *HashPos3;

    if (ip + MINMATCH > iHighLimit) return 0;

    /* First Match */
    HashPos = &HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];
    matchIndex = *HashPos;
#if MINMATCH == 3
    HashPos3 = &HashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)];

    if ((*HashPos3 < current) && (*HashPos3 >= lowLimit)) 
	{
		size_t offset = current - *HashPos3;
		if (offset < LZ6_SHORT_OFFSET_DISTANCE)
		{
			match = ip - offset;
			if (match > base && MEM_read24(ip) == MEM_read24(match))
			{
				size_t mlt = MEM_count(ip + MINMATCH, match + MINMATCH, iHighLimit) + MINMATCH;

				int back = 0;
				while ((ip + back > iLowLimit) && (match + back > lowPrefixPtr) && (ip[back - 1] == match[back - 1])) back--;
				mlt -= back;

				matches[mnum].off = (int)offset;
				matches[mnum].len = (int)mlt;
				matches[mnum].back = -back;
				mnum++;
			}
		}
	}

    *HashPos3 = current;
#endif


    chainTable[current & contentMask] = (U32)(current - matchIndex);
    *HashPos =  current;
    ctx->nextToUpdate++;


    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        /* Issue the chain-hop load now so its latency overlaps the candidate
           check below, and prefetch the next candidate's bytes. matchIndex only
           decreases along the chain, so this visits exactly the same indices as
           the tail-hop form => byte-identical output. */
        U32 nextIndex = matchIndex - chainTable[matchIndex & contentMask];
        LZ6_PREFETCH(base + nextIndex);
        nbAttempts--;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;

            /* best_mlen can exceed (iHighLimit-ip): a previous candidate in this
               same chain-walk may have been extended backward (see 'back' below),
               so its accepted length measures from before ip, not from ip -- it
               can legitimately be longer than the remaining forward distance to
               iHighLimit. ip[best_mlen] would then read past iHighLimit (and, at
               an exact allocation-size boundary, past the buffer itself). Skip
               the speculative peek rather than risk that read; falling through
               to the real check below never mis-skips a valid candidate. */
            if ((ip + best_mlen >= iHighLimit || ip[best_mlen] == match[best_mlen]) && (MEM_read24(match) == MEM_read24(ip)))
            {
                size_t mlt = MINMATCH + MEM_count(ip+MINMATCH, match+MINMATCH, iHighLimit);
                int back = 0;

                while ((ip+back>iLowLimit)
                       && (match+back > lowPrefixPtr)
                       && (ip[back-1] == match[back-1]))
                        back--;

                mlt -= back;

                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = -back;
                    mnum++;
                }

                if (best_mlen > LZ6_OPT_NUM) break;
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                size_t mlt;
                int back=0;
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iHighLimit);
                while ((ip+back > iLowLimit) && (matchIndex+back > lowLimit) && (ip[back-1] == match[back-1])) back--;
                mlt -= back;
                
                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = -back;
                    mnum++;
                }

                if (best_mlen > LZ6_OPT_NUM) break;
            }
        }
        matchIndex = nextIndex;
    }


    return mnum;
}



FORCE_INLINE int LZ6HC_BinTree_GetAllMatches (
    LZ6HC_Data_Structure* ctx,
    const BYTE* const ip,
    const BYTE* const iHighLimit,
    size_t best_mlen,
    LZ6HC_match_t* matches)
{
    U32* const chainTable = ctx->chainTable;
    U32* const HashTable = ctx->hashTable;
    const BYTE* const base = ctx->base;
    const U32 dictLimit = ctx->dictLimit;
    const U32 maxDistance = (1 << ctx->params.windowLog);
    const U32 current = (U32)(ip - base);
    const U32 lowLimit = (ctx->lowLimit + maxDistance > current) ? ctx->lowLimit : current - (maxDistance - 1);
    const U32 contentMask = (1 << ctx->params.contentLog) - 1;
    const BYTE* const dictBase = ctx->dictBase;
    const BYTE* match;
    int nbAttempts = ctx->params.searchNum;
    int mnum = 0;
    U32 *ptr0, *ptr1;
    U32 matchIndex, delta0, delta1;
    size_t mlt = 0;
    U32* HashPos, *HashPos3;
    
    if (ip + MINMATCH > iHighLimit) return 0;

    /* First Match */
    HashPos = &HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];
    matchIndex = *HashPos;

    
#if MINMATCH == 3
    HashPos3 = &ctx->hashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)];

    if ((*HashPos3 < current) && (*HashPos3 >= lowLimit)) 
	{
		size_t offset = current - *HashPos3;
		if (offset < LZ6_SHORT_OFFSET_DISTANCE)
		{
			match = ip - offset;
			if (match > base && MEM_read24(ip) == MEM_read24(match))
			{
				mlt = MEM_count(ip + MINMATCH, match + MINMATCH, iHighLimit) + MINMATCH;

				matches[mnum].off = (int)offset;
				matches[mnum].len = (int)mlt;
				matches[mnum].back = 0;
				mnum++;
			}
		}

		*HashPos3 = current;
	}
#endif
    

    *HashPos = current;
    ctx->nextToUpdate++;

    // check rest of matches
    ptr0 = &chainTable[(current*2+1) & contentMask];
    ptr1 = &chainTable[(current*2) & contentMask];
    delta0 = delta1 = current - matchIndex;

    while ((matchIndex < current) && (matchIndex>=lowLimit) && (nbAttempts))
    {
        nbAttempts--;
        mlt = 0;
        if (matchIndex >= dictLimit)
        {
            match = base + matchIndex;

            if (MEM_read24(match) == MEM_read24(ip))
            {
                mlt = MINMATCH + MEM_count(ip+MINMATCH, match+MINMATCH, iHighLimit);

                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = 0;
                    mnum++;
                }

                if (best_mlen > LZ6_OPT_NUM) break;
            }
        }
        else
        {
            match = dictBase + matchIndex;
            if (MEM_read32(match) == MEM_read32(ip))
            {
                const BYTE* vLimit = ip + (dictLimit - matchIndex);
                if (vLimit > iHighLimit) vLimit = iHighLimit;
                mlt = MEM_count(ip+MINMATCH, match+MINMATCH, vLimit) + MINMATCH;
                if ((ip+mlt == vLimit) && (vLimit < iHighLimit))
                    mlt += MEM_count(ip+mlt, base+dictLimit, iHighLimit);
                
                if (mlt > best_mlen)
                {
                    best_mlen = mlt;
                    matches[mnum].off = (int)(ip - match);
                    matches[mnum].len = (int)mlt;
                    matches[mnum].back = 0;
                    mnum++;
                }

                if (best_mlen > LZ6_OPT_NUM) break;
            }
        }
        
        if (*(ip+mlt) < *(match+mlt))
        {
            *ptr0 = delta0;
            ptr0 = &chainTable[(matchIndex*2) & contentMask];
    //		printf("delta0=%d\n", delta0);
            if (*ptr0 == (U32)-1) break;
            delta0 = *ptr0;
            delta1 += delta0;
            matchIndex -= delta0;
        }
        else
        {
            *ptr1 = delta1;
            ptr1 = &chainTable[(matchIndex*2+1) & contentMask];
    //		printf("delta1=%d\n", delta1);
            if (*ptr1 == (U32)-1) break;
            delta1 = *ptr1;
            delta0 += delta1;
            matchIndex -= delta1;
        }
    }

    *ptr0 = (U32)-1;
    *ptr1 = (U32)-1;

    return mnum;
}




typedef enum { noLimit = 0, limitedOutput = 1 } limitedOutput_directive;

/*
LZ6 uses 3 types of codewords from 2 to 4 bytes long:
- 1_OO_LL_MMM OOOOOOOO - 10-bit offset, 3-bit match length, 2-bit literal length
- 00_LLL_MMM OOOOOOOO OOOOOOOO - 16-bit offset, 3-bit match length, 3-bit literal length
- 010_LL_MMM OOOOOOOO OOOOOOOO OOOOOOOO - 24-bit offset, 3-bit match length, 2-bit literal length 
- 011_LL_MMM - last offset, 3-bit match length, 2-bit literal length
*/

FORCE_INLINE int LZ6HC_encodeSequence (
    LZ6HC_Data_Structure* ctx,
    const BYTE** ip,
    BYTE** op,
    const BYTE** anchor,
    int matchLength,
    const BYTE* const match,
    limitedOutput_directive limitedOutputBuffer,
    BYTE* oend)
{
    int length;
    BYTE* token;

    /* repeat-offset: reuse last offset via the 1-byte (flag 011) codeword, no offset bytes.
       Two callers signal a rep differently: the optimal parser passes off==0 (match==ip),
       the lowest_price parser passes the actual offset which equals last_off. Both => rep. */
    const int isRep = ((U32)(*ip - match) == 0) || ((U32)(*ip - match) == ctx->last_off);
    const size_t offset = (size_t)(*ip - match);

    /* Sequence-extraction path: skip the codeword, hand the caller the raw
       (lit_len, match_len, offset) tuple. The encoder still advances ip/anchor
       and updates last_off so the next call sees the right state.
       Rep matches: the optimal parser passes off==0 (match==ip) to signal
       "reuse last_off"; recover the actual offset here so the entropy coder
       sees a real value. */
    if (ctx->emitSeq)
    {
        size_t lit_len = (size_t)(*ip - *anchor);
        size_t emit_offset = isRep ? (size_t)ctx->last_off : offset;
        int rc = ctx->emitSeq(ctx->emitOpaque, lit_len, (size_t)matchLength, emit_offset);
        if (rc) return 1;
        /* maintain the MTF rep stack exactly like the entropy encoder's
         * repcache, so the parser's rep preference sees the same state */
        if (!isRep) {
            U32 off = (U32)offset;
            if (off == ctx->rep_off2) {
                U32 t = ctx->rep_off2;
                ctx->rep_off2 = ctx->last_off;
                ctx->last_off = t;
            } else if (off == ctx->rep_off3) {
                U32 t = ctx->rep_off3;
                ctx->rep_off3 = ctx->rep_off2;
                ctx->rep_off2 = ctx->last_off;
                ctx->last_off = t;
            } else {
                ctx->rep_off3 = ctx->rep_off2;
                ctx->rep_off2 = ctx->last_off;
                ctx->last_off = off;
            }
        }
        *ip += matchLength;
        *anchor = *ip;
        return 0;
    }

    /* Encode Literal length */
    length = (int)(*ip - *anchor);
    token = (*op)++;

    if ((limitedOutputBuffer) && ((*op + (length>>8) + length + (2 + 1 + LASTLITERALS)) > oend)) return 1;   /* Check output limit */

    if (!isRep && *ip-match >= LZ6_SHORT_OFFSET_DISTANCE && *ip-match < LZ6_MID_OFFSET_DISTANCE)
    {
        if (length>=(int)RUN_MASK) { int len; *token=(RUN_MASK<<ML_BITS); len = length-RUN_MASK; for(; len > 254 ; len-=255) *(*op)++ = 255;  *(*op)++ = (BYTE)len; }
        else *token = (BYTE)(length<<ML_BITS);
    }
    else
    {
        if (length>=(int)RUN_MASK2) { int len; *token=(RUN_MASK2<<ML_BITS); len = length-RUN_MASK2; for(; len > 254 ; len-=255) *(*op)++ = 255;  *(*op)++ = (BYTE)len; }
        else *token = (BYTE)(length<<ML_BITS);

    }

    /* Copy Literals */
    MEM_wildCopy(*op, *anchor, (*op) + length);
    *op += length;

    /* Encode Offset */
    if (isRep)
    {
        *token+=(3<<ML_RUN_BITS2);
    }
    else
    {
		ctx->last_off = (U32)(*ip-match);
        if (ctx->last_off < LZ6_SHORT_OFFSET_DISTANCE)
        {
            *token+=(BYTE)((4+(ctx->last_off>>8))<<ML_RUN_BITS2);
            **op=(BYTE)ctx->last_off; (*op)++;
        }
        else
        if (*ip-match < LZ6_MID_OFFSET_DISTANCE)
        {
            MEM_writeLE16(*op, (U16)ctx->last_off); *op+=2;
        }
        else
        {
            *token+=(2<<ML_RUN_BITS2);
            MEM_writeLE24(*op, (U32)ctx->last_off); *op+=3;
        }
    }

    /* Encode MatchLength */
    length = (int)(matchLength-MINMATCH);
    if ((limitedOutputBuffer) && (*op + (length>>8) + (1 + LASTLITERALS) > oend)) return 1;   /* Check output limit */
    if (length>=(int)ML_MASK) { *token+=ML_MASK; length-=ML_MASK; for(; length > 509 ; length-=510) { *(*op)++ = 255; *(*op)++ = 255; } if (length > 254) { length-=255; *(*op)++ = 255; } *(*op)++ = (BYTE)length; }
    else *token += (BYTE)(length);

    LZ6HC_DEBUG("%u: ENCODE literals=%u off=%u mlen=%u out=%u\n", (U32)(*ip - ctx->inputBuffer), (U32)(*ip - *anchor), (U32)(*ip-match), (U32)matchLength, 2+(U32)(*op - ctx->outputBuffer));

    /* Prepare next loop */
    *ip += matchLength;
    *anchor = *ip;

    return 0;
}


#define SET_PRICE(pos, ml_, offset, litlen, price)   \
    {                                                 \
        while (last_pos < pos)  { opt[last_pos+1].price = 1<<30; last_pos++; } \
        opt[pos].mlen = (int)(ml_);                        \
        opt[pos].off = (int)offset;                        \
        opt[pos].litlen = (int)litlen;                     \
        opt[pos].price = (int)price;                       \
        LZ6_LOG_PARSER("%d: SET price[%d/%d]=%d litlen=%d len=%d off=%d\n", (int)(inr-source), pos, last_pos, opt[pos].price, opt[pos].litlen, opt[pos].mlen, opt[pos].off); \
    }


/* entropy-aware prices (ctx->seqPrice) for the optimal parser; the byte
 * codeword prices stay the default so the frame codec is unchanged */
static inline unsigned LZ6HC_spLL(const LZ6HC_seqPrice* sp, size_t n) { return sp->ll[n < LZ6HC_SP_LEN ? n : LZ6HC_SP_LEN]; }
static inline unsigned LZ6HC_spML(const LZ6HC_seqPrice* sp, size_t n) { return sp->ml[n < LZ6HC_SP_LEN ? n : LZ6HC_SP_LEN]; }
static inline size_t LZ6HC_sp_price(const LZ6HC_seqPrice* sp, size_t litlen, size_t offset, size_t mlen3)
{
    unsigned oc;
    if (offset == 0) oc = sp->rep0;
    else { unsigned b = 31u - (unsigned)__builtin_clz((unsigned)offset); oc = sp->of[(b < 25 ? b : 24) * 8 + (offset & 7)]; }
    return (size_t)sp->lit * litlen + LZ6HC_spLL(sp, litlen) + LZ6HC_spML(sp, mlen3 + MINMATCH) + oc;
}
#define OPT_PRICE(l, o, m3) (ctx->seqPrice ? LZ6HC_sp_price(ctx->seqPrice, (l), (o), (m3)) : LZ6HC_get_price((l), (o), (m3)))
#define OPT_LITONLY(n)      (ctx->seqPrice ? (size_t)ctx->seqPrice->lit * (n) + LZ6HC_spLL(ctx->seqPrice, (n)) : (size_t)LZ6_LIT_ONLY_COST(n))
#define OPT_LLEN(n)         (ctx->seqPrice ? (size_t)ctx->seqPrice->lit * (n) : (size_t)(n))

int LZ6HC_seqLevelIsOptimal(int level)
{
    if (level < 1) level = LZ6HC_compressionLevel_default;
    if (level > g_maxCompressionLevel) level = g_maxCompressionLevel;
#ifdef LZ6_SEQ_TUNING
    if (getenv("LZ6_SEQPARAMS") && getenv("LZ6_SEQPARAMS_LEVEL") && atoi(getenv("LZ6_SEQPARAMS_LEVEL")) == level) {
        unsigned v[7];
        if (sscanf(getenv("LZ6_SEQPARAMS"), "%u:%u:%u:%u:%u:%u:%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) == 7)
            return v[6] == (unsigned)LZ6HC_optimal_price || v[6] == (unsigned)LZ6HC_optimal_price_bt;
    }
#endif
    return LZ6HC_seqParameters[level].strategy == LZ6HC_optimal_price
        || LZ6HC_seqParameters[level].strategy == LZ6HC_optimal_price_bt;
}

void LZ6HC_setSeqPrice(void* state, const LZ6HC_seqPrice* sp)
{
    ((LZ6HC_Data_Structure*)state)->seqPrice = sp;
}

static int LZ6HC_compress_optimal_price (
    LZ6HC_Data_Structure* ctx,
    const BYTE* source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit
    )
{
	LZ6HC_optimal_t opt[LZ6_OPT_NUM + 4];
	LZ6HC_match_t matches[LZ6_OPT_NUM + 1];
	const BYTE *inr;
	size_t res, cur, cur2, skip_num = 0;
	size_t i, llen, litlen, mlen, best_mlen, price, offset, best_off, match_num, last_pos;

    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = (iend - LASTLITERALS);
    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + maxOutputSize;
    const size_t sufficient_len = ctx->params.sufficientLength;
    const int faster_get_matches = (ctx->params.fullSearch == 0); 
 

    /* init */
	ctx->inputBuffer = (const BYTE*)source;
	ctx->outputBuffer = (const BYTE*)dest;
	ctx->end += inputSize;
    ip++;

    /* Main Loop */
    while (ip < mflimit)
    {
        memset(opt, 0, sizeof(LZ6HC_optimal_t));
        last_pos = 0;
        llen = ip - anchor;

        // check rep
        mlen = MEM_count(ip, ip - ctx->last_off, matchlimit);
        if (mlen >= MINMATCH)
        {
            LZ6_LOG_PARSER("%d: start try REP rep=%d mlen=%d\n", (int)(ip-source), ctx->last_off, mlen);
            if (mlen > sufficient_len || mlen >= LZ6_OPT_NUM)
            {
                best_mlen = mlen; best_off = 0; cur = 0; last_pos = 1;
                goto encode;
            }

            do
            {
                litlen = 0;
                price = OPT_PRICE(llen, 0, mlen - MINMATCH) - OPT_LLEN(llen);
                if (mlen > last_pos || price < (size_t)opt[mlen].price)
                    SET_PRICE(mlen, mlen, 0, litlen, price);
                mlen--;
            }
            while (mlen >= MINMATCH);
        }

 
       best_mlen = (last_pos) ? last_pos : MINMATCH;

       if (faster_get_matches && last_pos)
           match_num = 0;
       else
       {
            if (ctx->params.strategy == LZ6HC_optimal_price)
            {
                LZ6HC_Insert(ctx, ip);
                match_num = LZ6HC_GetAllMatches(ctx, ip, ip, matchlimit, best_mlen, matches);
            }
            else
            {
                if (ctx->params.fullSearch < 2)
                    LZ6HC_BinTree_Insert(ctx, ip);
                else
                    LZ6HC_BinTree_InsertFull(ctx, ip, matchlimit);
                match_num = LZ6HC_BinTree_GetAllMatches(ctx, ip, matchlimit, best_mlen, matches);
            }
       }

       LZ6_LOG_PARSER("%d: match_num=%d last_pos=%d\n", (int)(ip-source), match_num, last_pos);
       if (!last_pos && !match_num) { ip++; continue; }

       if (match_num && (size_t)matches[match_num-1].len > sufficient_len)
       {
            best_mlen = matches[match_num-1].len;
            best_off = matches[match_num-1].off;
            cur = 0;
            last_pos = 1;
            goto encode;
       }

       // set prices using matches at position = 0
       // stack-aware: a match offset sitting in the current rep stack codes
       // as a 2-bit rep flag in the seq backend — price it like rep0
       for (i = 0; i < match_num; i++)
       {
           mlen = (i>0) ? (size_t)matches[i-1].len+1 : best_mlen;
           best_mlen = (matches[i].len < LZ6_OPT_NUM) ? matches[i].len : LZ6_OPT_NUM;
           LZ6_LOG_PARSER("%d: start Found mlen=%d off=%d best_mlen=%d last_pos=%d\n", (int)(ip-source), matches[i].len, matches[i].off, best_mlen, last_pos);
           U32 eff_off = (U32)matches[i].off;
           if (ctx->emitSeq && (eff_off == (U32)ctx->last_off || eff_off == ctx->rep_off2 || eff_off == ctx->rep_off3)) eff_off = 0;
           while (mlen <= best_mlen)
           {
                litlen = 0;
                price = OPT_PRICE(llen + litlen, eff_off, mlen - MINMATCH) - OPT_LLEN(llen);
                if (mlen > last_pos || price < (size_t)opt[mlen].price)
                    SET_PRICE(mlen, mlen, matches[i].off, litlen, price);
                mlen++;
           }
       }

        if (last_pos < MINMATCH) { ip++; continue; }

        opt[0].rep = opt[1].rep = ctx->last_off;
        opt[0].rep2 = opt[1].rep2 = (int)ctx->rep_off2;
        opt[0].rep3 = opt[1].rep3 = (int)ctx->rep_off3;
        opt[0].mlen = opt[1].mlen = 1;

        // check further positions
        for (skip_num = 0, cur = 1; cur <= last_pos; cur++)
        { 
           inr = ip + cur;

           if (opt[cur-1].mlen == 1)
           {
                litlen = opt[cur-1].litlen + 1;
                
                if (cur != litlen)
                {
                    price = opt[cur - litlen].price + OPT_LITONLY(litlen);
                    LZ6_LOG_PRICE("%d: TRY1 opt[%d].price=%d price=%d cur=%d litlen=%d\n", (int)(inr-source), cur - litlen, opt[cur - litlen].price, price, cur, litlen);
                }
                else
                {
                    price = OPT_LITONLY(llen + litlen) - OPT_LLEN(llen);
                    LZ6_LOG_PRICE("%d: TRY2 price=%d cur=%d litlen=%d llen=%d\n", (int)(inr-source), price, cur, litlen, llen);
                }
           }
           else
           {
                litlen = 1;
                price = opt[cur - 1].price + OPT_LITONLY(litlen);                  
                LZ6_LOG_PRICE("%d: TRY3 price=%d cur=%d litlen=%d litonly=%d\n", (int)(inr-source), price, cur, litlen, OPT_LITONLY(litlen));
           }
           
           mlen = 1;
           best_mlen = 0;
           LZ6_LOG_PARSER("%d: TRY price=%d opt[%d].price=%d\n", (int)(inr-source), price, cur, opt[cur].price);

           if (cur > last_pos || price <= (size_t)opt[cur].price) // || ((price == opt[cur].price) && (opt[cur-1].mlen == 1) && (cur != litlen)))
                SET_PRICE(cur, mlen, best_mlen, litlen, price);

           if (cur == last_pos) break;

           if (opt[cur].mlen > 1)
           {
                mlen = opt[cur].mlen;
                offset = opt[cur].off;
                /* maintain the hypothetical MTF rep-stack for the DP: the
                 * chosen match at cur was (offset, mlen) starting from the
                 * stack at cur-mlen */
                {
                    U32 r0 = (U32)opt[cur-mlen].rep, r1 = (U32)opt[cur-mlen].rep2, r2 = (U32)opt[cur-mlen].rep3;
                    if (offset < 1 || (U32)offset == r0)
                    {
                        opt[cur].rep = (int)r0; opt[cur].rep2 = (int)r1; opt[cur].rep3 = (int)r2;
                    }
                    else if ((U32)offset == r1)
                    {
                        opt[cur].rep = (int)r1; opt[cur].rep2 = (int)r0; opt[cur].rep3 = (int)r2;
                    }
                    else if ((U32)offset == r2)
                    {
                        opt[cur].rep = (int)r2; opt[cur].rep2 = (int)r0; opt[cur].rep3 = (int)r1;
                    }
                    else
                    {
                        opt[cur].rep = offset; opt[cur].rep2 = (int)r0; opt[cur].rep3 = (int)r1;
                    }
                }
           }
           else
           {
                opt[cur].rep = opt[cur-1].rep; // copy rep
                opt[cur].rep2 = opt[cur-1].rep2;
                opt[cur].rep3 = opt[cur-1].rep3;
           }


            LZ6_LOG_PARSER("%d: CURRENT price[%d/%d]=%d off=%d mlen=%d litlen=%d rep=%d\n", (int)(inr-source), cur, last_pos, opt[cur].price, opt[cur].off, opt[cur].mlen, opt[cur].litlen, opt[cur].rep); 

           // check rep
           best_mlen = 0;  /* MUST reset per-cur: a stale value from an earlier
                               cur (this var is function-scoped) is later fed
                               unchanged into LZ6HC_GetAllMatches as its
                               best_mlen bound; if it's too high the ip[best_mlen]
                               head-check reads past iHighLimit (heap-buffer-
                               overflow at an exact block-size boundary, e.g. the
                               16MB streaming block edge) AND silently discards
                               genuinely-longest-so-far matches at this position. */
           mlen = MEM_count(inr, inr - opt[cur].rep, matchlimit);
           if (mlen >= MINMATCH && mlen > best_mlen)
           {
              LZ6_LOG_PARSER("%d: try REP rep=%d mlen=%d\n", (int)(inr-source), opt[cur].rep, mlen);   
              LZ6_LOG_PARSER("%d: Found REP mlen=%d off=%d rep=%d opt[%d].off=%d\n", (int)(inr-source), mlen, 0, opt[cur].rep, cur, opt[cur].off);

              if (mlen > sufficient_len || cur + mlen >= LZ6_OPT_NUM)
              {
                best_mlen = mlen;
                best_off = 0;
                LZ6_LOG_PARSER("%d: REP sufficient_len=%d best_mlen=%d best_off=%d last_pos=%d\n", (int)(inr-source), sufficient_len, best_mlen, best_off, last_pos);
                last_pos = cur + 1;
                goto encode;
               }

               if (opt[cur].mlen == 1)
               {
                    litlen = opt[cur].litlen;

                    if (cur != litlen)
                    {
                        price = opt[cur - litlen].price + OPT_PRICE(litlen, 0, mlen - MINMATCH);
                        LZ6_LOG_PRICE("%d: TRY1 opt[%d].price=%d price=%d cur=%d litlen=%d\n", (int)(inr-source), cur - litlen, opt[cur - litlen].price, price, cur, litlen);
                    }
                    else
                    {
                        price = OPT_PRICE(llen + litlen, 0, mlen - MINMATCH) - OPT_LLEN(llen);
                        LZ6_LOG_PRICE("%d: TRY2 price=%d cur=%d litlen=%d llen=%d\n", (int)(inr-source), price, cur, litlen, llen);
                    }
                }
                else
                {
                    litlen = 0;
                    price = opt[cur].price + OPT_PRICE(litlen, 0, mlen - MINMATCH);
                    LZ6_LOG_PRICE("%d: TRY3 price=%d cur=%d litlen=%d getprice=%d\n", (int)(inr-source), price, cur, litlen, OPT_PRICE(litlen, 0, mlen - MINMATCH));
                }

                best_mlen = mlen;
                if (faster_get_matches)
                    skip_num = best_mlen;

                LZ6_LOG_PARSER("%d: Found REP mlen=%d off=%d price=%d litlen=%d price[%d]=%d\n", (int)(inr-source), mlen, 0, price, litlen, cur - litlen, opt[cur - litlen].price);

                do
                {
                    if (cur + mlen > last_pos || price <= (size_t)opt[cur + mlen].price) // || ((price == opt[cur + mlen].price) && (opt[cur].mlen == 1) && (cur != litlen))) // at equal price prefer REP instead of MATCH
                        SET_PRICE(cur + mlen, mlen, 0, litlen, price);
                    mlen--;
                }
                while (mlen >= MINMATCH);
            }

            /* rep1/rep2 candidates: same price as rep0 — the seq backend
             * codes any rep-stack hit as a 2-bit flag — but stored with
             * their real offset so the emit passes it through and the
             * encoder's MTF detection codes it as pc=2/3. Seq-only: the
             * frame format has a single repeat offset (rep0), so pricing
             * rep1/2 at rep cost would lie about its codewords. */
            if (ctx->emitSeq)
            for (int rix = 1; rix <= 2; rix++)
            {
                U32 r = (rix == 1) ? (U32)opt[cur].rep2 : (U32)opt[cur].rep3;
                if (r == 0 || r == (U32)opt[cur].rep) continue;
                int rmlen = (int)MEM_count(inr, inr - r, matchlimit);
                if (rmlen < MINMATCH) continue;

                if ((size_t)rmlen > sufficient_len || cur + rmlen >= LZ6_OPT_NUM)
                {
                    best_mlen = rmlen;
                    best_off = (int)r;
                    last_pos = cur + 1;
                    goto encode;
                }

                if (opt[cur].mlen == 1)
                {
                    litlen = opt[cur].litlen;
                    if (cur != litlen)
                        price = opt[cur - litlen].price + OPT_PRICE(litlen, 0, rmlen - MINMATCH);
                    else
                        price = OPT_PRICE(llen + litlen, 0, rmlen - MINMATCH) - OPT_LLEN(llen);
                }
                else
                {
                    litlen = 0;
                    price = opt[cur].price + OPT_PRICE(litlen, 0, rmlen - MINMATCH);
                }

                if ((size_t)rmlen > best_mlen) best_mlen = rmlen;
                {
                    int mm = rmlen;
                    do
                    {
                        if (cur + mm > last_pos || price <= (size_t)opt[cur + mm].price)
                            SET_PRICE(cur + mm, mm, (int)r, litlen, price);
                        mm--;
                    }
                    while (mm >= MINMATCH);
                }
            }

            if (faster_get_matches && skip_num > 0)
            {
                skip_num--; 
                continue;
            }


            best_mlen = (best_mlen > MINMATCH) ? best_mlen : MINMATCH;      

            if (ctx->params.strategy == LZ6HC_optimal_price)
            {
                LZ6HC_Insert(ctx, inr);
                match_num = LZ6HC_GetAllMatches(ctx, inr, ip, matchlimit, best_mlen, matches);
                LZ6_LOG_PARSER("%d: LZ6HC_GetAllMatches match_num=%d\n", (int)(inr-source), match_num);
            }
            else
            {
                if (ctx->params.fullSearch < 2)
                    LZ6HC_BinTree_Insert(ctx, inr);
                else
                    LZ6HC_BinTree_InsertFull(ctx, inr, matchlimit);
                match_num = LZ6HC_BinTree_GetAllMatches(ctx, inr, matchlimit, best_mlen, matches);
                LZ6_LOG_PARSER("%d: LZ6HC_BinTree_GetAllMatches match_num=%d\n", (int)(inr-source), match_num);
            }


            if (match_num > 0 && (size_t)matches[match_num-1].len > sufficient_len)
            {
                cur -= matches[match_num-1].back;
                best_mlen = matches[match_num-1].len;
                best_off = matches[match_num-1].off;
                last_pos = cur + 1;
                goto encode;
            }

            // set prices using matches at position = cur
            for (i = 0; i < match_num; i++)
            {
                mlen = (i>0) ? (size_t)matches[i-1].len+1 : best_mlen;
                cur2 = cur - matches[i].back;
                best_mlen = (cur2 + matches[i].len < LZ6_OPT_NUM) ? (size_t)matches[i].len : LZ6_OPT_NUM - cur2;
                LZ6_LOG_PARSER("%d: Found1 cur=%d cur2=%d mlen=%d off=%d best_mlen=%d last_pos=%d\n", (int)(inr-source), cur, cur2, matches[i].len, matches[i].off, best_mlen, last_pos);

                if (mlen < (size_t)matches[i].back + 1)
                    mlen = matches[i].back + 1;

                /* stack-aware pricing: an offset in the rep stack at the
                 * match's source position (cur2) codes as a 2-bit rep flag.
                 * Seq-only — the frame format has no rep1/rep2 stack. */
                U32 eff_off = (U32)matches[i].off;
                if (ctx->emitSeq && (eff_off == (U32)opt[cur2].rep || eff_off == (U32)opt[cur2].rep2 || eff_off == (U32)opt[cur2].rep3)) eff_off = 0;

                while (mlen <= best_mlen)
                {
                    if (opt[cur2].mlen == 1)
                    {
                        litlen = opt[cur2].litlen;

                        if (cur2 != litlen)
                            price = opt[cur2 - litlen].price + OPT_PRICE(litlen, eff_off, mlen - MINMATCH);
                        else
                            price = OPT_PRICE(llen + litlen, eff_off, mlen - MINMATCH) - OPT_LLEN(llen);
                    }
                    else
                    {
                        litlen = 0;
                        price = opt[cur2].price + OPT_PRICE(litlen, eff_off, mlen - MINMATCH);
                    }

                    LZ6_LOG_PARSER("%d: Found2 pred=%d mlen=%d best_mlen=%d off=%d price=%d litlen=%d price[%d]=%d\n", (int)(inr-source), matches[i].back, mlen, best_mlen, matches[i].off, price, litlen, cur - litlen, opt[cur - litlen].price);
    //                if (cur2 + mlen > last_pos || ((matches[i].off != opt[cur2 + mlen].off) && (price < opt[cur2 + mlen].price)))
                    if (cur2 + mlen > last_pos || price < (size_t)opt[cur2 + mlen].price)
                    {
                        SET_PRICE(cur2 + mlen, mlen, matches[i].off, litlen, price);
                    }

                    mlen++;
                }
            }
        } //  for (skip_num = 0, cur = 1; cur <= last_pos; cur++)


        best_mlen = opt[last_pos].mlen;
        best_off = opt[last_pos].off;
        cur = last_pos - best_mlen;

encode: // cur, last_pos, best_mlen, best_off have to be set
        for (i = 1; i <= last_pos; i++)
        {
            LZ6_LOG_PARSER("%d: price[%d/%d]=%d off=%d mlen=%d litlen=%d rep=%d\n", (int)(ip-source+i), i, last_pos, opt[i].price, opt[i].off, opt[i].mlen, opt[i].litlen, opt[i].rep); 
        }

        LZ6_LOG_PARSER("%d: cur=%d/%d best_mlen=%d best_off=%d rep=%d\n", (int)(ip-source+cur), cur, last_pos, best_mlen, best_off, opt[cur].rep); 

        opt[0].mlen = 1;
        
        while (1)
        {
            mlen = opt[cur].mlen;
            offset = opt[cur].off;
            opt[cur].mlen = (int)best_mlen; 
            opt[cur].off = (int)best_off;
            best_mlen = mlen;
            best_off = offset;
            if (mlen > cur) break;
            cur -= mlen;
        }
          
        for (i = 0; i <= last_pos;)
        {
            LZ6_LOG_PARSER("%d: price2[%d/%d]=%d off=%d mlen=%d litlen=%d rep=%d\n", (int)(ip-source+i), i, last_pos, opt[i].price, opt[i].off, opt[i].mlen, opt[i].litlen, opt[i].rep); 
            i += opt[i].mlen;
        }

        cur = 0;

        while (cur < last_pos)
        {
            LZ6_LOG_PARSER("%d: price3[%d/%d]=%d off=%d mlen=%d litlen=%d rep=%d\n", (int)(ip-source+cur), cur, last_pos, opt[cur].price, opt[cur].off, opt[cur].mlen, opt[cur].litlen, opt[cur].rep); 
            mlen = opt[cur].mlen;
            if (mlen == 1) { ip++; cur++; continue; }
            offset = opt[cur].off;
            cur += mlen;

            LZ6_LOG_ENCODE("%d: ENCODE literals=%d off=%d mlen=%d ", (int)(ip-source), (int)(ip-anchor), (int)(offset), mlen);
            res = LZ6HC_encodeSequence(ctx, &ip, &op, &anchor, (int)mlen, ip - offset, limit, oend);
            LZ6_LOG_ENCODE("out=%d\n", (int)((char*)op - dest));

            if (res) return 0; 

            LZ6_LOG_PARSER("%d: offset=%d rep=%d\n", (int)(ip-source), offset, ctx->last_off);
        }
    }

    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
    //    if (inputSize > LASTLITERALS && lastRun < LASTLITERALS) { printf("ERROR: lastRun=%d\n", lastRun); }
        if (ctx->emitSeq)
        {
            /* sequence-extraction path: emit a final (lastRun, 0, 0) tuple */
            if (ctx->emitSeq(ctx->emitOpaque, (size_t)lastRun, 0, 0)) return 0;
        }
        else
        {
            if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;  /* Check output limit */
            if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
            else *op++ = (BYTE)(lastRun<<ML_BITS);
            LZ6_LOG_ENCODE("%d: ENCODE_LAST literals=%d out=%d\n", (int)(ip-source), (int)(iend-anchor), (int)((char*)op -dest));
            memcpy(op, anchor, iend - anchor);
            op += iend-anchor;
        }
    }

    /* End */
    return (int) ((char*)op-dest);
}



static int LZ6HC_compress_lowest_price (
    LZ6HC_Data_Structure* ctx,
    const char* source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit
    )
{
    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = (iend - LASTLITERALS);

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + maxOutputSize;

    int   ml, ml2, ml0;
    const BYTE* ref=NULL;
    const BYTE* start2=NULL;
    const BYTE* ref2=NULL;
    const BYTE* start0;
    const BYTE* ref0;
    const BYTE* lowPrefixPtr = ctx->base + ctx->dictLimit;

    /* init */
	ctx->inputBuffer = (const BYTE*)source;
	ctx->outputBuffer = (const BYTE*)dest;
	ctx->end += inputSize;

    ip++;

    /* Main Loop */
    while (ip < mflimit)
    {
        LZ6HC_Insert(ctx, ip);
        ml = LZ6HC_FindBestMatch (ctx, ip, matchlimit, (&ref));
        if (!ml) { ip++; continue; }

		{
			int back = 0;
			while ((ip + back > anchor) && (ref + back > lowPrefixPtr) && (ip[back - 1] == ref[back - 1])) back--;
			ml -= back;
			ip += back;
			ref += back;
		}

        /* Rep-stack preference (seq codec): same rationale as the fast
         * strategy — a rep offset within 2 bytes of the chain match saves
         * the whole offset coding. Gated on emitSeq. */
        if (ctx->emitSeq && (ctx->rep_off2 || ctx->rep_off3))
        {
            const U32 repcands[2] = { ctx->rep_off2, ctx->rep_off3 };
            for (int ri = 0; ri < 2; ri++) {
                U32 r = repcands[ri];
                if (r == 0 || r == ctx->last_off || r > (U32)(ip - lowPrefixPtr)) continue;
                const BYTE* rref = ip - r;
                int rml = (int)MEM_count(ip, rref, matchlimit);
                if (rml >= MINMATCH && rml + 2 >= ml) { ref = rref; ml = rml; break; }
            }
        }

        /* saved, in case we would skip too much */
        start0 = ip;
        ref0 = ref;
        ml0 = ml;

_Search:
        if (ip+ml >= mflimit) goto _Encode;

        LZ6HC_Insert(ctx, ip);
        ml2 = (int)LZ6HC_GetWiderMatch(ctx, ip + ml - 2, anchor, matchlimit, 0, &ref2, &start2);
        if (ml2 == 0) goto _Encode;

        {
        int price, best_price;
        U32 off0=0, off1=0;
        const uint8_t *pos, *best_pos;

    //	find the lowest price for encoding ml bytes
        best_pos = ip;
        best_price = 1<<30;
        off0 = (U32)(ip - ref);
        off1 = (U32)(start2 - ref2);

        for (pos = ip + ml; pos >= start2; pos--)
        {
            int common0 = (int)(pos - ip);
            if (common0 >= MINMATCH)
            {
                price = (int)LZ6_CODEWORD_COST(ip - anchor, (off0 == ctx->last_off) ? 0 : off0, common0 - MINMATCH);
                
				{
					int common1 = (int)(start2 + ml2 - pos);
					if (common1 >= MINMATCH)
						price += (int)LZ6_CODEWORD_COST(0, (off1 == off0) ? 0 : (off1), common1 - MINMATCH);
					else
						price += LZ6_LIT_ONLY_COST(common1) - 1;
				}

                if (price < best_price)
                {
                    best_price = price;
                    best_pos = pos;
                }
            }
            else
            {
                price = (int)LZ6_CODEWORD_COST(start2 - anchor, (off1 == ctx->last_off) ? 0 : off1, ml2 - MINMATCH);

                if (price < best_price)
                    best_pos = pos;
                break;
            }
        }
    //    LZ6HC_DEBUG("%u: TRY last_off=%d literals=%u off=%u mlen=%u literals2=%u off2=%u mlen2=%u best=%d\n", (U32)(ip - ctx->inputBuffer), ctx->last_off, (U32)(ip - anchor), off0, (U32)ml,  (U32)(start2 - anchor), off1, ml2, (U32)(best_pos - ip));
        ml = (int)(best_pos - ip);
        }


        if (ml < MINMATCH)
        {
            ip = start2;
            ref = ref2;
            ml = ml2;
            goto _Search;
        }
        
_Encode:

        if (start0 < ip)
        {
            if (LZ6HC_more_profitable((ip - ref), ml,(start0 - ref0), ml0, (ref0 - ref), ctx->last_off))
            {
                ip = start0;
                ref = ref0;
                ml = ml0;
            }
        }

        if (LZ6HC_encodeSequence(ctx, &ip, &op, &anchor, ml, ref, limit, oend)) return 0;
    }

    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
        if (ctx->emitSeq)
        {
            /* sequence-extraction path: emit a final (lastRun, 0, 0) tuple */
            if (ctx->emitSeq(ctx->emitOpaque, (size_t)lastRun, 0, 0)) return 0;
        }
        else
        {
            if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;  /* Check output limit */
            if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
            else *op++ = (BYTE)(lastRun<<ML_BITS);
            memcpy(op, anchor, iend - anchor);
            op += iend-anchor;
        }
    }

    /* End */
    return (int) (((char*)op)-dest);
}



static int LZ6HC_compress_price_fast (
    LZ6HC_Data_Structure* ctx,
    const char* source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit
    )
{
    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = (iend - LASTLITERALS);

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + maxOutputSize;

    int   ml, ml2=0;
    const BYTE* ref=NULL;
    const BYTE* start2=NULL;
    const BYTE* ref2=NULL;
    const BYTE* lowPrefixPtr = ctx->base + ctx->dictLimit;
    U32* HashTable  = ctx->hashTable;
#if MINMATCH == 3
    U32* HashTable3  = ctx->hashTable3;
#endif 
    const BYTE* const base = ctx->base;
    U32* HashPos, *HashPos3;

    /* init */
	ctx->inputBuffer = (const BYTE*)source;
	ctx->outputBuffer = (const BYTE*)dest;
	ctx->end += inputSize;

    ip++;

    /* Main Loop */
    while (ip < mflimit)
    {
        HashPos = &HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];
#if MINMATCH == 3
        HashPos3 = &HashTable3[LZ6HC_hash3Ptr(ip, ctx->params.hashLog3)];
        ml = LZ6HC_FindMatchFast (ctx, *HashPos, *HashPos3, ip, matchlimit, (&ref));
        *HashPos3 = (U32)(ip - base);
#else
        ml = LZ6HC_FindMatchFast (ctx, *HashPos, 0, ip, matchlimit, (&ref));
#endif 
        *HashPos =  (U32)(ip - base);

        if (!ml) { ip++; continue; }

        if ((U32)(ip - ref) == ctx->last_off) { ml2=0; goto _Encode; }

        {
        int back = 0;
        while ((ip+back>anchor) && (ref+back > lowPrefixPtr) && (ip[back-1] == ref[back-1])) back--;
        ml -= back;
        ip += back;
        ref += back;
        }

        /* Rep-stack preference (seq codec): same rationale as the fast
         * strategy — a rep offset within 2 bytes of the chain match saves
         * the whole offset coding. Gated on emitSeq. */
        if (ctx->emitSeq && (ctx->rep_off2 || ctx->rep_off3))
        {
            const U32 repcands[2] = { ctx->rep_off2, ctx->rep_off3 };
            for (int ri = 0; ri < 2; ri++) {
                U32 r = repcands[ri];
                if (r == 0 || r == ctx->last_off || r > (U32)(ip - lowPrefixPtr)) continue;
                const BYTE* rref = ip - r;
                int rml = (int)MEM_count(ip, rref, matchlimit);
                if (rml >= MINMATCH && rml + 2 >= ml) { ref = rref; ml = rml; break; }
            }
        }

_Search:
        if (ip+ml >= mflimit) goto _Encode;

        start2 = ip + ml - 2;
        HashPos = &HashTable[LZ6HC_hashPtr(start2, ctx->params.hashLog, ctx->params.searchLength)];
        ml2 = LZ6HC_FindMatchFaster(ctx, *HashPos, start2, matchlimit, (&ref2));      
        *HashPos = (U32)(start2 - base);
        if (!ml2) goto _Encode;

        {
        int back = 0;
        while ((start2+back>ip) && (ref2+back > lowPrefixPtr) && (start2[back-1] == ref2[back-1])) back--;
        ml2 -= back;
        start2 += back;
        ref2 += back;
        }

    //    LZ6HC_DEBUG("%u: TRY last_off=%d literals=%u off=%u mlen=%u literals2=%u off2=%u mlen2=%u best=%d\n", (U32)(ip - ctx->inputBuffer), ctx->last_off, (U32)(ip - anchor), off0, (U32)ml,  (U32)(start2 - anchor), off1, ml2, (U32)(best_pos - ip));

        if (ml2 <= ml) { ml2 = 0; goto _Encode; }

        if (start2 <= ip)
        {
            ip = start2; ref = ref2; ml = ml2;
            ml2 = 0;
            goto _Encode;
        }

        if (start2 - ip < 3) 
        { 
            ip = start2; ref = ref2; ml = ml2;
            ml2 = 0; 
            goto _Search; 
        }


        if (start2 < ip + ml) 
        {
            int correction = ml - (int)(start2 - ip);
            start2 += correction;
            ref2 += correction;
            ml2 -= correction;
            if (ml2 < 3) { ml2 = 0; }
        }
        
_Encode:
        if (LZ6HC_encodeSequence(ctx, &ip, &op, &anchor, ml, ref, limit, oend)) return 0;

        if (ml2)
        {
            ip = start2; ref = ref2; ml = ml2;
            ml2 = 0;
            goto _Search;
        }
    }

    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
        if (ctx->emitSeq)
        {
            /* sequence-extraction path: emit a final (lastRun, 0, 0) tuple */
            if (ctx->emitSeq(ctx->emitOpaque, (size_t)lastRun, 0, 0)) return 0;
        }
        else
        {
            if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;  /* Check output limit */
            if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
            else *op++ = (BYTE)(lastRun<<ML_BITS);
            memcpy(op, anchor, iend - anchor);
            op += iend-anchor;
        }
    }

    /* End */
    return (int) (((char*)op)-dest);
}



static int LZ6HC_compress_fast (
    LZ6HC_Data_Structure* ctx,
    const char* source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit
    )
{
    const BYTE* ip = (const BYTE*) source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = (iend - LASTLITERALS);

    BYTE* op = (BYTE*) dest;
    BYTE* const oend = op + maxOutputSize;

    int   ml;
    const BYTE* ref=NULL;
    const BYTE* lowPrefixPtr = ctx->base + ctx->dictLimit;
    const BYTE* const base = ctx->base;
    U32* HashPos;
    U32* HashTable  = ctx->hashTable;
	const int accel = (ctx->params.searchNum>0)?ctx->params.searchNum:1;
    
    /* init */
	ctx->inputBuffer = (const BYTE*)source;
	ctx->outputBuffer = (const BYTE*)dest;
	ctx->end += inputSize;

    ip++;

    /* Main Loop */
    while (ip < mflimit)
    {
        HashPos = &HashTable[LZ6HC_hashPtr(ip, ctx->params.hashLog, ctx->params.searchLength)];
        /* start the next probe's hash-table miss now (output-neutral, -3..4% cycles at L2) */
        if (ip + accel < mflimit)
            LZ6_PREFETCH(&HashTable[LZ6HC_hashPtr(ip + accel, ctx->params.hashLog, ctx->params.searchLength)]);
        ml = LZ6HC_FindMatchFastest (ctx, *HashPos, ip, matchlimit, (&ref));
        *HashPos =  (U32)(ip - base);
        if (!ml) { ip+=accel; continue; }

		{
			int back = 0;
			while ((ip + back > anchor) && (ref + back > lowPrefixPtr) && (ip[back - 1] == ref[back - 1])) back--;
			ml -= back;
			ip += back;
			ref += back;
		}

        /* Rep-stack preference (seq codec): an offset sitting in the MTF
         * repcache codes as a 2-bit rep flag instead of bucket+residual+low
         * bits (~1.5B saved per sequence), so a rep candidate within 2 bytes
         * of the chain match wins. Gated on emitSeq: the frame codec's parse
         * (and its baselines) stay byte-identical. */
        if (ctx->emitSeq && (ctx->rep_off2 || ctx->rep_off3))
        {
            const U32 repcands[2] = { ctx->rep_off2, ctx->rep_off3 };
            for (int ri = 0; ri < 2; ri++) {
                U32 r = repcands[ri];
                if (r == 0 || r == ctx->last_off || r > (U32)(ip - lowPrefixPtr)) continue;
                const BYTE* rref = ip - r;
                int rml = (int)MEM_count(ip, rref, matchlimit);
                if (rml >= MINMATCH && rml + 2 >= ml) { ref = rref; ml = rml; break; }
            }
        }

        /* Lazy match (seq codec benefit): if the next position has a
         * match at least 2 bytes longer, emit this position as a
         * literal (it accumulates in anchor) and let the loop take the
         * better match. Cheap: one extra hash lookup per emitted match.
         * Skipped for very long matches where a better one at ip+1 is
         * implausible. */
        if (ml < 128 && (ip + 1) < mflimit)
        {
            const BYTE* ref2 = NULL;
            U32* hp2 = &HashTable[LZ6HC_hashPtr(ip + 1, ctx->params.hashLog, ctx->params.searchLength)];
            int ml2 = LZ6HC_FindMatchFastest(ctx, *hp2, ip + 1, matchlimit, (&ref2));
            if (ml2 >= ml + 2)
            {
                ip++;   /* literal at old ip goes to anchor */
                continue;
            }
            /* two-position lazy: maybe ip+2 has the good match */
            if (ml < 64 && (ip + 2) < mflimit)
            {
                const BYTE* ref3 = NULL;
                U32* hp3 = &HashTable[LZ6HC_hashPtr(ip + 2, ctx->params.hashLog, ctx->params.searchLength)];
                int ml3 = LZ6HC_FindMatchFastest(ctx, *hp3, ip + 2, matchlimit, (&ref3));
                if (ml3 >= ml + 3)
                {
                    ip += 2;   /* two literals go to anchor */
                    continue;
                }
            }
        }

        if (LZ6HC_encodeSequence(ctx, &ip, &op, &anchor, ml, ref, limit, oend)) return 0;

    }

    /* Encode Last Literals */
    {
        int lastRun = (int)(iend - anchor);
        if (ctx->emitSeq)
        {
            /* sequence-extraction path: emit a final (lastRun, 0, 0) tuple */
            if (ctx->emitSeq(ctx->emitOpaque, (size_t)lastRun, 0, 0)) return 0;
        }
        else
        {
            if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;  /* Check output limit */
            if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
            else *op++ = (BYTE)(lastRun<<ML_BITS);
            memcpy(op, anchor, iend - anchor);
            op += iend-anchor;
        }
    }

    /* End */
    return (int) (((char*)op)-dest);
}




/* ---- row-hash match finder + lazy parser (LZ6HC_row, seq engine) ----
 * zstd 1.5's "row" match finder, simplified: the hash picks a row of
 * LZ6_ROW_SIZE positions; each slot also keeps an 8-bit tag (more hash
 * bits), so a search compares all the row's tags at once (SWAR, portable)
 * and only verifies slots whose tag matches. One cache miss yields up to
 * 16 candidates, where a chain walk takes one miss per candidate.
 * Params: sufficientLength = log2 cap on the rows' total slots (the rows
 * scale with the input below it); hashLog only sizes the context's own
 * hash table, unused here, so keep it small (it is zeroed per call);
 * searchLength = hashed bytes (4-7); searchNum = candidates verified per
 * search; fullSearch = lazy depth (0 greedy, 1 lazy, 2). */
#define LZ6_ROW_LOG  4
#define LZ6_ROW_SIZE (1u << LZ6_ROW_LOG)

typedef struct {
    U32* pos;          /* [nrows][ROW_SIZE] positions (index from base) */
    BYTE* tag;         /* [nrows][ROW_SIZE] tags */
    BYTE* head;        /* [nrows] next slot to overwrite (ring) */
    U32 rowsLog;
    U32 mls;
    U32 nextToUpdate;
} LZ6HC_rows;

FORCE_INLINE U64 LZ6HC_rowHash(const BYTE* p, U32 mls)
{
    static const U64 prime = 0xCF1BBCDCB7A56463ULL;
    const U64 v = MEM_read64(p) << (64 - 8 * mls);   /* little-endian: low mls bytes */
    return v * prime;
}

FORCE_INLINE void LZ6HC_rowInsert(LZ6HC_rows* r, const BYTE* base, U32 idx)
{
    const U64 h = LZ6HC_rowHash(base + idx, r->mls);
    const size_t row = (size_t)(h >> (64 - r->rowsLog));
    const BYTE t = (BYTE)(h >> (56 - r->rowsLog));
    const U32 slot = (U32)(r->head[row] - 1) & (LZ6_ROW_SIZE - 1);
    r->head[row] = (BYTE)slot;
    r->pos[row * LZ6_ROW_SIZE + slot] = idx;
    r->tag[row * LZ6_ROW_SIZE + slot] = t;
}

FORCE_INLINE U32 LZ6HC_highbit32(U32 v) { return 31u - (U32)__builtin_clz(v | 1u); }

/* start the row fetch for a position we will search / insert soon */
#define LZ6_ROW_AHEAD 8
FORCE_INLINE void LZ6HC_rowPrefetch(const LZ6HC_rows* r, const BYTE* p)
{
    const size_t row = (size_t)(LZ6HC_rowHash(p, r->mls) >> (64 - r->rowsLog));
    LZ6_PREFETCH(r->tag + row * LZ6_ROW_SIZE);
    LZ6_PREFETCH(r->pos + row * LZ6_ROW_SIZE);
    LZ6_PREFETCH(r->head + row);
}

FORCE_INLINE void LZ6HC_rowUpdate(LZ6HC_rows* r, const BYTE* base, U32 target, const BYTE* iLimit)
{
    U32 idx = r->nextToUpdate;
    /* after a very long match, only index its tail (zstd caps it too) */
    if (target - idx > 384) idx = target - 96;
    for (; idx < target; idx++) {
        /* the hash reads 8 bytes: prefetch only where they exist */
        if (base + idx + LZ6_ROW_AHEAD + 8 <= iLimit) LZ6HC_rowPrefetch(r, base + idx + LZ6_ROW_AHEAD);
        LZ6HC_rowInsert(r, base, idx);
    }
    r->nextToUpdate = target;
}

#if defined(__SSE2__)
#  include <emmintrin.h>
#endif
/* bit i set when byte i of x is zero (exact, no false positives) */
FORCE_INLINE U64 LZ6HC_zeroBytes(U64 x)
{
    const U64 m = 0x7F7F7F7F7F7F7F7FULL;
    return ~(((x & m) + m) | x | m);
}

/* best match at ip among the row's tag hits (newest first); returns its
 * length (0 if none >= 4) and sets *off */
FORCE_INLINE size_t LZ6HC_rowSearch(LZ6HC_Data_Structure* ctx, LZ6HC_rows* r,
                                    const BYTE* ip, const BYTE* iLimit, U32* off)
{
    const BYTE* const base = ctx->base;
    const U32 cur = (U32)(ip - base);
    const U32 maxDist = (1u << ctx->params.windowLog) - 1;
    const U32 low = (cur - ctx->dictLimit > maxDist) ? cur - maxDist : ctx->dictLimit;
    LZ6HC_rowUpdate(r, base, cur, iLimit);
    if (ip + LZ6_ROW_AHEAD + 8 <= iLimit) LZ6HC_rowPrefetch(r, ip + LZ6_ROW_AHEAD);
    const U64 h = LZ6HC_rowHash(ip, r->mls);
    const size_t row = (size_t)(h >> (64 - r->rowsLog));
    const BYTE t = (BYTE)(h >> (56 - r->rowsLog));
    const BYTE* const tg = r->tag + row * LZ6_ROW_SIZE;
    const U32* const ps = r->pos + row * LZ6_ROW_SIZE;
#if defined(__SSE2__)
    U32 mask = (U32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i*)tg), _mm_set1_epi8((char)t)));
#else
    const U64 rep = 0x0101010101010101ULL * t;
    const U64 z0 = LZ6HC_zeroBytes(MEM_read64(tg) ^ rep), z1 = LZ6HC_zeroBytes(MEM_read64(tg + 8) ^ rep);
    /* compact the high bits of each byte into a 16-bit hit mask */
    U32 mask = (U32)(((z0 >> 7) * 0x0102040810204080ULL) >> 56) | ((U32)(((z1 >> 7) * 0x0102040810204080ULL) >> 56) << 8);
#endif
    /* rotate so bit 0 is the newest slot (head) */
    const U32 hd = r->head[row];
    mask = ((mask >> hd) | (mask << (LZ6_ROW_SIZE - hd))) & 0xFFFFu;
    U32 tries = ctx->params.searchNum ? ctx->params.searchNum : 1;
    size_t best = 0;
    int bestGain = 0;
    while (mask && tries) {
        const U32 k = (U32)__builtin_ctz(mask);
        mask &= mask - 1;
        const U32 m = ps[(k + hd) & (LZ6_ROW_SIZE - 1)];
        if (m < low || m >= cur) continue;
        tries--;
        const BYTE* const mp = base + m;
        if (MEM_read32(mp) != MEM_read32(ip) || mp[best] != ip[best]) continue;
        const size_t l = MEM_count(ip + 4, mp + 4, iLimit) + 4;
        /* by gain, not length: a slightly longer match much farther away
         * costs more offset bits, and a cache miss per match to decode */
        const int g = (int)(l * 4) - (int)LZ6HC_highbit32(cur - m);
        if (g > bestGain || best == 0) { best = l; bestGain = g; *off = cur - m; if (ip + l >= iLimit) break; }
    }
    /* the row now owns ip too */
    LZ6HC_rowInsert(r, base, cur);
    r->nextToUpdate = cur + 1;
    return best;
}


static int LZ6HC_compress_row (
    LZ6HC_Data_Structure* ctx,
    const char* source,
    char* dest,
    int inputSize,
    int maxOutputSize,
    limitedOutput_directive limit)
{
    const BYTE* ip = (const BYTE*)source;
    const BYTE* anchor = ip;
    const BYTE* const iend = ip + inputSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;
    const BYTE* const base = ctx->base;
    const BYTE* const lowPrefixPtr = base + ctx->dictLimit;
    BYTE* op = (BYTE*)dest;
    BYTE* const oend = op + maxOutputSize;
    const U32 depth = ctx->params.fullSearch;

    /* rows sized to the input (~ one slot per 4 bytes) but capped at
     * 2^hashLog slots: every position is inserted, so a table that
     * outgrows the caches makes each insert a miss */
    LZ6HC_rows r;
    U32 slotsLog = LZ6HC_ceilLog2((size_t)inputSize) >= 2 ? LZ6HC_ceilLog2((size_t)inputSize) - 2 : 0;
    {
        const U32 cap = ctx->params.sufficientLength ? ctx->params.sufficientLength : 20;
        if (slotsLog > cap) slotsLog = cap;
    }
    if (slotsLog > 24) slotsLog = 24;
    if (slotsLog < LZ6_ROW_LOG + 4) slotsLog = LZ6_ROW_LOG + 4;
    r.rowsLog = slotsLog - LZ6_ROW_LOG;
    r.mls = ctx->params.searchLength < 4 ? 4 : ctx->params.searchLength > 7 ? 7 : ctx->params.searchLength;
    r.pos = (U32*)calloc((size_t)1 << slotsLog, sizeof(U32));
    r.tag = (BYTE*)calloc((size_t)1 << slotsLog, 1);
    r.head = (BYTE*)calloc((size_t)1 << r.rowsLog, 1);
    r.nextToUpdate = (U32)(ip - base);
    if (!r.pos || !r.tag || !r.head) { free(r.pos); free(r.tag); free(r.head); return 0; }

    ctx->inputBuffer = (const BYTE*)source;
    ctx->outputBuffer = (const BYTE*)dest;
    ctx->end += inputSize;

    ip++;
    while (ip < mflimit) {
        size_t ml = 0; U32 off = 0;
        const BYTE* start = ip;
        /* rep0 at ip+1 first (zstd): a hit there costs almost nothing */
        if (ctx->last_off && (size_t)(ip + 1 - lowPrefixPtr) >= ctx->last_off
            && MEM_read32(ip + 1) == MEM_read32(ip + 1 - ctx->last_off)) {
            ml = MEM_count(ip + 1 + 4, ip + 1 - ctx->last_off + 4, matchlimit) + 4;
            off = ctx->last_off; start = ip + 1;
        }
        {
            U32 o2 = 0;
            const size_t l2 = LZ6HC_rowSearch(ctx, &r, ip, matchlimit, &o2);
            /* a new offset has to beat the rep by its cost (~log2 bits) */
            if (l2 > ml && (ml == 0 || (int)(l2 * 4) - (int)LZ6HC_highbit32(o2) > (int)(ml * 4) + 1)) {
                ml = l2; off = o2; start = ip;
            }
        }
        if (ml < 4) {
            ip += ((size_t)(ip - anchor) >> 8) + 1;   /* accelerate through incompressible runs */
            continue;
        }
        /* lazy: a better match one or two bytes later wins */
        for (U32 d = 0; d < depth && start + 1 < mflimit; d++) {
            const BYTE* const nip = start + 1;
            U32 o2 = 0;
            size_t l2 = 0;
            if (off != ctx->last_off && ctx->last_off && (size_t)(nip - lowPrefixPtr) >= ctx->last_off
                && MEM_read32(nip) == MEM_read32(nip - ctx->last_off)) {
                l2 = MEM_count(nip + 4, nip - ctx->last_off + 4, matchlimit) + 4;
                o2 = ctx->last_off;
            }
            {
                U32 o3 = 0;
                const size_t l3 = LZ6HC_rowSearch(ctx, &r, nip, matchlimit, &o3);
                if (l3 > l2) { l2 = l3; o2 = o3; }
            }
            const int gain2 = (int)(l2 * 4) - (int)LZ6HC_highbit32(o2 == ctx->last_off ? 1 : o2);
            const int gain1 = (int)(ml * 4) - (int)LZ6HC_highbit32(off == ctx->last_off ? 1 : off) + 4;
            if (l2 >= 4 && gain2 > gain1) { ml = l2; off = o2; start = nip; }
            else break;
        }
        /* catch up: extend the match backwards over pending literals */
        {
            const BYTE* m = start - off;
            while (start > anchor && m > lowPrefixPtr && start[-1] == m[-1]) { start--; m--; ml++; }
        }
        ip = start;
        if (LZ6HC_encodeSequence(ctx, &ip, &op, &anchor, (int)ml, ip - off, limit, oend)) {
            free(r.pos); free(r.tag); free(r.head); return 0;
        }
    }
    free(r.pos); free(r.tag); free(r.head);

    {   /* last literals */
        int lastRun = (int)(iend - anchor);
        if (ctx->emitSeq) {
            if (ctx->emitSeq(ctx->emitOpaque, (size_t)lastRun, 0, 0)) return 0;
        } else {
            if ((limit) && (((char*)op - dest) + lastRun + 1 + ((lastRun+255-RUN_MASK)/255) > (U32)maxOutputSize)) return 0;
            if (lastRun>=(int)RUN_MASK) { *op++=(RUN_MASK<<ML_BITS); lastRun-=RUN_MASK; for(; lastRun > 254 ; lastRun-=255) *op++ = 255; *op++ = (BYTE) lastRun; }
            else *op++ = (BYTE)(lastRun<<ML_BITS);
            memcpy(op, anchor, iend - anchor);
            op += iend-anchor;
        }
    }
    return (int)(((char*)op) - dest);
}

static int LZ6HC_compress_generic (void* ctxvoid, const char* source, char* dest, int inputSize, int maxOutputSize, limitedOutput_directive limit)
{
    LZ6HC_Data_Structure* ctx = (LZ6HC_Data_Structure*) ctxvoid;

    switch(ctx->params.strategy)
    {
    default:
    case LZ6HC_fast:
        return LZ6HC_compress_fast(ctx, source, dest, inputSize, maxOutputSize, limit);
    case LZ6HC_price_fast:
        return LZ6HC_compress_price_fast(ctx, source, dest, inputSize, maxOutputSize, limit);
    case LZ6HC_lowest_price:
        return LZ6HC_compress_lowest_price(ctx, source, dest, inputSize, maxOutputSize, limit);
    case LZ6HC_row:
        return LZ6HC_compress_row(ctx, source, dest, inputSize, maxOutputSize, limit);
    case LZ6HC_optimal_price:
    case LZ6HC_optimal_price_bt:
        return LZ6HC_compress_optimal_price(ctx, (const BYTE* )source, dest, inputSize, maxOutputSize, limit);
    }
}


int LZ6_sizeofStateHC(void) { return sizeof(LZ6HC_Data_Structure); }

int LZ6_compress_HC_extStateHC (void* state, const char* src, char* dst, int srcSize, int maxDstSize)
{
    if (((size_t)(state)&(sizeof(void*)-1)) != 0) return 0;   /* Error : state is not aligned for pointers (32 or 64 bits) */
    LZ6HC_init ((LZ6HC_Data_Structure*)state, (const BYTE*)src);
    if (maxDstSize < LZ6_compressBound(srcSize))
        return LZ6HC_compress_generic (state, src, dst, srcSize, maxDstSize, limitedOutput);
    else
        return LZ6HC_compress_generic (state, src, dst, srcSize, maxDstSize, noLimit);
}


/* Sequence-extraction variant of LZ6_compress_HC_extStateHC. Same match-finding
   and parse restrictions, but no codeword is written into dst — instead the
   encoder invokes cb(opaque, lit_len, match_len, offset) for each emitted
   sequence (including a final (lastRun, 0, 0) for the trailing literals).
   The dst buffer is unused; pass NULL/0. The state is reusable across calls
   but the cb/opaque wiring is process-global per state instance. */
int LZ6HC_compress_sequences (void* state, const char* src, size_t srcSize,
                              LZ6HC_seq_cb cb, void* opaque)
{
    LZ6HC_Data_Structure* ctx;
    if (((size_t)(state)&(sizeof(void*)-1)) != 0) return 0;
    if (!cb) return 0;
    ctx = (LZ6HC_Data_Structure*)state;
    { const LZ6HC_seqPrice* sp = ctx->seqPrice;   /* LZ6HC_init clears it */
      LZ6HC_init(ctx, (const BYTE*)src);
      ctx->seqPrice = sp; }
    ctx->emitSeq  = cb;
    ctx->emitOpaque = opaque;
    /* Pass dst=NULL/0 — encodeSequence skips the codeword write when emitSeq
       is set, so the buffer is never touched. limitedOutput is a no-op here
       (no output budget to track). */
    { int rc = LZ6HC_compress_generic(state, src, NULL, (int)srcSize, 0, noLimit);
      ctx->emitSeq = NULL;
      ctx->emitOpaque = NULL;
      return rc; }
}


/* Pico 2 of the lz6→ozip pipeline: walk the input, run the match finder
   at every position, and report the candidates found via cb. The cb
   receives matches in chain-walk order (NOT sorted by length) — the
   consumer scores them itself.

   Each position's match finder does its own insertion (LZ6HC_Insert for
   chain strategy, LZ6HC_BinTree_Insert / _InsertFull for BT) so the
   internal hash + chain state stays consistent across positions. The
   same compressor-level param table drives which strategy/findNum is
   used, so the user gets the same match coverage as a full pass. The
   optimal parser's DP over the candidates is NOT run here — that's the
   consumer's job (or use LZ6HC_compress_sequences for the cheap default).

   Returns 0 on success, non-zero on error (NULL state, cb, or bad
   alignment). */
int LZ6HC_find_matches(void* state, const char* src, size_t srcSize,
                        LZ6HC_match_cb cb, void* opaque)
{
    LZ6HC_Data_Structure* ctx;
    const BYTE* ip;
    const BYTE* const iend = (const BYTE*)src + srcSize;
    const BYTE* const mflimit = iend - MFLIMIT;
    const BYTE* const matchlimit = iend - LASTLITERALS;
    LZ6HC_match_t matches[LZ6_OPT_NUM + 1];
    size_t pos;
    int rc;

    if (((size_t)(state)&(sizeof(void*)-1)) != 0) return 0;
    if (!cb) return 0;
    if (srcSize < (size_t)(MFLIMIT + LASTLITERALS)) return 0;  /* too small */
    ctx = (LZ6HC_Data_Structure*)state;
    LZ6HC_init(ctx, (const BYTE*)src);

    pos = 0;
    for (ip = (const BYTE*)src; ip < mflimit; ip++, pos++)
    {
        int n_matches;
        size_t best_mlen = 0;

        /* Insert the current position so the chain/BT links are fresh. */
        if (ctx->params.strategy == LZ6HC_optimal_price)
            LZ6HC_Insert(ctx, ip);
        else if (ctx->params.fullSearch < 2)
            LZ6HC_BinTree_Insert(ctx, ip);
        else
            LZ6HC_BinTree_InsertFull(ctx, ip, matchlimit);

        /* Find candidates at the NEXT position (ip + 1). */
        n_matches = LZ6HC_GetAllMatches(ctx, ip + 1, ip, matchlimit,
                                        best_mlen, matches);
        rc = cb(opaque, pos, ctx->last_off, matches, (size_t)n_matches);
        if (rc) return rc;
    }

    return 0;
}


int LZ6_compress_HC(const char* src, char* dst, int srcSize, int maxDstSize, int compressionLevel)
{
    LZ6HC_Data_Structure state;
    LZ6HC_Data_Structure* const statePtr = &state;
    int cSize = 0;

    if (!LZ6_alloc_mem_HC_sized(statePtr, compressionLevel, (size_t)(srcSize > 0 ? srcSize : 1)))
        return 0;
    cSize = LZ6_compress_HC_extStateHC(statePtr, src, dst, srcSize, maxDstSize);

    LZ6_free_mem_HC(statePtr);

    return cSize;
}



/**************************************
*  Streaming Functions
**************************************/
/* allocation */
/* Size the context's window/chain table to maxBlockSize (fixed for its life). */
LZ6_streamHC_t* LZ6_createStreamHC_sized(int compressionLevel, size_t maxBlockSize)
{
    LZ6_streamHC_t* statePtr = (LZ6_streamHC_t*)malloc(sizeof(LZ6_streamHC_t));
    if (!statePtr)
        return NULL;

    if (!LZ6_alloc_mem_HC_sized((LZ6HC_Data_Structure*)statePtr, compressionLevel, maxBlockSize))
    {
        FREEMEM(statePtr);
        return NULL;
    }
    return statePtr;
}

LZ6_streamHC_t* LZ6_createStreamHC(int compressionLevel)
{
    return LZ6_createStreamHC_sized(compressionLevel, (size_t)1 << MAXD_LOG);
}

int LZ6_freeStreamHC (LZ6_streamHC_t* LZ6_streamHCPtr)
{
    LZ6HC_Data_Structure* statePtr = (LZ6HC_Data_Structure*)LZ6_streamHCPtr;
    if (statePtr)
    {
        LZ6_free_mem_HC(statePtr);
        free(LZ6_streamHCPtr); 
    }
    return 0; 
}


/* initialization */
void LZ6_resetStreamHC (LZ6_streamHC_t* LZ6_streamHCPtr)
{
    LZ6_STATIC_ASSERT(sizeof(LZ6HC_Data_Structure) <= sizeof(LZ6_streamHC_t));   /* if compilation fails here, LZ6_STREAMHCSIZE must be increased */
    ((LZ6HC_Data_Structure*)LZ6_streamHCPtr)->base = NULL;
}

int LZ6_loadDictHC (LZ6_streamHC_t* LZ6_streamHCPtr, const char* dictionary, int dictSize)
{
    LZ6HC_Data_Structure* ctxPtr = (LZ6HC_Data_Structure*) LZ6_streamHCPtr;
    if (dictSize > LZ6_DICT_SIZE)
    {
        dictionary += dictSize - LZ6_DICT_SIZE;
        dictSize = LZ6_DICT_SIZE;
    }
    LZ6HC_init (ctxPtr, (const BYTE*)dictionary);
    if (dictSize >= 4) LZ6HC_Insert (ctxPtr, (const BYTE*)dictionary +(dictSize-3));
    ctxPtr->end = (const BYTE*)dictionary + dictSize;
    return dictSize;
}


/* compression */

static void LZ6HC_setExternalDict(LZ6HC_Data_Structure* ctxPtr, const BYTE* newBlock)
{
    if (ctxPtr->end >= ctxPtr->base + 4)
        LZ6HC_Insert (ctxPtr, ctxPtr->end-3);   /* Referencing remaining dictionary content */
    /* Only one memory segment for extDict, so any previous extDict is lost at this stage */
    ctxPtr->lowLimit  = ctxPtr->dictLimit;
    ctxPtr->dictLimit = (U32)(ctxPtr->end - ctxPtr->base);
    ctxPtr->dictBase  = ctxPtr->base;
    ctxPtr->base = newBlock - ctxPtr->dictLimit;
    ctxPtr->end  = newBlock;
    ctxPtr->nextToUpdate = ctxPtr->dictLimit;   /* match referencing will resume from there */
}

static int LZ6_compressHC_continue_generic (LZ6HC_Data_Structure* ctxPtr,
                                            const char* source, char* dest,
                                            int inputSize, int maxOutputSize, limitedOutput_directive limit)
{
    /* auto-init if forgotten */
    if (ctxPtr->base == NULL)
        LZ6HC_init (ctxPtr, (const BYTE*) source);

    /* Check overflow */
    if ((size_t)(ctxPtr->end - ctxPtr->base) > 2 GB)
    {
        size_t dictSize = (size_t)(ctxPtr->end - ctxPtr->base) - ctxPtr->dictLimit;
        if (dictSize > LZ6_DICT_SIZE) dictSize = LZ6_DICT_SIZE;

        LZ6_loadDictHC((LZ6_streamHC_t*)ctxPtr, (const char*)(ctxPtr->end) - dictSize, (int)dictSize);
    }

    /* Check if blocks follow each other */
    if ((const BYTE*)source != ctxPtr->end)
        LZ6HC_setExternalDict(ctxPtr, (const BYTE*)source);

    /* Check overlapping input/dictionary space */
    {
        const BYTE* sourceEnd = (const BYTE*) source + inputSize;
        const BYTE* dictBegin = ctxPtr->dictBase + ctxPtr->lowLimit;
        const BYTE* dictEnd   = ctxPtr->dictBase + ctxPtr->dictLimit;
        if ((sourceEnd > dictBegin) && ((const BYTE*)source < dictEnd))
        {
            if (sourceEnd > dictEnd) sourceEnd = dictEnd;
            ctxPtr->lowLimit = (U32)(sourceEnd - ctxPtr->dictBase);
            if (ctxPtr->dictLimit - ctxPtr->lowLimit < 4) ctxPtr->lowLimit = ctxPtr->dictLimit;
        }
    }

    return LZ6HC_compress_generic (ctxPtr, source, dest, inputSize, maxOutputSize, limit);
}

int LZ6_compress_HC_continue (LZ6_streamHC_t* LZ6_streamHCPtr, const char* source, char* dest, int inputSize, int maxOutputSize)
{
    if (maxOutputSize < LZ6_compressBound(inputSize))
        return LZ6_compressHC_continue_generic ((LZ6HC_Data_Structure*)LZ6_streamHCPtr, source, dest, inputSize, maxOutputSize, limitedOutput);
    else
        return LZ6_compressHC_continue_generic ((LZ6HC_Data_Structure*)LZ6_streamHCPtr, source, dest, inputSize, maxOutputSize, noLimit);
}


/* dictionary saving */

int LZ6_saveDictHC (LZ6_streamHC_t* LZ6_streamHCPtr, char* safeBuffer, int dictSize)
{
    LZ6HC_Data_Structure* streamPtr = (LZ6HC_Data_Structure*)LZ6_streamHCPtr;
    int prefixSize = (int)(streamPtr->end - (streamPtr->base + streamPtr->dictLimit));
    if (dictSize > LZ6_DICT_SIZE) dictSize = LZ6_DICT_SIZE;
  //  if (dictSize < 4) dictSize = 0;
    if (dictSize > prefixSize) dictSize = prefixSize;
    memmove(safeBuffer, streamPtr->end - dictSize, dictSize);
    {
        U32 endIndex = (U32)(streamPtr->end - streamPtr->base);
        streamPtr->end = (const BYTE*)safeBuffer + dictSize;
        streamPtr->base = streamPtr->end - endIndex;
        streamPtr->dictLimit = endIndex - dictSize;
        streamPtr->lowLimit = endIndex - dictSize;
        if (streamPtr->nextToUpdate < streamPtr->dictLimit) streamPtr->nextToUpdate = streamPtr->dictLimit;
    }
    return dictSize;
}

/***********************************
*  Deprecated Functions
***********************************/
/* Deprecated compression functions */
/* These functions are planned to start generate warnings by r132 approximately */
int LZ6_compressHC(const char* src, char* dst, int srcSize) { return LZ6_compress_HC (src, dst, srcSize, LZ6_compressBound(srcSize), 0); }
int LZ6_compressHC_limitedOutput(const char* src, char* dst, int srcSize, int maxDstSize) { return LZ6_compress_HC(src, dst, srcSize, maxDstSize, 0); }
int LZ6_compressHC_continue (LZ6_streamHC_t* ctx, const char* src, char* dst, int srcSize) { return LZ6_compress_HC_continue (ctx, src, dst, srcSize, LZ6_compressBound(srcSize)); }
int LZ6_compressHC_limitedOutput_continue (LZ6_streamHC_t* ctx, const char* src, char* dst, int srcSize, int maxDstSize) { return LZ6_compress_HC_continue (ctx, src, dst, srcSize, maxDstSize); } 
int LZ6_compressHC_withStateHC (void* state, const char* src, char* dst, int srcSize) { return LZ6_compress_HC_extStateHC (state, src, dst, srcSize, LZ6_compressBound(srcSize)); }
int LZ6_compressHC_limitedOutput_withStateHC (void* state, const char* src, char* dst, int srcSize, int maxDstSize) { return LZ6_compress_HC_extStateHC (state, src, dst, srcSize, maxDstSize); } 
