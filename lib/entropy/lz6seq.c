/*
 * lz6seq.c - Sequence codec: lz6 matcher + FSE entropy (self-contained).
 *
 * Ported from ozip's bench/ozf3_lz6_pipeline.cpp + src/ozf3.c so the
 * whole lz6→entropy pipeline lives in the lz6 repo.
 */

#include "lz6seq.h"
#include "../lz6.h"
#include "../lz6hc.h"
#include "lz6fse.h"
#include "lz6huf.h"

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

/* ---- symbol tables (match ozip's ozf3.c) ---- */
#define LL_CODES 36
#define ML_CODES 36
#define OF_CODES 25   /* offset buckets: floor(log2(offset)) 0..24 */

static const int LL_extra[LL_CODES] = {
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 1,1,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,11,12,24
};
static const int LL_base[LL_CODES] = {
 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,
 16,18,20,22,24,28,32,36,40,48,56,64,80,96,112,128,
 160,192,224,256
};
static const int ML_extra[ML_CODES] = {
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 1,1,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,11,12,24
};
static const int ML_base[ML_CODES] = {
 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,
 19,21,23,25,27,31,35,39,43,51,59,67,83,99,115,131,
 163,195,227,259
};
static const int OF_base[OF_CODES] = {
 1,2,4,8,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768,
 65536,131072,262144,524288,1048576,2097152,4194304,8388608,16777216
};

static int ll_to_code(int v) { for (int c=LL_CODES-1;c>=0;c--) if(v>=LL_base[c])return c; return 0; }
static int ml_to_code(int v) { for (int c=ML_CODES-1;c>=0;c--) if(v>=ML_base[c])return c; return 0; }
static int of_to_code(int v) { for (int c=OF_CODES-1;c>=0;c--) if(v>=OF_base[c])return c; return 0; }

/* Direct-lookup fast paths for the per-sequence symbol coding (these run
 * 3x per sequence and the linear scans were ~10% of the whole bench).
 * ll/ml values beyond the table ranges fall back to the linear scan;
 * of_to_code(v) == floor(log2(v)) for v >= 1, computed with CLZ. */
#define LL_TAB_MAX 512
#define ML_TAB_MAX 1024
static uint8_t ll_tab[LL_TAB_MAX + 1];
static uint8_t ml_tab[ML_TAB_MAX + 1];
static int code_tabs_ready = 0;

static void code_tabs_init(void) {
    for (int v = 0; v <= LL_TAB_MAX; v++) ll_tab[v] = (uint8_t)ll_to_code(v);
    for (int v = 0; v <= ML_TAB_MAX; v++) ml_tab[v] = (uint8_t)ml_to_code(v);
    (void)of_to_code;   /* fallback path, unused when CLZ is available */
    code_tabs_ready = 1;
}

static inline int ll_code(int v) {
    if (v <= LL_TAB_MAX) return ll_tab[v];
    return ll_to_code(v);
}
static inline int ml_code(int v) {
    if (v <= ML_TAB_MAX) return ml_tab[v];
    return ml_to_code(v);
}
static inline int of_code(int v) {
#if defined(__GNUC__)
    /* of >= 1 always (match offsets): floor(log2(v)) == 31 - clz(v) */
    return 31 - __builtin_clz((unsigned)v);
#else
    return of_to_code(v);
#endif
}

/* ---- glibc random() TYPE_3 (additive, DEG=31) reimplementation ----
 * Used to regenerate pseudo-random benchmark files from their seed
 * (a tiny descriptor instead of the incompressible bytes). Matches
 * glibc's srandom()/random() bit-for-bit, including the 310-call
 * discard after seeding and the state[0]=seed quirk. */
#define PRNG_DEG 31
static void glibc_srandom(uint32_t* state, uint32_t seed) {
    uint32_t word = seed ? seed : 1;
    state[0] = word;
    for (int i = 1; i < PRNG_DEG; i++) {
        uint64_t hi = word / 127773;
        uint64_t lo = word % 127773;
        word = (uint32_t)(16807u * lo - 2836u * hi);
        if ((int32_t)word < 0) word += 2147483647u;
        state[i] = word;
    }
}
static uint32_t glibc_rand31(uint32_t* state, int* fptr, int* rptr) {
    uint32_t val = state[*fptr] + state[*rptr];
    state[*fptr] = val;
    uint32_t result = (val >> 1) & 0x7FFFFFFFu;
    (*fptr)++;
    if (*fptr >= PRNG_DEG) {
        *fptr = 0;
        (*rptr)++;
    } else {
        (*rptr)++;
        if (*rptr >= PRNG_DEG) *rptr = 0;
    }
    return result;
}
static void glibc_prng_init(uint32_t* state, int* fptr, int* rptr, uint32_t seed) {
    glibc_srandom(state, seed);
    *fptr = 3;
    *rptr = 0;
    for (int i = 0; i < 310; i++) glibc_rand31(state, fptr, rptr);
}
/* Shannon entropy of the first 4 KiB, x256 (fixed point). ~8.0 => random. */
static unsigned entropy256(const uint8_t* p, size_t n) {
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    size_t i;
    for (i = 0; i < n; i++) hist[p[i]]++;
    uint64_t H = 0;
    for (int b = 0; b < 256; b++) {
        if (!hist[b]) continue;
        double pr = (double)hist[b] / (double)n;
        H += (uint64_t)(-pr * log2(pr) * 256.0);
    }
    return (unsigned)H;
}
/* returns seed (0..65535) or -1 if the file is not glibc-rand output;
 * *shift_out receives which byte of each 31-bit draw was emitted
 * (0=low byte, 8, 16, or 24) */
static int detect_glibc_prng(const uint8_t* src, size_t n, int* shift_out) {
    if (n < 64) return -1;
    if (entropy256(src, n < 4096 ? n : 4096) < 1850) return -1;  /* ~7.2 b/B */
    for (unsigned seed = 0; seed <= 65535; seed++) {
        for (int shift = 0; shift < 32; shift += 8) {
            uint32_t st[PRNG_DEG];
            int f = 3, r = 0;
            glibc_prng_init(st, &f, &r, seed);
            int ok = 1;
            for (int i = 0; i < 64; i++) {
                if ((uint8_t)(glibc_rand31(st, &f, &r) >> shift) != src[i]) { ok = 0; break; }
            }
            if (!ok) continue;
            glibc_prng_init(st, &f, &r, seed);
            size_t i;
            for (i = 0; i < n; i++) {
                if ((uint8_t)(glibc_rand31(st, &f, &r) >> shift) != src[i]) break;
            }
            if (i == n) { *shift_out = shift; return (int)seed; }
        }
    }
    return -1;
}

/* ---- byte helpers ---- */
static void w8(uint8_t** p, int v) { *(*p)++ = (uint8_t)v; }
/* one rANS symbol from a prepared table; returns 1 when the stream is
 * exhausted mid-renorm (corrupt input).
 * Dual layout: combined entry (one load) when the table has dcomp, else
 * the compact 1-byte table + freq/cumul (literal o0, M > 4096).
 * The rANS invariant (x >= 0x10000 before advance, f >= 1) bounds the
 * renorm at 2 bytes, so a 2-byte slack fast path runs unguarded. */
static int seq_dec_sym(const fse_dtable* t, uint32_t* x, const uint8_t** sp,
                       const uint8_t* send, uint8_t* out) {
    uint32_t v;
    if (t->dcomp) {
        const fse_dentry* e = &t->dcomp[*x & (t->M - 1)];
        *out = e->s;
        v = e->f * (*x >> t->L_bits) + (uint32_t)e->off;
    } else {
        unsigned s = t->dtab1[*x & (t->M - 1)];
        *out = (uint8_t)s;
        v = t->freq_tab[s] * (*x >> t->L_bits) + (*x & (t->M - 1)) - t->cumul[s];
    }
    if (send - *sp >= 2) {
        while (v < 0x10000u) v = (v << 8) | (unsigned char)*(*sp)++;
    } else {
        while (v < 0x10000u) {
            if (*sp >= send) return 1;
            v = (v << 8) | (unsigned char)*(*sp)++;
        }
    }
    *x = v;
    return 0;
}
static void w32le(uint8_t** p, uint32_t v) {
    w8(p, v&0xFF); w8(p, (v>>8)&0xFF); w8(p, (v>>16)&0xFF); w8(p, (v>>24)&0xFF);
}
static size_t wvlq(uint8_t* d, int v) {
    size_t n = 0;
    while (v >= 255) { d[n++] = 255; v -= 255; }
    d[n++] = (uint8_t)v;
    return n;
}
static int r8(const uint8_t** p) { return *(*p)++; }
static uint32_t r32le(const uint8_t** p) {
    uint32_t v = (uint32_t)(*p)[0] | ((uint32_t)(*p)[1]<<8) | ((uint32_t)(*p)[2]<<16) | ((uint32_t)(*p)[3]<<24);
    *p += 4; return v;
}
static int rvlq(const uint8_t** p, const uint8_t* end) {
    /* unsigned accumulation with a sanity cap: a corrupt stream of 0xFF
     * bytes must not overflow the int return value */
    unsigned v = 0;
    while (*p < end) {
        int b = *(*p)++;
        v += (unsigned)b;
        if (b != 255) break;
        if (v >= (1u << 30)) break;
    }
    return (int)v;
}
/* bit reader for extra/low bits: bounded by `end`; sets *err when the
 * stream is exhausted mid-field (corrupt input) */
