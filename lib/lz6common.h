#ifndef LZ6COMMON_H
#define LZ6COMMON_H

#if defined (__cplusplus)
extern "C" {
#endif


/**************************************
*  Tuning parameters
**************************************/
/*
 * HEAPMODE :
 * Select how default compression functions will allocate memory for their hash table,
 * in memory stack (0:default, fastest), or in memory heap (1:requires malloc()).
 */
#ifdef _MSC_VER
	#define HEAPMODE 1   /* Default stack size for VC++ is 1 MB and size of LZ6_stream_t exceeds that limit */ 
#else
	#define HEAPMODE 0
#endif


/*
 * ACCELERATION_DEFAULT :
 * Select "acceleration" for LZ6_compress_fast() when parameter value <= 0
 */
#define ACCELERATION_DEFAULT 1




/**************************************
*  Compiler Options
**************************************/
#ifdef _MSC_VER    /* Visual Studio */
#  define FORCE_INLINE static __forceinline
#  include <intrin.h>
#  pragma warning(disable : 4127)        /* disable: C4127: conditional expression is constant */
#  pragma warning(disable : 4293)        /* disable: C4293: too large shift (32-bits) */
#else
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)   /* C99 */
#    if defined(__GNUC__) || defined(__clang__)
#      define FORCE_INLINE static inline __attribute__((always_inline))
#    else
#      define FORCE_INLINE static inline
#    endif
#  else
#    define FORCE_INLINE static
#  endif   /* __STDC_VERSION__ */
#endif  /* _MSC_VER */

#define LZ6_GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)

#if (LZ6_GCC_VERSION >= 302) || (__INTEL_COMPILER >= 800) || defined(__clang__)
#  define expect(expr,value)    (__builtin_expect ((expr),(value)) )
#else
#  define expect(expr,value)    (expr)
#endif

#define likely(expr)     expect((expr) != 0, 1)
#define unlikely(expr)   expect((expr) != 0, 0)



/**************************************
*  Memory routines
**************************************/
#include <stdlib.h>   /* malloc, calloc, free */
#define ALLOCATOR(n,s) calloc(n,s)
#define FREEMEM        free
#include <string.h>   /* memset, memcpy */
#define MEM_INIT       memset


/**************************************
*  Common Constants
**************************************/
#define MINMATCH 3 // should be 3 or 4

#define WILDCOPYLENGTH 8
#define LASTLITERALS 5
#define MFLIMIT (WILDCOPYLENGTH+MINMATCH)
static const int LZ6_minLength = (MFLIMIT+1);

#define KB *(1 <<10)
#define MB *(1 <<20)
#define GB *(1U<<30)

#define MAXD_LOG 24
#define MAX_DISTANCE ((1 << MAXD_LOG) - 1)
#define LZ6_DICT_SIZE (1 << MAXD_LOG)

#define ML_BITS  3
#define ML_MASK  ((1U<<ML_BITS)-1)
#define RUN_BITS 3
#define RUN_MASK ((1U<<RUN_BITS)-1)
#define RUN_BITS2 2
#define RUN_MASK2 ((1U<<RUN_BITS2)-1)
#define ML_RUN_BITS (ML_BITS + RUN_BITS)
#define ML_RUN_BITS2 (ML_BITS + RUN_BITS2)

#define LZ6_SHORT_OFFSET_BITS 10
#define LZ6_SHORT_OFFSET_DISTANCE (1<<LZ6_SHORT_OFFSET_BITS)
#define LZ6_MID_OFFSET_BITS 16
#define LZ6_MID_OFFSET_DISTANCE (1<<LZ6_MID_OFFSET_BITS)


/**************************************
*  Common Utils
**************************************/
#define LZ6_STATIC_ASSERT(c)    { enum { LZ6_static_assert = 1/(int)(!!(c)) }; }   /* use only *after* variable declarations */



