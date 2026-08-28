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
                               size_t maxSrcSize, int maxWindowLog)
{
    ctx->compressionLevel = compressionLevel;
    if (compressionLevel > g_maxCompressionLevel) ctx->compressionLevel = g_maxCompressionLevel;
    if (compressionLevel < 1) ctx->compressionLevel = LZ6HC_compressionLevel_default;

    ctx->params = LZ6HC_defaultParameters[ctx->compressionLevel];
    /* seq codec: the format carries offsets up to 2^25-1 (bucket 24), so a
     * bigger window is allowed there without touching the frame codec */
    if (maxWindowLog > (int)ctx->params.windowLog)
    {
        U32 delta = ctx->params.contentLog - ctx->params.windowLog;
        ctx->params.windowLog  = (U32)maxWindowLog;
        ctx->params.contentLog = (U32)maxWindowLog + delta;
    }
    LZ6HC_capParamsToSize(&ctx->params, maxSrcSize);

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


#define SET_PRICE(pos, mlen, offset, litlen, price)   \
    {                                                 \
        while (last_pos < pos)  { opt[last_pos+1].price = 1<<30; last_pos++; } \
        opt[pos].mlen = (int)mlen;                         \
        opt[pos].off = (int)offset;                        \
        opt[pos].litlen = (int)litlen;                     \
        opt[pos].price = (int)price;                       \
        LZ6_LOG_PARSER("%d: SET price[%d/%d]=%d litlen=%d len=%d off=%d\n", (int)(inr-source), pos, last_pos, opt[pos].price, opt[pos].litlen, opt[pos].mlen, opt[pos].off); \
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
                price = LZ6HC_get_price(llen, 0, mlen - MINMATCH) - llen;
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
       for (i = 0; i < match_num; i++)
       {
           mlen = (i>0) ? (size_t)matches[i-1].len+1 : best_mlen;
           best_mlen = (matches[i].len < LZ6_OPT_NUM) ? matches[i].len : LZ6_OPT_NUM;
           LZ6_LOG_PARSER("%d: start Found mlen=%d off=%d best_mlen=%d last_pos=%d\n", (int)(ip-source), matches[i].len, matches[i].off, best_mlen, last_pos);
           while (mlen <= best_mlen)
           {
                litlen = 0;
                price = LZ6HC_get_price(llen + litlen, matches[i].off, mlen - MINMATCH) - llen;
                if (mlen > last_pos || price < (size_t)opt[mlen].price)
                    SET_PRICE(mlen, mlen, matches[i].off, litlen, price);
                mlen++;
           }
        }

        if (last_pos < MINMATCH) { ip++; continue; }

        opt[0].rep = opt[1].rep = ctx->last_off;
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
                    price = opt[cur - litlen].price + LZ6_LIT_ONLY_COST(litlen);
                    LZ6_LOG_PRICE("%d: TRY1 opt[%d].price=%d price=%d cur=%d litlen=%d\n", (int)(inr-source), cur - litlen, opt[cur - litlen].price, price, cur, litlen);
                }
                else
                {
                    price = LZ6_LIT_ONLY_COST(llen + litlen) - llen;
                    LZ6_LOG_PRICE("%d: TRY2 price=%d cur=%d litlen=%d llen=%d\n", (int)(inr-source), price, cur, litlen, llen);
                }
           }
           else
           {
                litlen = 1;
                price = opt[cur - 1].price + LZ6_LIT_ONLY_COST(litlen);                  
                LZ6_LOG_PRICE("%d: TRY3 price=%d cur=%d litlen=%d litonly=%d\n", (int)(inr-source), price, cur, litlen, LZ6_LIT_ONLY_COST(litlen));
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
                if (offset < 1)
                {
                    opt[cur].rep = opt[cur-mlen].rep;
                    LZ6_LOG_PARSER("%d: COPYREP1 cur=%d mlen=%d rep=%d\n", (int)(inr-source), cur, mlen, opt[cur-mlen].rep);
                }
                else
                {
                    opt[cur].rep = (int)offset;
                    LZ6_LOG_PARSER("%d: COPYREP2 cur=%d offset=%d rep=%d\n", (int)(inr-source), cur, offset, opt[cur].rep);
                }
           }
           else
           {
                opt[cur].rep = opt[cur-1].rep; // copy rep
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
                        price = opt[cur - litlen].price + LZ6HC_get_price(litlen, 0, mlen - MINMATCH);
                        LZ6_LOG_PRICE("%d: TRY1 opt[%d].price=%d price=%d cur=%d litlen=%d\n", (int)(inr-source), cur - litlen, opt[cur - litlen].price, price, cur, litlen);
                    }
                    else
                    {
                        price = LZ6HC_get_price(llen + litlen, 0, mlen - MINMATCH) - llen;
                        LZ6_LOG_PRICE("%d: TRY2 price=%d cur=%d litlen=%d llen=%d\n", (int)(inr-source), price, cur, litlen, llen);
                    }
                }
                else
                {
                    litlen = 0;
                    price = opt[cur].price + LZ6HC_get_price(litlen, 0, mlen - MINMATCH);
                    LZ6_LOG_PRICE("%d: TRY3 price=%d cur=%d litlen=%d getprice=%d\n", (int)(inr-source), price, cur, litlen, LZ6HC_get_price(litlen, 0, mlen - MINMATCH));
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

                while (mlen <= best_mlen)
                {
                    if (opt[cur2].mlen == 1)
                    {
                        litlen = opt[cur2].litlen;

                        if (cur2 != litlen)
                            price = opt[cur2 - litlen].price + LZ6HC_get_price(litlen, matches[i].off, mlen - MINMATCH);
                        else
                            price = LZ6HC_get_price(llen + litlen, matches[i].off, mlen - MINMATCH) - llen;
                    }
                    else
                    {
                        litlen = 0;
                        price = opt[cur2].price + LZ6HC_get_price(litlen, matches[i].off, mlen - MINMATCH);
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
    LZ6HC_init(ctx, (const BYTE*)src);
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