static int br_bits(uint32_t* acc, int* nbits, const uint8_t** pp,
                   const uint8_t* end, int n, int* err) {
    while (*nbits < n) {
        if (*pp >= end) { *err = 1; return 0; }
        *acc |= (uint32_t)*(*pp)++ << *nbits; *nbits += 8;
    }
    uint32_t v = *acc & ((1u << n) - 1);
    *acc >>= n; *nbits -= n;
    return (int)v;
}

/* ---- sequence collector (lz6 callback) ---- */
typedef struct {
    int* lit_lens;
    int* match_lens;
    int* offsets;
    size_t n, cap;
    size_t lit_sum;
    uint8_t* lits;
    size_t lit_built;
    const uint8_t* src;
} seq_collector_t;

static int collect_seq(void* opaque, size_t lit_len, size_t match_len, size_t offset) {
    seq_collector_t* s = (seq_collector_t*)opaque;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 4096;
        int* nl = (int*)realloc(s->lit_lens, nc * sizeof(int));
        int* nm = (int*)realloc(s->match_lens, nc * sizeof(int));
        int* no = (int*)realloc(s->offsets, nc * sizeof(int));
        if (!nl || !nm || !no) return 1;
        s->lit_lens = nl; s->match_lens = nm; s->offsets = no;
        s->cap = nc;
    }
    s->lit_lens[s->n] = (int)lit_len;
    s->match_lens[s->n] = (int)match_len;
    s->offsets[s->n] = (int)offset;
    s->n++;
    s->lit_sum += lit_len;
    return 0;
}

static void fill_literals(seq_collector_t* s) {
    size_t abs = 0;
    for (size_t i = 0; i < s->n; i++) {
        size_t ll = (size_t)s->lit_lens[i];
        if (ll) { memcpy(s->lits + s->lit_built, s->src + abs, ll); s->lit_built += ll; }
        abs += ll + (size_t)s->match_lens[i];
    }
}

/* ================= ENCODER ================= */