/****************************************************************
*  Basic Types
*****************************************************************/
#if defined (__cplusplus) || (defined (__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */)
# include <stdint.h>
  typedef  uint8_t BYTE;
  typedef uint16_t U16;
  typedef  int16_t S16;
  typedef uint32_t U32;
  typedef  int32_t S32;
  typedef uint64_t U64;
  typedef  int64_t S64;
#else
  typedef unsigned char       BYTE;
  typedef unsigned short      U16;
  typedef   signed short      S16;
  typedef unsigned int        U32;
  typedef   signed int        S32;
  typedef unsigned long long  U64;
  typedef   signed long long  S64;
#endif


/* *************************************
*  HC Inline functions and Macros
***************************************/
#include "mem.h" // MEM_read
#include "lz6.h" // LZ6HC_MAX_CLEVEL

    
static const U32 prime4bytes = 2654435761U;
static const U64 prime5bytes = 889523592379ULL;

#ifdef LZ6HC_INCLUDES
static const U32 prime3bytes = 506832829U;
static const U64 prime6bytes = 227718039650203ULL;
static const U64 prime7bytes = 58295818150454627ULL;

static U32 LZ6HC_hash3(U32 u, U32 h) { return (u * prime3bytes) << (32-24) >> (32-h) ; }
static size_t LZ6HC_hash3Ptr(const void* ptr, U32 h) { return LZ6HC_hash3(MEM_read32(ptr), h); }

static U32 LZ6HC_hash4(U32 u, U32 h) { return (u * prime4bytes) >> (32-h) ; }
static size_t LZ6HC_hash4Ptr(const void* ptr, U32 h) { return LZ6HC_hash4(MEM_read32(ptr), h); }

static size_t LZ6HC_hash5(U64 u, U32 h) { return (size_t)((u * prime5bytes) << (64-40) >> (64-h)) ; }
static size_t LZ6HC_hash5Ptr(const void* p, U32 h) { return LZ6HC_hash5(MEM_read64(p), h); }

static size_t LZ6HC_hash6(U64 u, U32 h) { return (size_t)((u * prime6bytes) << (64-48) >> (64-h)) ; }
static size_t LZ6HC_hash6Ptr(const void* p, U32 h) { return LZ6HC_hash6(MEM_read64(p), h); }

static size_t LZ6HC_hash7(U64 u, U32 h) { return (size_t)((u * prime7bytes) << (64-56) >> (64-h)) ; }
static size_t LZ6HC_hash7Ptr(const void* p, U32 h) { return LZ6HC_hash7(MEM_read64(p), h); }

FORCE_INLINE size_t LZ6HC_hashPtr(const void* p, U32 hBits, U32 mls)
{
    switch(mls)
    {
    default:
    case 4: return LZ6HC_hash4Ptr(p, hBits);
    case 5: return LZ6HC_hash5Ptr(p, hBits);
    case 6: return LZ6HC_hash6Ptr(p, hBits);
    case 7: return LZ6HC_hash7Ptr(p, hBits);
    }
}


/**************************************
*  HC Local Macros
**************************************/
#define LZ6HC_DEBUG(fmt, ...) //printf(fmt, __VA_ARGS__)
#define LZ6_LOG_PARSER(fmt, ...) //printf(fmt, __VA_ARGS__)
#define LZ6_LOG_PRICE(fmt, ...) //printf(fmt, __VA_ARGS__)
#define LZ6_LOG_ENCODE(fmt, ...) //printf(fmt, __VA_ARGS__)

#define MAX(a,b) ((a)>(b))?(a):(b)
#define LZ6_OPT_NUM   (1<<12)

#define LZ6_SHORT_LITERALS          ((1<<RUN_BITS2)-1)
#define LZ6_LITERALS                ((1<<RUN_BITS)-1)

#define LZ6_SHORT_LITLEN_COST(len)  (len<LZ6_SHORT_LITERALS ? 0 : (len-LZ6_SHORT_LITERALS < 255 ? 1 : (len-LZ6_SHORT_LITERALS-255 < (1<<7) ? 2 : 3)))
#define LZ6_LEN_COST(len)           (len<LZ6_LITERALS ? 0 : (len-LZ6_LITERALS < 255 ? 1 : (len-LZ6_LITERALS-255 < (1<<7) ? 2 : 3)))

static size_t LZ6_LIT_COST(size_t len, size_t offset){ return (len)+(((offset > LZ6_MID_OFFSET_DISTANCE) || (offset<LZ6_SHORT_OFFSET_DISTANCE)) ? LZ6_SHORT_LITLEN_COST(len) : LZ6_LEN_COST(len)); }
static size_t LZ6_MATCH_COST(size_t mlen, size_t offset) { return LZ6_LEN_COST(mlen) + ((offset == 0) ? 1 : (offset<LZ6_SHORT_OFFSET_DISTANCE ? 2 : (offset<LZ6_MID_OFFSET_DISTANCE ? 3 : 4))); }

#define LZ6_CODEWORD_COST(litlen,offset,mlen)   (LZ6_MATCH_COST(mlen,offset) + LZ6_LIT_COST(litlen,offset))
#define LZ6_LIT_ONLY_COST(len)                  ((len)+(LZ6_LEN_COST(len))+1)

#define LZ6_NORMAL_MATCH_COST(mlen,offset)  (LZ6_MATCH_COST(mlen,offset))
#define LZ6_NORMAL_LIT_COST(len)            (len)



FORCE_INLINE size_t LZ6HC_get_price(size_t litlen, size_t offset, size_t mlen)
{
	return LZ6_CODEWORD_COST(litlen, offset, mlen);
}

FORCE_INLINE size_t LZ6HC_better_price(size_t best_off, size_t best_common, size_t off, size_t common, size_t last_off)
{
  return LZ6_NORMAL_MATCH_COST(common - MINMATCH, (off == last_off) ? 0 : off) < LZ6_NORMAL_MATCH_COST(best_common - MINMATCH, (best_off == last_off) ? 0 : best_off) + (LZ6_NORMAL_LIT_COST(common - best_common) );
}


FORCE_INLINE size_t LZ6HC_more_profitable(size_t best_off, size_t best_common, size_t off, size_t common, size_t literals, size_t last_off)
{
	size_t sum;
	
	if (literals > 0)
		sum = MAX(common + literals, best_common);
	else
		sum = MAX(common, best_common - literals);
	
//	return LZ6_CODEWORD_COST(sum - common, (off == last_off) ? 0 : (off), common - MINMATCH) <= LZ6_CODEWORD_COST(sum - best_common, (best_off == last_off) ? 0 : (best_off), best_common - MINMATCH);
	return LZ6_NORMAL_MATCH_COST(common - MINMATCH, (off == last_off) ? 0 : off) + LZ6_NORMAL_LIT_COST(sum - common) <= LZ6_NORMAL_MATCH_COST(best_common - MINMATCH, (best_off == last_off) ? 0 : (best_off)) + LZ6_NORMAL_LIT_COST(sum - best_common);
}

#endif // LZ6HC_INCLUDES



/* *************************************
*  HC Types
***************************************/
/** from faster to stronger */
/* LZ6HC_row (seq engine only): row-hash match finder + lazy parser, see
 * LZ6HC_compress_row in lz6hc.c. Appended last so the numeric values of the
 * other strategies (tuning hook) stay the same; compare strategies by name,
 * not by order, where row matters. */
typedef enum { LZ6HC_fast, LZ6HC_price_fast, LZ6HC_lowest_price, LZ6HC_optimal_price, LZ6HC_optimal_price_bt, LZ6HC_row } LZ6HC_strategy;