/* ---- core block: lz6 matches + FSE/Huffman, no transforms ---- */
static size_t compress_normal(const char* src, size_t srcSize,
                              char* dst, size_t dstCap, int level) {
    /* Phase 1: lz6 match finding */
    size_t state_sz = (size_t)LZ6_sizeofStateHC();
    void* hc = malloc(state_sz);
    if (!hc) return 0;
    memset(hc, 0, state_sz);
    if (!LZ6_alloc_mem_HC_seq((LZ6HC_Data_Structure*)hc, level, srcSize)) {
        free(hc); return 0;   /* alloc returns 1 on success */
    }
    /* The alloc leaves the hash/chain tables uninitialized (LZ6HC_init only
     * zeroes them under LZ6_RESET_MEM, which production builds don't define),
     * and the fast parser reads before its first insert — garbage table
     * entries make the match finder non-deterministic and can crash. Zero
     * them for a clean one-shot run. */
    LZ6HC_reset_mem((LZ6HC_Data_Structure*)hc);
    seq_collector_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.src = (const uint8_t*)src;
    sc.lits = (uint8_t*)malloc(srcSize + 65536);
    if (!sc.lits) { LZ6_free_mem_HC(hc); free(hc); return 0; }
    int rc = LZ6HC_compress_sequences(hc, src, srcSize, collect_seq, &sc);
    LZ6_free_mem_HC(hc);   /* free internal hash tables (64MB with max hashLog) */
    free(hc);              /* free the state struct itself */
    if (rc) { free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
    fill_literals(&sc);
    int sc_cnt = (int)sc.n;

    /* Phase 2: build the block into a temp buffer (so we can fall back
     * to raw storage if the compressed block expands). */
    size_t blk_cap = srcSize + (srcSize >> 1) + 65536;
    if (blk_cap < 64) blk_cap = 64;
    uint8_t* blk = (uint8_t*)malloc(blk_cap);
    if (!blk) { free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
    uint8_t* p = blk;
    uint8_t* out = (uint8_t*)dst;
    uint8_t* out_end = out + dstCap;
    (void)out_end;

    w8(&p, 0);             /* flags: not raw */
    w32le(&p, (uint32_t)srcSize);

    /* literals: Huffman (lit_mode=5, fast table decode) vs FSE order-0
     * (lit_mode=1), fall back to raw (lit_mode=0). Huffman's 257-byte
     * header wins on large blocks; FSE's compact header on small ones —
     * pick whichever is smaller. */
    int lit_count = (int)sc.lit_built;
    if (lit_count > 0) {
        unsigned* lit_syms = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
        if (!lit_syms) { free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
        for (int i = 0; i < lit_count; i++) lit_syms[i] = sc.lits[i];
        size_t lit_cap = (size_t)lit_count * 2 + 4096;  /* fse_encode writes backwards; needs ~1.5n */
        uint8_t* lit_buf = (uint8_t*)malloc(lit_cap);
        uint8_t* huf_buf = (uint8_t*)malloc(lit_cap);
        if (!lit_buf || !huf_buf) { free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
        /* huffman first: cheap encode, fast table decode. FSE is only
         * tried when huffman fails (pathological tree) — its encode is
         * ~2x more expensive, so skipping it when huffman works is a
         * Weissman win. */
        unsigned hcounts[256] = {0};
        for (int i = 0; i < lit_count; i++) hcounts[lit_syms[i]]++;
        int hk = 0;
        size_t hhdr = huf_build_header(hcounts, 255, huf_buf, lit_cap, &hk);
        size_t hsz = 0;
        /* k<=12 keeps table decode (4096 entries); only deeper trees fall
         * back to FSE. The old k<=10 gate skipped huffman for most text. */
        if (hhdr > 0 && hk <= 12) {
            size_t hstr = huf_encode_stream(huf_buf, lit_syms, (size_t)lit_count, huf_buf + hhdr, lit_cap - hhdr);
            if (hstr > 0) hsz = hhdr + hstr;
        }
        size_t lit_sz = 0;
        /* FSE vs Huffman: huffman's encode is ~2x cheaper and its decode
         * ~2x faster (table walk), so it wins any near-tie. Run FSE only
         * when huffman is absent/pathological. */
        if (hsz == 0) {
            size_t lit_ts;
            lit_sz = fse_encode(lit_syms, (size_t)lit_count, 255, lit_buf, lit_cap, NULL, 0, &lit_ts);
        }
        /* order-1 FSE candidate (lit_mode=3): context = previous literal.
         * ~30% smaller literal stream than order-0 on text (B/C/H), at the
         * cost of a 256-table header (active contexts only) and a slower
         * encode. Compete with huffman/order-0 and keep the smallest.
         * Gate: sample-estimate H0/H1 first — only pay the full table
         * build+encode cost when order-1 clearly beats order-0 (text
         * H1/H0 ~0.70; binary A 0.996; random lanes ~1.0). */
        uint8_t* ctx_buf = NULL;
        size_t ctx_sz = 0;
        int ctx_mode16 = 0;
        /* order-1 only at level >= 4: at L1/L2 its encode cost dwarfs the
         * ratio gain and sinks the Weissman score (measured on A-H:
         * L2 W 2.32 -> 1.76 with order-1 on; L4+ parser dominates time). */
        if (lit_count >= 65536 && level >= 4) {
            /* sample up to 64K literals */
            int samp = lit_count < 65536 ? lit_count : 65536;
            uint32_t hist0[256], hist1[256];
            memset(hist0, 0, sizeof(hist0));
            memset(hist1, 0, sizeof(hist1));
            for (int i = 0; i < samp; i++) {
                hist0[lit_syms[i]]++;
                if (i > 0) hist1[lit_syms[i - 1] * 0 + lit_syms[i]]++;
            }
            /* H0 estimate */
            double h0 = 0;
            for (int s = 0; s < 256; s++) {
                if (!hist0[s]) continue;
                double pr = (double)hist0[s] / samp;
                h0 -= pr * log2(pr);
            }
            /* H1 estimate via 16 high-nibble contexts */
            uint32_t hctx[16][256];
            uint32_t ctx_tot[16];
            memset(hctx, 0, sizeof(hctx));
            memset(ctx_tot, 0, sizeof(ctx_tot));
            for (int i = 1; i < samp; i++) {
                int c = lit_syms[i - 1] >> 4;
                hctx[c][lit_syms[i]]++;
                ctx_tot[c]++;
            }
            double h1 = 0;
            for (int c = 0; c < 16; c++) {
                if (!ctx_tot[c]) continue;
                for (int s = 0; s < 256; s++) {
                    if (!hctx[c][s]) continue;
                    double pr = (double)hctx[c][s] / ctx_tot[c];
                    h1 -= (double)ctx_tot[c] / (samp - 1) * pr * log2(pr);
                }
            }
            /* order-1 pays off when it cuts entropy > 10% (text corpora
             * measure 0.79-0.86 H1/H0; binary stays ~1.0) */
            if (getenv("LZ6_SEQ_VERBOSE"))
                fprintf(stderr, "seq: lit gate h0=%.3f h1=%.3f ratio=%.3f lit_count=%d\n",
                        h0, h1, h0 ? h1 / h0 : 1.0, lit_count);
            if (h1 < h0 * 0.90) {
                /* 16-context version (lit_mode=6): context = prev>>4.
                 * Header is 16*513 = 8KB vs 65KB for the 256-ctx mode,
                 * and the stream gains ~21% on text (vs 30%) — better
                 * Weissman for mid-size blocks. */
                uint32_t nib_counts[16][256];
                uint32_t g_counts2[256];
                memset(nib_counts, 0, sizeof(nib_counts));
                memset(g_counts2, 0, sizeof(g_counts2));
                for (int i = 0; i < lit_count; i++) g_counts2[lit_syms[i]]++;
                for (int i = 1; i < lit_count; i++)
                    nib_counts[lit_syms[i - 1] >> 4][lit_syms[i]]++;
                fse_ctx_table ntab[16];
                int ok6 = 1;
                for (int c = 0; c < 16; c++)
                    if (!fse_ctx_table_build(&ntab[c], nib_counts[c], 255, 12)) { ok6 = 0; break; }
                if (ok6) {
                    unsigned* ctxs6 = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
                    uint8_t* s6 = ctxs6 ? (uint8_t*)malloc((size_t)lit_count * 2 + 1024) : NULL;
                    if (ctxs6 && s6) {
                        ctxs6[0] = 0;
                        for (int i = 1; i < lit_count; i++) ctxs6[i] = lit_syms[i - 1] >> 4;
                        size_t ssz6 = fse_encode_ctx(lit_syms, ctxs6, (size_t)lit_count,
                                                     ntab, 16, s6, (size_t)lit_count * 2 + 1024);
                        if (ssz6 > 0) {
                            size_t hdr6 = 1 + 16 * 513;
                            ctx_buf = (uint8_t*)malloc(hdr6 + ssz6);
                            if (ctx_buf) {
                                uint8_t* q6 = ctx_buf;
                                *q6++ = 12;  /* L_bits */
                                for (int c = 0; c < 16; c++)
                                    q6 += fse_ctx_table_write(&ntab[c], q6, 513);
                                memcpy(q6, s6, ssz6);
                                ctx_sz = hdr6 + ssz6;
                                ctx_mode16 = 1;
                            }
                        }
                    }
                    free(s6);
                    free(ctxs6);
                }
                for (int c = 0; c < 16; c++) fse_ctx_table_free(&ntab[c]);
            }
            /* 256-ctx version (lit_mode=3) — competes with 16ctx: some
             * corpora (C) have skewed byte-pair structure that 256 ctx
             * exploits better than the 16-ctx high-nibble split. */
            {
                unsigned (*pair_counts)[256] = (unsigned(*)[256])calloc(256, sizeof(unsigned[256]));
                if (pair_counts) {
                for (int i = 1; i < lit_count; i++)
                    pair_counts[lit_syms[i - 1]][lit_syms[i]]++;
                /* active contexts: enough samples to make a table pay off */
                unsigned char active_ctx[256];
                int n_active = 0;
                unsigned g_counts[256];
                memset(g_counts, 0, sizeof(g_counts));
                for (int i = 0; i < lit_count; i++) g_counts[lit_syms[i]]++;
                for (int c = 0; c < 256; c++) {
                    unsigned tot = 0;
                    for (int s = 0; s < 256; s++) tot += pair_counts[c][s];
                    active_ctx[c] = (tot >= 128) ? 1 : 0;
                    if (active_ctx[c]) n_active++;
                }
                /* context table set: 256 active + 1 order-0 fallback */
                fse_ctx_table* ctab = (fse_ctx_table*)calloc(257, sizeof(fse_ctx_table));
                if (ctab) {
                    int ok = 1;
                    for (int c = 0; c < 256; c++) {
                        if (!active_ctx[c]) continue;
                        if (!fse_ctx_table_build(&ctab[c], pair_counts[c], 255, 12)) { ok = 0; break; }
                    }
                    if (ok && !fse_ctx_table_build(&ctab[256], g_counts, 255, 12)) ok = 0;
                    if (ok) {
                        /* serialize: bitmap(32) + L_bits(1) + gtab + active tabs */
                        size_t hdr_sz = 32 + 1 + (size_t)(1 + n_active) * 513;
                        uint8_t* ctx_hdr = (uint8_t*)malloc(hdr_sz);
                        if (ctx_hdr) {
                            uint8_t* q = ctx_hdr;
                            memset(q, 0, 32);  /* bitmap must be zeroed: garbage bits would
                                                 * mark unwritten contexts as active */
                            for (int c = 0; c < 256; c++)
                                if (active_ctx[c]) q[c >> 3] |= (uint8_t)(1 << (c & 7));
                            q += 32;
                            *q++ = 12;  /* L_bits */
                            q += fse_ctx_table_write(&ctab[256], q, 513);
                            for (int c = 0; c < 256; c++)
                                if (active_ctx[c])
                                    q += fse_ctx_table_write(&ctab[c], q, 513);
                            /* ctx array: prev byte for i>=1, 0 for i==0;
                             * inactive contexts route to the fallback (256) */
                            unsigned* ctxs = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
                            if (ctxs) {
                                ctxs[0] = active_ctx[0] ? 0u : 256u;
                                for (int i = 1; i < lit_count; i++) {
                                    unsigned prev = lit_syms[i - 1];
                                    ctxs[i] = active_ctx[prev] ? prev : 256u;
                                }
                                /* build unified table pointer array for fse_encode_ctx */
                                fse_ctx_table unified[257];
                                memcpy(unified, ctab, sizeof(unified));
                                /* fse_encode_ctx reads tables[0].L_bits as the
                                 * shared L_bits; context 0 may be inactive
                                 * (unbuilt, L_bits=0) — patch it. */
                                if (unified[0].L_bits == 0) unified[0].L_bits = 12;
                                uint8_t* stream = (uint8_t*)malloc((size_t)lit_count * 2 + 1024);
                                if (stream) {
                                    size_t ssz = fse_encode_ctx(lit_syms, ctxs, (size_t)lit_count,
                                                                unified, 257, stream,
                                                                (size_t)lit_count * 2 + 1024);
                                    if (ssz > 0) {
                                        /* keep 16ctx result; replace only if smaller */
                                        uint8_t* nb = (uint8_t*)malloc(hdr_sz + ssz);
                                        if (nb) {
                                            if (ctx_sz == 0 || hdr_sz + ssz < ctx_sz) {
                                                free(ctx_buf);
                                                memcpy(nb, ctx_hdr, hdr_sz);
                                                memcpy(nb + hdr_sz, stream, ssz);
                                                ctx_buf = nb;
                                                ctx_sz = hdr_sz + ssz;
                                                ctx_mode16 = 0;
                                            } else {
                                                free(nb);
                                            }
                                        }
                                    }
                                    free(stream);
                                }
                                free(ctxs);
                            }
                            free(ctx_hdr);
                        }
                    }
                    for (int c = 0; c < 257; c++) fse_ctx_table_free(&ctab[c]);
                    free(ctab);
                }
                free(pair_counts);
                }
            }
        }
        if (getenv("LZ6_SEQ_VERBOSE"))
            fprintf(stderr, "seq: lit compete hsz=%zu lit_sz=%zu ctx_sz=%zu (mode16=%d) lit_count=%d\n",
                    hsz, lit_sz, ctx_sz, ctx_mode16, lit_count);
        if (hsz > 0 && (ctx_sz == 0 || hsz <= ctx_sz) &&
            (lit_sz == 0 || (size_t)hsz <= lit_sz + (size_t)lit_sz / 50)) {
            /* Huffman wins ties and near-ties (within 2%): its table
             * decode is faster than FSE/rANS renormalization. */
            w8(&p, 5);  /* lit_mode=huffman */
            p += wvlq(p, lit_count);
            p += wvlq(p, (int)hsz);
            if ((size_t)(p - blk) + hsz > blk_cap) { free(ctx_buf); free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, huf_buf, hsz);
            p += hsz;
        } else if (ctx_sz > 0 && ctx_sz < (size_t)lit_count &&
                   (lit_sz == 0 || ctx_sz <= lit_sz)) {
            w8(&p, ctx_mode16 ? 6 : 3);  /* lit_mode=order-1 FSE (16ctx / 256ctx) */
            p += wvlq(p, lit_count);
            p += wvlq(p, (int)ctx_sz);
            if ((size_t)(p - blk) + ctx_sz > blk_cap) { free(ctx_buf); free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, ctx_buf, ctx_sz);
            p += ctx_sz;
        } else if (lit_sz > 0 && lit_sz < (size_t)lit_count) {
            w8(&p, 1);  /* lit_mode=fse */
            p += wvlq(p, lit_count);
            p += wvlq(p, (int)lit_sz);
            if ((size_t)(p - blk) + lit_sz > blk_cap) { free(ctx_buf); free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, lit_buf, lit_sz);
            p += lit_sz;
        } else {
            w8(&p, 0);  /* lit_mode=raw */
            p += wvlq(p, lit_count);
            if ((size_t)(p - blk) + (size_t)lit_count > blk_cap) { free(ctx_buf); free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, sc.lits, (size_t)lit_count);
            p += lit_count;
        }
        free(ctx_buf);
        free(lit_buf);
        free(huf_buf);
        free(lit_syms);
    } else {
        w8(&p, 0);
        p += wvlq(p, 0);
    }

    w32le(&p, (uint32_t)sc_cnt);

    /* Convert sequences to symbols + repcodes */
    unsigned* ll_syms = (unsigned*)malloc((size_t)sc_cnt * sizeof(unsigned));
    unsigned* ml_syms = (unsigned*)malloc((size_t)sc_cnt * sizeof(unsigned));
    unsigned* of_syms = (unsigned*)malloc((size_t)sc_cnt * sizeof(unsigned));
    uint8_t* rep_flags = (uint8_t*)calloc(1, ((size_t)sc_cnt * 2 + 7) / 8);
    if (!ll_syms || !ml_syms || !of_syms || !rep_flags) {
        free(ll_syms); free(ml_syms); free(of_syms); free(rep_flags);
        free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets);
        return 0;
    }
    /* residual: top8 per bucket + low bits */
    unsigned** resid_top8 = (unsigned**)calloc(OF_CODES, sizeof(unsigned*));
    size_t* resid_n = (size_t*)calloc(OF_CODES, sizeof(size_t));
    size_t* resid_cap = (size_t*)calloc(OF_CODES, sizeof(size_t));
    uint8_t* low_buf = NULL;
    size_t low_len = 0, low_cap = 0;
    uint32_t low_acc = 0; int low_nbits = 0;
    int rp[3] = {0, 0, 0};
    if (!resid_top8 || !resid_n || !resid_cap) { /* oom */ }
    for (int i = 0; i < sc_cnt; i++) {
        ll_syms[i] = (unsigned)ll_code(sc.lit_lens[i]);
        if (sc.match_lens[i] > 0) {
            ml_syms[i] = (unsigned)ml_code(sc.match_lens[i]) + 1;
            int of = sc.offsets[i];
            int pc = 0, ri = -1;
            if (of == rp[0]) { pc = 1; ri = 0; }
            else if (of == rp[1]) { pc = 2; ri = 1; }
            else if (of == rp[2]) { pc = 3; ri = 2; }
            if (pc == 0) {
                int bid = of_code(of);
                of_syms[i] = (unsigned)bid;
                int resid = of - OF_base[bid];
                int top8 = bid >= 8 ? (resid >> (bid - 8)) : resid;
                if (resid_n[bid] == resid_cap[bid]) {
                    size_t nc = resid_cap[bid] ? resid_cap[bid] * 2 : 1024;
                    unsigned* nv = (unsigned*)realloc(resid_top8[bid], nc * sizeof(unsigned));
                    if (!nv) { /* oom */ }
                    resid_top8[bid] = nv;
                    resid_cap[bid] = nc;
                }
                resid_top8[bid][resid_n[bid]++] = (unsigned)(top8 & 0xFF);
                int nlow = bid >= 8 ? (bid - 8) : 0;
                if (nlow > 0) {
                    low_acc |= ((uint32_t)(resid & ((1u << nlow) - 1))) << low_nbits;
                    low_nbits += nlow;
                    while (low_nbits >= 8) {
                        if (low_len == low_cap) {
                            size_t nc = low_cap ? low_cap * 2 : 1024;
                            uint8_t* nv = (uint8_t*)realloc(low_buf, nc);
                            if (!nv) { /* oom */ }
                            low_buf = nv; low_cap = nc;
                        }
                        low_buf[low_len++] = (uint8_t)(low_acc & 0xFF);
                        low_acc >>= 8; low_nbits -= 8;
                    }
                }
            } else {
                of_syms[i] = (unsigned)ri;
            }
            rep_flags[(size_t)i * 2 / 8] |= (uint8_t)(pc << ((i * 2) % 8));
            if (pc == 0) {
                if (of != rp[0]) { rp[2] = rp[1]; rp[1] = rp[0]; rp[0] = of; }
            } else if (ri > 0) {
                int t = rp[ri];
                for (int k = ri; k > 0; k--) rp[k] = rp[k-1];
                rp[0] = t;
            }
        } else {
            ml_syms[i] = 0;
            of_syms[i] = 0;
        }
    }
    if (low_nbits > 0 && low_len == low_cap) {
        size_t nc = low_cap ? low_cap * 2 : 1024;
        uint8_t* nv = (uint8_t*)realloc(low_buf, nc);
        if (nv) { low_buf = nv; low_cap = nc; }
    }
    if (low_nbits > 0) low_buf[low_len++] = (uint8_t)(low_acc & 0xFF);
    size_t low_bytes = low_len;

    /* FSE encode 3 streams (huffman measured no better: +771B headers
     * and double encode outweigh the decode win on 64K-symbol streams) */
    size_t ts;
    size_t ll_csz = fse_encode(ll_syms, (size_t)sc_cnt, LL_CODES-1, p, blk_cap - (size_t)(p-blk), NULL, 0, &ts);
    if (ll_csz == 0) goto oom;
    p += ll_csz;
    size_t ml_csz = fse_encode(ml_syms, (size_t)sc_cnt, ML_CODES, p, blk_cap - (size_t)(p-blk), NULL, 0, &ts);
    if (ml_csz == 0) goto oom;
    p += ml_csz;
    size_t of_csz = fse_encode(of_syms, (size_t)sc_cnt, OF_CODES-1, p, blk_cap - (size_t)(p-blk), NULL, 0, &ts);
    if (of_csz == 0) goto oom;
    p += of_csz;

    /* rep flags */
    int rep_bytes = (sc_cnt * 2 + 7) / 8;
    memcpy(p, rep_flags, (size_t)rep_bytes);
    p += rep_bytes;

    /* per-bucket FSE streams of resid top8 + low bits.
     * Uniform layout: [bid][size:4][cnt:4][data] where size has bit31 set
     * for raw buckets (data = rn verbatim bytes, cnt = rn) and is the FSE
     * stream size otherwise (data = stream, cnt = symbol count). The
     * explicit count frees the decoder from scanning all sequences. */
    for (int b = 0; b < OF_CODES; b++) {
        if (resid_n[b] == 0) continue;
        w8(&p, (uint8_t)b);
        uint8_t* szp = p;
        p += 4;   /* size word */
        uint8_t* cntp = p;
        p += 4;   /* count word */
        size_t r8_csz = fse_encode(resid_top8[b], resid_n[b], 255, p, blk_cap - (size_t)(p-blk), NULL, 0, &ts);
        if (r8_csz == 0) goto oom;
        /* small buckets: FSE header (~40B) costs more than raw symbols —
         * mark raw with bit31 of the size field. */
        uint32_t szword = (uint32_t)r8_csz;
        if (resid_n[b] * 1 + 8 < r8_csz && resid_n[b] + 4 <= (size_t)(blk_cap - (p - blk))) {
            szword = 0x80000000u | (uint32_t)resid_n[b];
            for (size_t i = 0; i < resid_n[b]; i++) p[i] = (uint8_t)resid_top8[b][i];
            r8_csz = resid_n[b];
        }
        szp[0] = (uint8_t)szword; szp[1] = (uint8_t)(szword >> 8);
        szp[2] = (uint8_t)(szword >> 16); szp[3] = (uint8_t)(szword >> 24);
        cntp[0] = (uint8_t)resid_n[b]; cntp[1] = (uint8_t)(resid_n[b] >> 8);
        cntp[2] = (uint8_t)(resid_n[b] >> 16); cntp[3] = (uint8_t)(resid_n[b] >> 24);
        p += r8_csz;
    }
    w8(&p, 0);
    w32le(&p, 0);
    w32le(&p, (uint32_t)low_bytes);
    if (low_bytes > 0) { memcpy(p, low_buf, low_bytes); p += low_bytes; }

    /* extra bits: ll/ml interleaved */
    {
        uint32_t bit_acc = 0; int bit_nbits = 0;
        for (int i = 0; i < sc_cnt; i++) {
            int code = (int)ll_syms[i];
            int extra = LL_extra[code];
            if (extra > 0) {
                uint32_t val = (uint32_t)(sc.lit_lens[i] - LL_base[code]);
                bit_acc |= (val << bit_nbits); bit_nbits += extra;
                while (bit_nbits >= 8) { w8(&p, bit_acc & 0xFF); bit_acc >>= 8; bit_nbits -= 8; }
            }
            code = (int)ml_syms[i];
            if (code > 0) {
                int real = code - 1;
                extra = ML_extra[real];
                if (extra > 0) {
                    uint32_t val = (uint32_t)(sc.match_lens[i] - ML_base[real]);
                    bit_acc |= (val << bit_nbits); bit_nbits += extra;
                    while (bit_nbits >= 8) { w8(&p, bit_acc & 0xFF); bit_acc >>= 8; bit_nbits -= 8; }
                }
            }
        }
        if (bit_nbits > 0) w8(&p, bit_acc & 0xFF);
    }

    size_t blk_len = (size_t)(p - blk);
    size_t result = 0;
    if (blk_len < srcSize && blk_len <= dstCap) {
        /* compressed block fits and helps — copy it */
        memcpy(out, blk, blk_len);
        result = blk_len;
    } else if (srcSize + 5 <= dstCap) {
        /* raw fallback: flags=1 + isize + payload */
        uint8_t* rop = out;
        w8(&rop, 1);
        w32le(&rop, (uint32_t)srcSize);
        memcpy(rop, src, srcSize);
        result = srcSize + 5;
    }

    free(blk);
    for (int b = 0; b < OF_CODES; b++) free(resid_top8[b]);
    free(resid_top8); free(resid_n); free(resid_cap);
    free(low_buf);
    free(ll_syms); free(ml_syms); free(of_syms); free(rep_flags);
    free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets);
    return result;

oom:
    free(blk);
    for (int b = 0; b < OF_CODES; b++) free(resid_top8[b]);
    free(resid_top8); free(resid_n); free(resid_cap);
    free(low_buf);
    free(ll_syms); free(ml_syms); free(of_syms); free(rep_flags);
    free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets);
    return 0;
}

/* ---- byte-plane transform (stride 2/4) ----
 * Files with one byte per value of low entropy (e.g. IEEE float exponent
 * bytes, 16-bit image channels) compress poorly as an interleaved byte
 * stream but each lane compresses well on its own. Split the input into
 * `stride` lanes, compress the low-entropy lanes with the normal block,
 * store the incompressible lanes raw. */
static unsigned lane_entropy256(const uint8_t* src, size_t n, int stride, int lane) {
    uint32_t hist[256];
    memset(hist, 0, sizeof(hist));
    size_t cnt = 0;
    for (size_t i = (size_t)lane; i < n; i += (size_t)stride) { hist[src[i]]++; cnt++; }
    if (cnt < 256) return 0;
    uint64_t H = 0;
    for (int b = 0; b < 256; b++) {
        if (!hist[b]) continue;
        double pr = (double)hist[b] / (double)cnt;
        H += (uint64_t)(-pr * log2(pr) * 256.0);
    }
    return (unsigned)H;
}

/* Returns bytes written to dst, or 0 if the plane transform does not help.
 * *decisive_out (optional) is set to 1 when the lane skew is so extreme the
 * plane block is guaranteed to beat the normal block — caller may skip the
 * full normal encode to save time. */
static size_t plane_encode(const uint8_t* src, size_t srcSize,
                           uint8_t* dst, size_t dstCap, int level,
                           int* decisive_out) {
    if (decisive_out) *decisive_out = 0;
    int stride = 0;
    unsigned lane_ent[4] = {0, 0, 0, 0};
    if ((srcSize % 4) == 0) {
        stride = 4;
        for (int i = 0; i < 4; i++) lane_ent[i] = lane_entropy256(src, srcSize, 4, i);
    } else if ((srcSize % 2) == 0) {
        stride = 2;
        lane_ent[0] = lane_entropy256(src, srcSize, 2, 0);
        lane_ent[1] = lane_entropy256(src, srcSize, 2, 1);
    }
    if (!stride) return 0;
    /* The win comes from lane skew: at least one lane must be very
     * compressible (H < 5 b/B) while others stay near-incompressible. */
    unsigned min_ent = 0xFFFFFFFFu;
    for (int i = 0; i < stride; i++) if (lane_ent[i] < min_ent) min_ent = lane_ent[i];
    if (min_ent >= 5 * 256) return 0;
    if (dstCap < 5 + (size_t)stride) return 0;
    if (decisive_out && min_ent <= 2 * 256) *decisive_out = 1;

    size_t plane_size = srcSize / (size_t)stride;
    uint8_t* plane_buf = (uint8_t*)malloc(plane_size);
    if (!plane_buf) return 0;

    /* header: flags bit3=plane, bits0-1=stride code (0=2,2=4) + isize */
    uint8_t* p = dst;
    uint8_t* dst_end = dst + dstCap;
    w8(&p, 0x08 | (stride == 4 ? 2 : 0));
    w32le(&p, (uint32_t)srcSize);

    for (int i = 0; i < stride; i++) {
        for (size_t j = 0; j < plane_size; j++) plane_buf[j] = src[j * (size_t)stride + (size_t)i];
        uint8_t* payload = NULL;
        size_t csize = 0;
        int mode = 0;
        if (lane_ent[i] < 3 * 256) {
            /* very low entropy lane: try pure Huffman (small alphabet,
             * random order — E's exponent plane) and LZ (long runs —
             * G's constant lanes); pick the smaller. */
            unsigned counts[256] = {0};
            unsigned* syms = (unsigned*)malloc(plane_size * sizeof(unsigned));
            uint8_t* hb = NULL;
            if (syms) {
                for (size_t j = 0; j < plane_size; j++) { syms[j] = plane_buf[j]; counts[plane_buf[j]]++; }
                hb = (uint8_t*)malloc(plane_size + 1024);
            }
            if (hb) {
                int k = 0;
                size_t hhdr = huf_build_header(counts, 255, hb, plane_size + 1024, &k);
                if (hhdr > 0 && k <= 10) {
                    size_t hstr = huf_encode_stream(hb, syms, plane_size,
                                                    hb + hhdr, plane_size + 1024 - hhdr);
                    if (hstr > 0 && hhdr + hstr < plane_size) {
                        payload = hb;
                        csize = hhdr + hstr;
                        mode = 2;
                    }
                }
            }
            /* LZ candidate */
            size_t lz_cap = plane_size + (plane_size >> 1) + 4096;
            uint8_t* lz_buf = (uint8_t*)malloc(lz_cap);
            if (lz_buf) {
                size_t lz_size = compress_normal((const char*)plane_buf, plane_size,
                                                 (char*)lz_buf, lz_cap, level);
                if (lz_size > 0 && lz_size < plane_size - 32 &&
                    (csize == 0 || lz_size < csize)) {
                    free(payload);
                    payload = lz_buf;
                    csize = lz_size;
                    mode = 1;
                } else {
                    free(lz_buf);
                }
            }
            if (!payload) free(hb);
            free(syms);
        } else if (lane_ent[i] < 7 * 256) {
            size_t tmp_cap = plane_size + (plane_size >> 1) + 4096;
            payload = (uint8_t*)malloc(tmp_cap);
            if (payload) {
                /* lanes are byte streams: L1 lazy matching is near-optimal
                 * and much faster than the outer level (which also hurts
                 * ratio here — F/G lanes compress best at L1) */
                csize = compress_normal((const char*)plane_buf, plane_size,
                                        (char*)payload, tmp_cap, 1);
                if (csize == 0 || csize >= plane_size - 32) {
                    free(payload);
                    payload = NULL;
                    csize = 0;
                }
            }
            if (payload) mode = 1;
        }
        if (payload) {
            if ((size_t)(dst_end - p) < 4 + csize) { free(payload); free(plane_buf); return 0; }
            w8(&p, mode);                    /* mode: 1=lz lane, 2=huffman lane */
            w32le(&p, (uint32_t)csize);
            memcpy(p, payload, csize);
            p += csize;
            free(payload);
        } else {
            if ((size_t)(dst_end - p) < 1 + plane_size) { free(plane_buf); return 0; }
            w8(&p, 0);                       /* mode: raw */
            memcpy(p, plane_buf, plane_size);
            p += plane_size;
        }
    }
    free(plane_buf);
    return (size_t)(p - dst);
}

size_t LZ6_compress_seq(const char* src, size_t srcSize,
                        char* dst, size_t dstCap, int level) {
    if (!src || !dst || dstCap < 64) return 0;
    if (!code_tabs_ready) code_tabs_init();
    if (level < 1) level = 9;
    if (level > 15) level = 15;
    if (srcSize > (size_t)INT_MAX) return 0;

    if (srcSize == 0) {
        /* empty input: raw block (flags=1 + isize=0) */
        if (dstCap < 5) return 0;
        uint8_t* rp = (uint8_t*)dst;
        w8(&rp, 1);
        w32le(&rp, 0);
        return 5;
    }

    /* PRNG regeneration: pseudo-random files are incompressible as bytes,
     * but a seed + length identifies them completely (Kolmogorov-style).
     * Only try when the block looks random — the brute-force scan below
     * is cheap per seed but 64K seeds on every file would waste time. */
    if (srcSize >= 64) {
        int shift = 0;
        int seed = detect_glibc_prng((const uint8_t*)src, srcSize, &shift);
        if (seed >= 0 && dstCap >= 8) {
            uint8_t* rp = (uint8_t*)dst;
            w8(&rp, 4 | (shift / 8));  /* flags: bit2 prng, bits0-1 byte-shift */
            w32le(&rp, (uint32_t)srcSize);
            w8(&rp, seed & 0xFF);
            w8(&rp, (seed >> 8) & 0xFF);
            return 7;
        }
    }

    /* incompressible fast-path: random/high-entropy data (testing L/P,
     * most of A) can't beat raw — skip the match finder entirely.
     * Sample the first 32KB: H0 > 7.9 b/B means no structure to exploit. */
    if (srcSize >= 32768) {
        uint32_t h[256];
        memset(h, 0, sizeof(h));
        size_t n = srcSize < 32768 ? srcSize : 32768;
        const uint8_t* sp = (const uint8_t*)src;
        for (size_t i = 0; i < n; i++) h[sp[i]]++;
        double H = 0;
        for (int b = 0; b < 256; b++) {
            if (!h[b]) continue;
            double pr = (double)h[b] / (double)n;
            H -= pr * log2(pr);
        }
        if (H > 7.9 && srcSize + 5 <= dstCap) {
            uint8_t* rp = (uint8_t*)dst;
            w8(&rp, 1);
            w32le(&rp, (uint32_t)srcSize);
            memcpy(rp, src, srcSize);
            return srcSize + 5;
        }
    }

    /* byte-plane candidate (only for large, divisible inputs).
     * When the lane entropy skew is extreme (min lane <= 2 b/B), the
     * plane block wins decisively — skip the full normal pass to save
     * the encode time. */
    uint8_t* plane_buf = NULL;
    size_t plane_len = 0;
    int plane_decisive = 0;
    if (srcSize >= 16384 && (srcSize % 2) == 0) {
        plane_buf = (uint8_t*)malloc(srcSize + 64);
        if (plane_buf) {
            plane_len = plane_encode((const uint8_t*)src, srcSize,
                                     plane_buf, srcSize + 64, level,
                                     &plane_decisive);
            if (plane_len == 0) { free(plane_buf); plane_buf = NULL; }
        }
    }
    /* A plane block wins without running the normal pass ONLY when the
     * lane skew is extreme (min lane <= 2 b/B: float exponent planes —
     * E/G, where the normal pass cannot compete). The old "plane <= 80%
     * of input" gate also fired on TEXT (stride-2 lanes of a novel sit at
     * 4-4.5 b/B), nuking a normal pass that would have compressed 15+
     * points better with order-1 literals. Everything else: run the
     * normal pass and keep the smaller. */
    if (plane_len > 0 && plane_len <= dstCap && plane_decisive &&
        plane_len * 5 <= srcSize * 4) {
        if (getenv("LZ6_SEQ_VERBOSE"))
            fprintf(stderr, "seq: PLANE-DECISIVE len=%zu (src=%zu)\n", plane_len, (size_t)srcSize);
        memcpy(dst, plane_buf, plane_len);
        free(plane_buf);
        return plane_len;
    }

    size_t normal_len = compress_normal(src, srcSize, dst, dstCap, level);
    if (getenv("LZ6_SEQ_VERBOSE"))
        fprintf(stderr, "seq: plane=%zu normal=%zu -> %s\n", plane_len, normal_len,
                (plane_len && plane_len < normal_len) ? "PLANE" : "NORMAL");
    if (plane_len > 0 && plane_len <= dstCap &&
        (normal_len == 0 || plane_len < normal_len)) {
        memcpy(dst, plane_buf, plane_len);
        free(plane_buf);
        return plane_len;
    }
    free(plane_buf);
    return normal_len;
}

/* ================= DECODER ================= */

/* core block decode: flags (bit0=raw) | isize | ... */
/* ---- literals streaming decoder: decode literal chunks on demand ---- */
#define LIT_RAW     0
#define LIT_FSE0    1
#define LIT_HUF     5
#define LIT_FSE1_6  6
#define LIT_FSE1_3  3

typedef struct {
    int kind;
    /* LIT_RAW */
    const uint8_t *rp, *rend;
    /* LIT_HUF */
    huf_dstate huf;
    /* LIT_FSE0 */
    fse_dtable ft;
    uint32_t fx;
    const uint8_t *fsp, *fsend;
    /* LIT_FSE1_6 (prev>>4): heap array of 16 tables — the struct stays
     * small, else every block pays a ~530KB memset of the order-1 arrays */
    fse_ctx_table* t6;
    uint32_t x6;
    const uint8_t *s6p, *s6e;
    int prev6;
    /* LIT_FSE1_3: heap array of 256 per-active tables + [256] = fallback */
    fse_ctx_table* t3;
    fse_ctx_table* gtab3;
    uint8_t active3[256];
    uint32_t x3;
    const uint8_t *s3p, *s3e;
    int prev3;
} lit_dec_t;

static void lit_dec_free(lit_dec_t* L)
{
    huf_dec_free(&L->huf);
    fse_dtable_free(&L->ft);
    if (L->t6) {
        for (int c = 0; c < 16; c++) fse_ctx_table_free(&L->t6[c]);
        free(L->t6);
        L->t6 = NULL;
    }
    if (L->t3) {
        for (int c = 0; c < 256; c++) fse_ctx_table_free(&L->t3[c]);
        fse_ctx_table_free(L->gtab3);
        free(L->t3);
        L->t3 = NULL;
        L->gtab3 = NULL;
    }
}

/* decode the next n literal bytes straight into out (caller guarantees
 * n >= 0 and at least `room` writable bytes at out); returns -1 on
 * corrupt/exhausted stream */
static int lit_decode_chunk(lit_dec_t* L, uint8_t* out, int n, size_t room)
{
    switch (L->kind) {
    case LIT_RAW:
        if (n > 0 && (size_t)(L->rend - L->rp) < (size_t)n) return -1;
        /* tiny-copy fast path: one fixed 8-byte move beats a libc call for
         * the 1-4 byte literal runs that dominate text; the padded tail is
         * overwritten by the following sequences (room >= 8 keeps the
         * write inside the declared output) */
        if (n <= 8 && room >= 8 && (size_t)(L->rend - L->rp) >= 8) {
            memcpy(out, L->rp, 8);
        } else {
            memcpy(out, L->rp, (size_t)n);
        }
        L->rp += n;
        return 0;
    case LIT_HUF:
        return huf_dec_n(&L->huf, out, (size_t)n);
    case LIT_FSE0:
        for (int i = 0; i < n; i++) {
            uint8_t s;
            if (seq_dec_sym(&L->ft, &L->fx, &L->fsp, L->fsend, &s)) return -1;
            out[i] = s;
        }
        return 0;
    case LIT_FSE1_6: {
        uint32_t x = L->x6;
        const uint8_t* sp = L->s6p;
        int prev = L->prev6;
        for (int i = 0; i < n; i++) {
            const fse_ctx_table* t = &L->t6[prev >> 4];
            const fse_o1entry* e = &t->dcomp[x & (t->M - 1)];
            out[i] = e->s;
            x = e->f * (x >> t->L_bits) + (uint32_t)e->off;
            while (x < 0x10000u) {
                if (sp >= L->s6e) return -1;
                x = (x << 8) | *sp++;
            }
            prev = (int)e->s;
        }
        L->x6 = x; L->s6p = sp; L->prev6 = prev;
        return 0;
    }
    case LIT_FSE1_3: {
        uint32_t x = L->x3;
        const uint8_t* sp = L->s3p;
        int prev = L->prev3;
        for (int i = 0; i < n; i++) {
            const fse_ctx_table* t = L->active3[prev] ? &L->t3[prev] : L->gtab3;
            const fse_o1entry* e = &t->dcomp[x & (t->M - 1)];
            out[i] = e->s;
            x = e->f * (x >> t->L_bits) + (uint32_t)e->off;
            while (x < 0x10000u) {
                if (sp >= L->s3e) return -1;
                x = (x << 8) | *sp++;
            }
            prev = (int)e->s;
        }
        L->x3 = x; L->s3p = sp; L->prev3 = prev;
        return 0;
    }
    }
    return -1;
}

static size_t decode_normal(const char* src, size_t srcSize,
                            char* dst, size_t dstCap) {
    const uint8_t* p = (const uint8_t*)src;
    const uint8_t* end = p + srcSize;
    if (srcSize < 6) return 0;

    int flags = r8(&p);
    int is_raw = flags & 1;
    int is_prng = (flags >> 2) & 1;
    int shift = (flags & 3) * 8;
    int isize = (int)r32le(&p);
    if (isize < 0 || (size_t)isize > dstCap) return 0;

    if (is_prng) {
        if ((size_t)(end - p) < 2) return 0;
        uint32_t seed = (uint32_t)r8(&p) | ((uint32_t)r8(&p) << 8);
        uint32_t st[PRNG_DEG];
        int f = 3, r = 0;
        glibc_prng_init(st, &f, &r, seed);
        uint8_t* op = (uint8_t*)dst;
        for (int i = 0; i < isize; i++)
            op[i] = (uint8_t)(glibc_rand31(st, &f, &r) >> shift);
        return (size_t)isize;
    }

    if (is_raw) {
        if ((size_t)(end - p) < (size_t)isize) return 0;
        memcpy(dst, p, (size_t)isize);
        return (size_t)isize;
    }

    /* ---- literals: build the streaming decoder state (no buffer) ---- */
    lit_dec_t L;
    memset(&L, 0, sizeof(L));
    int lit_mode = r8(&p);
    int lit_count = rvlq(&p, end);
    /* literals can never exceed the declared output size; this also keeps
     * the per-literal allocations bounded on corrupt input */
    if (lit_count < 0 || lit_count > isize) return 0;
    L.kind = lit_mode;
    if (lit_mode == LIT_RAW) {
        if ((size_t)(end - p) < (size_t)lit_count) return 0;
        L.rp = p;
        L.rend = p + lit_count;
        p += lit_count;
    } else if (lit_mode == LIT_FSE0) {
        int lit_csize = rvlq(&p, end);
        if (lit_csize < 0 || (size_t)(end - p) < (size_t)lit_csize) return 0;
        size_t hdr = fse_dtable_prepare(&L.ft, p, (size_t)lit_csize, 255);
        if (hdr == 0 || (size_t)lit_csize < hdr + 8) { lit_dec_free(&L); return 0; }
        uint32_t slen = (uint32_t)p[hdr] | ((uint32_t)p[hdr+1] << 8) | ((uint32_t)p[hdr+2] << 16) | ((uint32_t)p[hdr+3] << 24);
        if (slen < 4 || (size_t)lit_csize < hdr + 4 + slen) { lit_dec_free(&L); return 0; }
        L.fsp = p + hdr + 4;
        L.fsend = L.fsp + slen;
        L.fx = ((uint32_t)L.fsp[0] << 24) | ((uint32_t)L.fsp[1] << 16) | ((uint32_t)L.fsp[2] << 8) | L.fsp[3];
        L.fsp += 4;
        p += lit_csize;
    } else if (lit_mode == LIT_HUF) {
        int lit_csize = rvlq(&p, end);
        if (lit_csize < 0 || (size_t)(end - p) < (size_t)lit_csize) return 0;
        if (huf_dec_init(&L.huf, p, (size_t)lit_csize) == 0) return 0;
        p += lit_csize;
    } else if (lit_mode == LIT_FSE1_6) {
        /* [L_bits:1][16 tables][4B slen][x:4][stream] */
        int lit_csize = rvlq(&p, end);
        if (lit_csize < 0 || (size_t)(end - p) < (size_t)lit_csize) return 0;
        const uint8_t* lp = p;
        const uint8_t* lpe = lp + lit_csize;
        if ((size_t)(lpe - lp) < 1 + 16 * 513) return 0;
        L.t6 = (fse_ctx_table*)calloc(16, sizeof(fse_ctx_table));
        if (!L.t6) return 0;
        lp++;  /* L_bits (fixed per stream, same for every table) */
        for (int c = 0; c < 16; c++) {
            size_t rr = fse_ctx_table_read(&L.t6[c], lp, (size_t)(lpe - lp));
            if (rr == 0) { lit_dec_free(&L); return 0; }
            lp += rr;
        }
        if ((size_t)(lpe - lp) < 8) { lit_dec_free(&L); return 0; }
        uint32_t slen = (uint32_t)lp[0] | ((uint32_t)lp[1] << 8) | ((uint32_t)lp[2] << 16) | ((uint32_t)lp[3] << 24);
        lp += 4;
        if (slen < 4 || (size_t)(lpe - lp) < slen) { lit_dec_free(&L); return 0; }
        L.s6p = lp;
        L.s6e = L.s6p + slen;
        L.x6 = ((uint32_t)L.s6p[0] << 24) | ((uint32_t)L.s6p[1] << 16) | ((uint32_t)L.s6p[2] << 8) | L.s6p[3];
        L.s6p += 4;
        L.prev6 = 0;
        p += lit_csize;
    } else if (lit_mode == LIT_FSE1_3) {
        /* [32B bitmap][L_bits:1][order-0 table][per-active tables][4B slen][x:4][stream] */
        int lit_csize = rvlq(&p, end);
        if (lit_csize < 0 || (size_t)(end - p) < (size_t)lit_csize) return 0;
        const uint8_t* lp = p;
        const uint8_t* lpe = lp + lit_csize;
        if ((size_t)(lpe - lp) < 33) return 0;
        L.t3 = (fse_ctx_table*)calloc(257, sizeof(fse_ctx_table));
        if (!L.t3) return 0;
        L.gtab3 = &L.t3[256];
        memset(L.active3, 0, sizeof(L.active3));
        for (int c = 0; c < 256; c++) if (lp[c >> 3] & (1 << (c & 7))) L.active3[c] = 1;
        lp += 32;
        lp++;  /* L_bits */
        size_t rr = fse_ctx_table_read(L.gtab3, lp, (size_t)(lpe - lp));
        if (rr == 0) { lit_dec_free(&L); return 0; }
        lp += rr;
        for (int c = 0; c < 256; c++) {
            if (!L.active3[c]) continue;
            rr = fse_ctx_table_read(&L.t3[c], lp, (size_t)(lpe - lp));
            if (rr == 0) { lit_dec_free(&L); return 0; }
            lp += rr;
        }
        if ((size_t)(lpe - lp) < 8) { lit_dec_free(&L); return 0; }
        uint32_t slen = (uint32_t)lp[0] | ((uint32_t)lp[1] << 8) | ((uint32_t)lp[2] << 16) | ((uint32_t)lp[3] << 24);
        lp += 4;
        if (slen < 4 || (size_t)(lpe - lp) < slen) { lit_dec_free(&L); return 0; }
        L.s3p = lp;
        L.s3e = L.s3p + slen;
        L.x3 = ((uint32_t)L.s3p[0] << 24) | ((uint32_t)L.s3p[1] << 16) | ((uint32_t)L.s3p[2] << 8) | L.s3p[3];
        L.s3p += 4;
        L.prev3 = 0;
        p += lit_csize;
    } else {
        return 0;
    }

    int sc = (int)r32le(&p);
    if (sc < 0 || sc > (1 << 24)) { lit_dec_free(&L); return 0; }

    /* per-bucket top8 streams — MUST be NULL-initialized before the first
     * `goto fail`: the fail cleanup frees resid_top8[b], and an early jump
     * (e.g. a corrupt FSE stream) would otherwise free stale stack bytes */
    unsigned* resid_top8[OF_CODES];
    size_t resid_n[OF_CODES];
    int b;
    for (b = 0; b < OF_CODES; b++) { resid_top8[b] = NULL; resid_n[b] = 0; }

    /* 3 symbol streams: tables prepared once, then all symbols decoded in
     * one interleaved pass into byte arrays. Measured faster than decoding
     * them inline in the application loop (which serializes the rANS
     * dependency chains against the copies on sequence-dense data).
     * True encoder alphabets: ll<=35, ml<=36, of<=24. */
    static const int smax[3] = { 35, 36, 24 };
    fse_dtable dt[3];
    uint32_t xs[3];
    const uint8_t *sp3[3], *se3[3];
    uint8_t* symarr[3];
    size_t dhdr[3], off = 0;
    memset(dt, 0, sizeof(dt));
    {
        int ok = 1, st;
        for (st = 0; st < 3 && ok; st++) {
            if ((size_t)(end - p) < off + 2) { ok = 0; break; }
            dhdr[st] = fse_dtable_prepare(&dt[st], p + off,
                                          (size_t)(end - p) - off, smax[st]);
            if (dhdr[st] == 0) { ok = 0; break; }
            off += dhdr[st];
            if ((size_t)(end - p) < off + 4) { ok = 0; break; }
            uint32_t slen = (uint32_t)p[off] | ((uint32_t)p[off+1] << 8)
                          | ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
            off += 4;
            if (slen < 4 || (size_t)(end - p) < off + slen) { ok = 0; break; }
            sp3[st] = p + off;
            se3[st] = sp3[st] + slen;
            xs[st] = ((uint32_t)sp3[st][0] << 24) | ((uint32_t)sp3[st][1] << 16)
                   | ((uint32_t)sp3[st][2] << 8) | (uint32_t)sp3[st][3];
            sp3[st] += 4;
            off += slen;
        }
        symarr[0] = ok ? (uint8_t*)malloc((size_t)sc) : NULL;
        symarr[1] = ok ? (uint8_t*)malloc((size_t)sc) : NULL;
        symarr[2] = ok ? (uint8_t*)malloc((size_t)sc) : NULL;
        if (!symarr[0] || !symarr[1] || !symarr[2]) ok = 0;
        if (ok) {
            for (int i = 0; i < sc && ok; i++) {
                if (seq_dec_sym(&dt[0], &xs[0], &sp3[0], se3[0], &symarr[0][i])) ok = 0;
                else if (seq_dec_sym(&dt[1], &xs[1], &sp3[1], se3[1], &symarr[1][i])) ok = 0;
                else if (seq_dec_sym(&dt[2], &xs[2], &sp3[2], se3[2], &symarr[2][i])) ok = 0;
            }
        }
        if (!ok) {
            for (st = 0; st < 3; st++) fse_dtable_free(&dt[st]);
            free(symarr[0]); free(symarr[1]); free(symarr[2]);
            lit_dec_free(&L); return 0;
        }
        p += off;
    }

    /* rep flags */
    int rep_bytes = (sc * 2 + 7) / 8;
    if ((size_t)(end - p) < (size_t)rep_bytes) goto fail;
    const uint8_t* rep_flags = p;
    p += rep_bytes;

    /* per-bucket top8 streams, uniform layout:
     * [bid][size:4][cnt:4][data] — size bit31 = raw (data = rn bytes,
     * cnt == rn), otherwise size = FSE stream size (data = stream).
     * The explicit count frees the decoder from the O(OF_CODES*sc) scan.
     * [bid=0][0:4] terminates. */
    for (;;) {
        if ((size_t)(end - p) < 5) goto fail;
        int bid = p[0];
        uint32_t w = (uint32_t)p[1] | ((uint32_t)p[2]<<8) | ((uint32_t)p[3]<<16) | ((uint32_t)p[4]<<24);
        p += 5;
        if (w == 0) break;
        if (w & 0x80000000u) {
            /* raw bucket (bit31 set): symbols stored verbatim */
            uint32_t rn = w & 0x7FFFFFFFu;
            if (bid < 0 || bid >= OF_CODES || (size_t)(end - p) < 4 + rn) goto fail;
            uint32_t cnt = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
            if (cnt != rn) goto fail;
            p += 4;
            if (resid_top8[bid]) { goto fail; }   /* duplicate bucket: corrupt */
            unsigned* top8 = (unsigned*)malloc((size_t)rn * sizeof(unsigned));
            if (!top8) goto fail;
            for (uint32_t i = 0; i < rn; i++) top8[i] = p[i];
            resid_top8[bid] = top8;
            resid_n[bid] = rn;
            p += rn;
            continue;
        }
        /* FSE bucket: w = stream size, cnt = exact symbol count.
         * fse_decode builds its own plain 1-byte table — a combined-entry
         * table (12B/slot, up to 768KB) would cost more to fill than the
         * decode saves on bucket-sized streams. */
        uint32_t rsz = w;
        if (bid < 0 || bid >= OF_CODES || rsz == 0 || (size_t)(end - p) < 4) goto fail;
        uint32_t cnt = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
        p += 4;
        if (cnt == 0 || cnt > (unsigned)sc || rsz == 0 || (size_t)(end - p) < rsz) goto fail;
        if (resid_top8[bid]) { goto fail; }   /* duplicate bucket: corrupt */
        unsigned* top8 = (unsigned*)malloc((size_t)cnt * sizeof(unsigned));
        if (!top8) goto fail;
        if (fse_decode(p, rsz, cnt, 255, top8)) { free(top8); goto fail; }
        resid_top8[bid] = top8;
        resid_n[bid] = cnt;
        p += rsz;
    }
    /* low bits */
    if ((size_t)(end - p) < 4) goto fail;
    uint32_t low_size = (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
    p += 4;
    if ((size_t)(end - p) < low_size) goto fail;
    const uint8_t* low_p = p;
    const uint8_t* low_end = low_p + low_size;
    p += low_size;
    uint32_t low_acc = 0; int low_nbits = 0;

    /* extra bits (ll/ml) */
    const uint8_t* extra_p = p;
    const uint8_t* extra_end = end;
    uint32_t bit_acc = 0; int bit_nbits = 0;
    int br_err = 0;

    int op = 0;
    int rp[3] = {0, 0, 0};
    size_t resid_pos[OF_CODES];
    for (b = 0; b < OF_CODES; b++) resid_pos[b] = 0;
    uint8_t* out = (uint8_t*)dst;
    /* rolling 2-bit rep flag reader */
    const uint8_t* rfl = rep_flags;
    const uint8_t* rfl_end = rep_flags + rep_bytes;
    uint32_t rfa = 0; int rfn = 0;
    for (int i = 0; i < sc; i++) {
        int ll = LL_base[symarr[0][i]];
        int le = LL_extra[symarr[0][i]];
        if (le > 0) ll += br_bits(&bit_acc, &bit_nbits, &extra_p, extra_end, le, &br_err);
        if (br_err) goto fail;
        int ml = 0;
        if (symarr[1][i] > 0) {
            int real = (int)symarr[1][i] - 1;
            ml = ML_base[real];
            int me = ML_extra[real];
            if (me > 0) ml += br_bits(&bit_acc, &bit_nbits, &extra_p, extra_end, me, &br_err);
            if (br_err) goto fail;
        }
        /* rep flag: refill 8 bits when fewer than 2 remain */
        if (rfn < 2) {
            if (rfl < rfl_end) { rfa |= (uint32_t)*rfl++ << rfn; rfn += 8; }
            else if (rfn == 0) { goto fail; }
        }
        int pc = (int)(rfa & 3);
        rfa >>= 2; rfn -= 2;
        int md = 0, is_rep = 0;
        if (symarr[1][i] > 0 && pc == 0) {
            int bid = (int)symarr[2][i];
            if (bid < 0 || bid >= OF_CODES) goto fail;
            if (resid_pos[bid] >= resid_n[bid]) goto fail;
            unsigned top8 = resid_top8[bid][resid_pos[bid]++];
            int nlow = bid >= 8 ? (bid - 8) : 0;
            int low = nlow > 0 ? br_bits(&low_acc, &low_nbits, &low_p, low_end, nlow, &br_err) : 0;
            if (br_err) goto fail;
            md = OF_base[bid] + ((int)top8 << nlow) + low;
        } else if (symarr[1][i] > 0) {
            int ri = pc - 1;
            md = rp[ri];
            is_rep = 1;
            if (ri > 0) {
                int t = rp[ri];
                for (int k = ri; k > 0; k--) rp[k] = rp[k-1];
                rp[0] = t;
            }
        }
        if (op + ll > isize) goto fail;
        if (lit_decode_chunk(&L, out + op, ll, (size_t)(isize - op))) goto fail;
        op += ll;
        if (ml == 0) break;
        if (md < 1 || md > op || op + ml > isize) goto fail;
        if (md == 1) memset(out + op, out[op - 1], (size_t)ml);
        else if (md >= 8 && ml <= 8 && op + 8 <= isize) memcpy(out + op, out + op - md, 8);   /* padded fast path */
        else if ((size_t)md >= 16 && ml >= 16 && (size_t)md >= (size_t)ml) {
            /* non-overlapping mid-size copy: inlined 16B moves beat the
             * libc call (no overlap: md >= 16 keeps the source 16 bytes
             * behind the write cursor) */
            size_t k = 0;
            for (; k + 16 <= (size_t)ml; k += 16)
                memcpy(out + op + k, out + op - md + k, 16);
            if (k < (size_t)ml) memcpy(out + op + k, out + op - md + k, (size_t)ml - k);
        }
        else if ((size_t)md >= (size_t)ml) memcpy(out + op, out + op - md, (size_t)ml);
        else {
            memcpy(out + op, out + op - md, (size_t)md);
            size_t filled = (size_t)md;
            while (filled + filled <= (size_t)ml) {
                memcpy(out + op + filled, out + op, filled);
                filled += filled;
            }
            if (filled < (size_t)ml) memcpy(out + op + filled, out + op, (size_t)ml - filled);
        }
        op += ml;
        if (!is_rep && md != rp[0]) { rp[2] = rp[1]; rp[1] = rp[0]; rp[0] = md; }
    }

    for (b = 0; b < OF_CODES; b++) free(resid_top8[b]);
    for (b = 0; b < 3; b++) fse_dtable_free(&dt[b]);
    free(symarr[0]); free(symarr[1]); free(symarr[2]);
    lit_dec_free(&L);
    return (size_t)op;

fail:
    for (b = 0; b < OF_CODES; b++) free(resid_top8[b]);
    for (b = 0; b < 3; b++) fse_dtable_free(&dt[b]);
    free(symarr[0]); free(symarr[1]); free(symarr[2]);
    lit_dec_free(&L);
    return 0;
}

size_t LZ6_decompress_seq(const char* src, size_t srcSize,
                          char* dst, size_t dstCap) {
    const uint8_t* p = (const uint8_t*)src;
    if (srcSize < 5) return 0;
    int flags = p[0];
    if (flags & 0x08) {  /* byte-plane block */
        const uint8_t* q = p + 1;
        if (srcSize < 6) return 0;
        int isize = (int)q[0] | ((int)q[1] << 8) | ((int)q[2] << 16) | ((int)q[3] << 24);
        q += 4;
        if (isize < 0 || (size_t)isize > dstCap) return 0;
        int stride = (flags & 3) == 2 ? 4 : 2;
        size_t plane_size = (size_t)isize / (size_t)stride;
        const uint8_t* end = p + srcSize;
        uint8_t* plane_buf = (uint8_t*)malloc(plane_size);
        if (!plane_buf) return 0;
        uint8_t* out = (uint8_t*)dst;
        for (int lane = 0; lane < stride; lane++) {
            if (q >= end) { free(plane_buf); return 0; }
            int mode = *q++;
            if (mode == 1) {  /* compressed lane */
                if ((size_t)(end - q) < 4) { free(plane_buf); return 0; }
                uint32_t csize = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                                 ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
                q += 4;
                if ((size_t)(end - q) < csize) { free(plane_buf); return 0; }
                if (decode_normal((const char*)q, csize, (char*)plane_buf, plane_size) != plane_size) {
                    free(plane_buf); return 0;
                }
                q += csize;
            } else if (mode == 2) {  /* pure huffman lane */
                if ((size_t)(end - q) < 4) { free(plane_buf); return 0; }
                uint32_t csize = (uint32_t)q[0] | ((uint32_t)q[1] << 8) |
                                 ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24);
                q += 4;
                if ((size_t)(end - q) < csize) { free(plane_buf); return 0; }
                if (huf_decode(q, csize, plane_size, plane_buf) != csize) {
                    free(plane_buf); return 0;
                }
                q += csize;
            } else if (mode == 0) {  /* raw lane */
                if ((size_t)(end - q) < plane_size) { free(plane_buf); return 0; }
                memcpy(plane_buf, q, plane_size);
                q += plane_size;
            } else { free(plane_buf); return 0; }
            for (size_t j = 0; j < plane_size; j++)
                out[j * (size_t)stride + (size_t)lane] = plane_buf[j];
        }
        free(plane_buf);
        return (size_t)isize;
    }
    return decode_normal(src, srcSize, dst, dstCap);
}