typedef struct
{
    U32 windowLog;     /* largest match distance : impact decompression buffer size */
    U32 contentLog;    /* full search segment : larger == more compression, slower, more memory (useless for fast) */
    U32 hashLog;       /* dispatch table : larger == more memory, faster*/
    U32 hashLog3;      /* dispatch table : larger == more memory, faster*/
    U32 searchNum;     /* nb of searches : larger == more compression, slower*/
    U32 searchLength;  /* size of matches : larger == faster decompression */
    U32 sufficientLength;  /* used only by optimal parser: size of matches which is acceptable: larger == more compression, slower */
    U32 fullSearch;    /* used only by optimal parser: perform full search of matches: 1 == more compression, slower */
    LZ6HC_strategy strategy;
} LZ6HC_parameters;


struct LZ6HC_Data_s
{
    U32*   hashTable;
    U32*   hashTable3;
    U32*   chainTable;
    const BYTE* end;        /* next block here to continue on current prefix */
    const BYTE* base;       /* All index relative to this position */
    const BYTE* dictBase;   /* alternate base for extDict */
    const BYTE* inputBuffer;      /* for debugging */
    const BYTE* outputBuffer;     /* for debugging */
    U32   dictLimit;        /* below that point, need extDict */
    U32   lowLimit;         /* below that point, no more dict */
    U32   nextToUpdate;     /* index from which to continue dictionary update */
    U32   compressionLevel;
    U32   last_off;
    /* MTF rep stack positions 1..2 (rep_off mirrors last_off == position 0).
     * Maintained only on the emitSeq (sequence-extraction) path, mirroring
     * the entropy encoder's repcache, so the parser can prefer offsets that
     * code as cheap 2-bit rep flags. */
    U32   rep_off2;
    U32   rep_off3;
    LZ6HC_parameters params;
    /* Sequence-emit hook (Paso 1 of the lz6→ozip pipeline). When emitSeq is
       non-NULL, the encoder skips writing the literal+match codeword and
       instead calls emitSeq(emitOpaque, lit_len, match_len, offset) for each
       emitted sequence — including a final (lastRun, 0, 0) for the trailing
       literals. offset is the raw match offset (1 == immediate repeat); the
       entropy coder picks whether to render it as a 1B rep codeword. */
    int (*emitSeq)(void* opaque, size_t lit_len, size_t match_len, size_t offset);
    void* emitOpaque;
    /* entropy-aware parser prices (seq path only; NULL = codeword bytes) */
    const struct LZ6HC_seqPrice_s* seqPrice;
};

/* LZ6HC_match_t is now defined in lz6hc.h (the public header). This
   internal redeclaration is kept under a guard so external callers that
   only include lz6common.h don't break. */
#if !defined(LZ6HC_MATCH_T_DEFINED)
#define LZ6HC_MATCH_T_DEFINED
typedef struct LZ6HC_match_s {
	int off;
	int len;
	int back;
} LZ6HC_match_t;
#endif

typedef struct
{
	int price;
	int off;
	int mlen;
	int litlen;
   	int rep;
	/* hypothetical MTF rep-stack positions 1..2 at this DP position
	 * (rep == position 0 / last_off). Maintained by the optimal parser's
	 * transitions so rep1/rep2 candidates can be priced and tracked. */
	int rep2;
	int rep3;
} LZ6HC_optimal_t;



/* *************************************
*  HC Pre-defined compression levels
***************************************/

static const int g_maxCompressionLevel = LZ6HC_MAX_CLEVEL;
static const int LZ6HC_compressionLevel_default = 6;

static const LZ6HC_parameters LZ6HC_defaultParameters[LZ6HC_MAX_CLEVEL+1] =
{
    /* windLog, contentLog,  H, H3,  Snum, SL, SuffL, FS, Strategy */
    {        0,          0,  0,  0,     0,  0,     0,  0, LZ6HC_fast             }, // level 0 - never used
    { MAXD_LOG,   MAXD_LOG, 13,  0,     4,  6,     0,  0, LZ6HC_fast             }, // level 1
    { MAXD_LOG,   MAXD_LOG, 13,  0,     2,  6,     0,  0, LZ6HC_fast             }, // level 2
    { MAXD_LOG,   MAXD_LOG, 13,  0,     1,  5,     0,  0, LZ6HC_fast             }, // level 3
    { MAXD_LOG,   MAXD_LOG, 14, 13,     1,  4,     0,  0, LZ6HC_price_fast       }, // level 4
    { MAXD_LOG,   MAXD_LOG, 17, 13,     1,  4,     0,  0, LZ6HC_price_fast       }, // level 5
    { MAXD_LOG,   MAXD_LOG, 15, 13,     1,  4,     0,  0, LZ6HC_lowest_price     }, // level 6
    { MAXD_LOG,   MAXD_LOG, 17, 13,     1,  4,     0,  0, LZ6HC_lowest_price     }, // level 7
    { MAXD_LOG,   MAXD_LOG, 19, 16,     1,  4,     0,  0, LZ6HC_lowest_price     }, // level 8
    { MAXD_LOG,   MAXD_LOG, 23, 16,     3,  4,     0,  0, LZ6HC_lowest_price     }, // level 9
    { MAXD_LOG,   MAXD_LOG, 23, 16,     8,  4,     0,  0, LZ6HC_lowest_price     }, // level 10
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    32,  4,    48,  1, LZ6HC_optimal_price_bt }, // level 11 — BT fs=1
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    48,  1, LZ6HC_optimal_price_bt }, // level 12 — BT fs=1
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 13
    { MAXD_LOG, MAXD_LOG+1, 23, 16,   128,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 14
    { MAXD_LOG, MAXD_LOG+1, 23, 16,  1024,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 15
    // Recalibrated 2026-06: sufficientLength sweet-spot is ~32 (higher HURTS ratio, against
    // the old comment); searchNum scales the L11-14 chains ladder monotonically.
    // L15 re-flipped to the binary-tree finder 2026-07 (post rep-fix + adaptive-window):
    // measured vs chains-sn256 at L15, BT-sn1024-fs2 is smaller on dictionary/text corpora
    // (corpus8 -1.9%, sil40 -0.8%) AND 4.6-6.7x faster to encode; on high-entropy binary
    // content it is ~0.4-0.5% LARGER than L13/L14 chains — the two finders trade wins by
    // content type and no strategy dominates, so strict per-file level monotonicity is not
    // guaranteed at the L14->L15 step (use L14 for max ratio on binary-heavy data).
    // BT saturates in searchNum (256..1024 near-identical output); fs=2 (InsertFull) buys
    // a little more ratio everywhere at ~1.5x BT encode time — still far cheaper than chains.
    // 2026-07-24 L13-14 also flipped to BT (fs=2): L13 +62% encode speed (2.1->3.4 MB/s),
    // L14 +192% (1.3->3.8 MB/s) on sil40.dat, ratio within 0.05pp. L11-12 stay on chains:
    // BT insertion overhead exceeds chain walk at searchNum<=64 (L11 -34% on sil40).
//  {       10,         10, 10,  0,     0,  4,     0,  0, LZ6HC_fast          }, // min values
//  {       24,         24, 28, 24, 1<<24,  7, 1<<24,  2, LZ6HC_optimal_price }, // max values
};

/* Seq-engine levels (levels 1-15 without --hc). Kept apart from the frame
 * codec's table above so the seq ladder can be tuned while the frame HC
 * codec (the frozen portable profile) keeps its exact output. */
/* The optimal levels' price pre-parse (LZ6HC_SEQ_PRE_LEVEL): its own row,
 * so retuning L3 does not move L8-L15. price_fast with a 6-byte hash: its
 * statistics priced Silesia best among the fast parsers (row-hash and
 * fast-parser statistics priced it 0.2-0.3 points worse at L12). */
static const LZ6HC_parameters LZ6HC_seqPreParameters =
    { MAXD_LOG,   MAXD_LOG, 15, 13,     1,  6,     0,  0, LZ6HC_price_fast       };

static const LZ6HC_parameters LZ6HC_seqParameters[LZ6HC_MAX_CLEVEL+1] =
{
    /* Retuned 2026-09-24 as a strictly monotonic ladder (ratio and encode
     * speed both fall at every step), measured single-core on a Silesia
     * subset (dickens/samba/osdb/ooffice/xml) with the entropy-priced
     * optimal parser. Levels 13-15 also re-parse with refined prices
     * (seq_refine_passes in lz6seq.c): past ~64 searches the depth stops
     * helping (256..1024 searches gave the same size) and the refinement
     * passes are what move the ratio. 2026-09-25: L3-L7 retuned from a
     * sweep of the existing parsers (L3 price_fast; L4-L7 lowest_price
     * with 6-byte hashes and 1/2/4/8 candidates: every level smaller and no
     * slower than before). L3-L5 then moved to the row-hash match finder
     * (LZ6HC_row; SufL = log2 cap of its row slots, FS = lazy depth): up
     * to 2x faster than lowest_price at the same ratio. The optimal
     * levels' price pre-parse has its own row (LZ6HC_seqPreParameters).
     * Full Silesia, L1 -> L15: 33.43% -> 25.45%; AIT 42.40% -> 38.83%;
     * every step smaller than the one before.
     * windLog, contentLog,  H, H3,  Snum, SL, SuffL, FS, Strategy                   subset ratio / MB/s */
    {        0,          0,  0,  0,     0,  0,     0,  0, LZ6HC_fast             }, // level 0 - never used
    { MAXD_LOG,   MAXD_LOG, 13,  0,     4,  6,     0,  0, LZ6HC_fast             }, // level 1   33.42% / 95
    { MAXD_LOG,   MAXD_LOG, 13,  0,     2,  6,     0,  0, LZ6HC_fast             }, // level 2   31.95% / 88
    { MAXD_LOG,   MAXD_LOG, 10,  0,     8,  5,    22,  0, LZ6HC_row              }, // level 3   28.64% / 57
    { MAXD_LOG,   MAXD_LOG, 10,  0,     8,  5,    22,  1, LZ6HC_row              }, // level 4   28.12% / 45
    { MAXD_LOG,   MAXD_LOG, 10,  0,    16,  5,    23,  2, LZ6HC_row              }, // level 5   27.71% / 38
    { MAXD_LOG,   MAXD_LOG, 15, 13,     4,  6,     0,  0, LZ6HC_lowest_price     }, // level 6   27.44% / 17
    { MAXD_LOG,   MAXD_LOG, 15, 13,     8,  6,     0,  0, LZ6HC_lowest_price     }, // level 7   27.19% / 14
    { MAXD_LOG, MAXD_LOG+1, 23, 16,     8,  4,    32,  0, LZ6HC_optimal_price_bt }, // level 8   27.07% / 7.1
    { MAXD_LOG, MAXD_LOG+1, 23, 16,     8,  4,    32,  1, LZ6HC_optimal_price_bt }, // level 9   26.67% / 5.0
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    16,  4,    48,  1, LZ6HC_optimal_price_bt }, // level 10  25.57% / 3.6
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    32,  4,    48,  1, LZ6HC_optimal_price_bt }, // level 11  25.39% / 3.3
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 12  25.28% / 2.7
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 13  25.22% / 1.6  (+1 refine)
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 14  25.20% / 1.1  (+2 refine)
    { MAXD_LOG, MAXD_LOG+1, 23, 16,    64,  4,    64,  2, LZ6HC_optimal_price_bt }, // level 15  25.18% / 0.7  (+3 refine)
};



#if defined (__cplusplus)
}
#endif

#endif /* LZ6COMMON_H */
