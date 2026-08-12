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
 1,1,1,1,2,2,3,3,4,4,5,5,6,7,8,9,10,11,12,16
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

/* ---- byte helpers ---- */
static void w8(uint8_t** p, int v) { *(*p)++ = (uint8_t)v; }
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
    int v = 0;
    while (*p < end) { int b = *(*p)++; v += b; if (b != 255) return v; }
    return v;
}
static uint32_t br_bits(uint32_t* acc, int* nbits, const uint8_t** pp, int n) {
    while (*nbits < n) { *acc |= (uint32_t)*(*pp)++ << *nbits; *nbits += 8; }
    uint32_t v = *acc & ((1u << n) - 1);
    *acc >>= n; *nbits -= n;
    return v;
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

size_t LZ6_compress_seq(const char* src, size_t srcSize,
                        char* dst, size_t dstCap, int level) {
    if (!src || !dst || dstCap < 64) return 0;
    if (level < 1) level = 9;
    if (level > 15) level = 15;

    if (srcSize == 0) {
        /* empty input: raw block (flags=1 + isize=0) */
        if (dstCap < 5) return 0;
        uint8_t* rp = (uint8_t*)dst;
        w8(&rp, 1);
        w32le(&rp, 0);
        return 5;
    }

    /* Phase 1: lz6 match finding */
    size_t state_sz = (size_t)LZ6_sizeofStateHC();
    void* hc = malloc(state_sz);
    if (!hc) return 0;
    memset(hc, 0, state_sz);
    if (!LZ6_alloc_mem_HC_sized((LZ6HC_Data_Structure*)hc, level, srcSize)) {
        free(hc); return 0;   /* alloc returns 1 on success */
    }
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
        /* k>10 -> long codes force the slow walk decode; FSE wins there,
         * so skip the huffman stream entirely. */
        if (hhdr > 0 && hk <= 10) {
            size_t hstr = huf_encode_stream(huf_buf, lit_syms, (size_t)lit_count, huf_buf + hhdr, lit_cap - hhdr);
            if (hstr > 0) hsz = hhdr + hstr;
        }
        size_t lit_sz = 0;
        if (hsz == 0) {
            size_t lit_ts;
            lit_sz = fse_encode(lit_syms, (size_t)lit_count, 255, lit_buf, lit_cap, NULL, 0, &lit_ts);
        }
        if (hsz > 0) {
            w8(&p, 5);  /* lit_mode=huffman */
            p += wvlq(p, lit_count);
            p += wvlq(p, (int)hsz);
            if ((size_t)(p - blk) + hsz > blk_cap) { free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, huf_buf, hsz);
            p += hsz;
        } else if (lit_sz > 0 && lit_sz < (size_t)lit_count) {
            w8(&p, 1);  /* lit_mode=fse */
            p += wvlq(p, lit_count);
            p += wvlq(p, (int)lit_sz);
            if ((size_t)(p - blk) + lit_sz > blk_cap) { free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, lit_buf, lit_sz);
            p += lit_sz;
        } else {
            w8(&p, 0);  /* lit_mode=raw */
            p += wvlq(p, lit_count);
            if ((size_t)(p - blk) + (size_t)lit_count > blk_cap) { free(lit_buf); free(huf_buf); free(lit_syms); free(sc.lits); free(sc.lit_lens); free(sc.match_lens); free(sc.offsets); return 0; }
            memcpy(p, sc.lits, (size_t)lit_count);
            p += lit_count;
        }
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
        ll_syms[i] = (unsigned)ll_to_code(sc.lit_lens[i]);
        if (sc.match_lens[i] > 0) {
            ml_syms[i] = (unsigned)ml_to_code(sc.match_lens[i]) + 1;
            int of = sc.offsets[i];
            int pc = 0, ri = -1;
            if (of == rp[0]) { pc = 1; ri = 0; }
            else if (of == rp[1]) { pc = 2; ri = 1; }
            else if (of == rp[2]) { pc = 3; ri = 2; }
            if (pc == 0) {
                int bid = of_to_code(of);
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

    /* FSE encode 3 streams */
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

    /* per-bucket FSE streams of resid top8 + low bits */
    for (int b = 0; b < OF_CODES; b++) {
        if (resid_n[b] == 0) continue;
        w8(&p, (uint8_t)b);
        uint8_t* szp = p;
        p += 4;
        size_t r8_csz = fse_encode(resid_top8[b], resid_n[b], 255, p, blk_cap - (size_t)(p-blk), NULL, 0, &ts);
        if (r8_csz == 0) goto oom;
        szp[0] = (uint8_t)(r8_csz); szp[1] = (uint8_t)(r8_csz >> 8);
        szp[2] = (uint8_t)(r8_csz >> 16); szp[3] = (uint8_t)(r8_csz >> 24);
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

/* ================= DECODER ================= */

size_t LZ6_decompress_seq(const char* src, size_t srcSize,
                          char* dst, size_t dstCap) {
    const uint8_t* p = (const uint8_t*)src;
    const uint8_t* end = p + srcSize;
    if (srcSize < 6) return 0;

    int flags = r8(&p);
    int is_raw = flags & 1;
    int isize = (int)r32le(&p);
    if (isize < 0 || (size_t)isize > dstCap) return 0;

    if (is_raw) {
        if ((size_t)(end - p) < (size_t)isize) return 0;
        memcpy(dst, p, (size_t)isize);
        return (size_t)isize;
    }

    int lit_mode = r8(&p);
    int lit_count = rvlq(&p, end);
    uint8_t* literals = NULL;
    if (lit_mode == 0) {
        if ((size_t)(end - p) < (size_t)lit_count) return 0;
        literals = (uint8_t*)malloc((size_t)lit_count + 16);
        if (!literals) return 0;
        memcpy(literals, p, (size_t)lit_count);
        p += lit_count;
    } else if (lit_mode == 1) {
        int lit_csize = rvlq(&p, end);
        if ((size_t)(end - p) < (size_t)lit_csize) return 0;
        literals = (uint8_t*)malloc((size_t)lit_count + 16);
        if (!literals) return 0;
        unsigned* lit_dec = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
        if (!lit_dec) { free(literals); return 0; }
        if (fse_decode(p, (size_t)lit_csize, (size_t)lit_count, 255, lit_dec)) {
            free(lit_dec); free(literals); return 0;
        }
        for (int i = 0; i < lit_count; i++) literals[i] = (uint8_t)lit_dec[i];
        free(lit_dec);
        p += lit_csize;
    } else if (lit_mode == 5) {
        /* Huffman: 257-byte header + bitstream (fast table decode) */
        int lit_csize = rvlq(&p, end);
        if ((size_t)(end - p) < (size_t)lit_csize) return 0;
        literals = (uint8_t*)malloc((size_t)lit_count + 16);
        if (!literals) return 0;
        if (huf_decode(p, (size_t)lit_csize, (size_t)lit_count, literals) == 0) {
            free(literals); return 0;
        }
        p += lit_csize;
    } else if (lit_mode == 3) {
        /* FSE order-1: 32B bitmap + 1B L_bits + order-0 table + per-active tables + stream */
        int lit_csize = rvlq(&p, end);
        if ((size_t)(end - p) < (size_t)lit_csize) return 0;
        const uint8_t* lp = p;
        const uint8_t* lpe = lp + lit_csize;
        if (lpe - lp < 33) return 0;
        uint8_t active[256];
        memset(active, 0, sizeof(active));
        for (int c = 0; c < 256; c++) if (lp[c >> 3] & (1 << (c & 7))) active[c] = 1;
        lp += 32;
        (void)*lp++;  /* L_bits: fixed per-block, same value for every table */
        fse_ctx_table tables[256];
        memset(tables, 0, sizeof(tables));
        /* order-0 fallback table (ctx 256) */
        fse_ctx_table gtab;
        memset(&gtab, 0, sizeof(gtab));
        size_t rr = fse_ctx_table_read(&gtab, lp, (size_t)(lpe - lp));
        if (rr == 0) { free(literals); return 0; }
        lp += rr;
        for (int c = 0; c < 256; c++) {
            if (!active[c]) continue;
            rr = fse_ctx_table_read(&tables[c], lp, (size_t)(lpe - lp));
            if (rr == 0) { fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); free(literals); return 0; }
            lp += rr;
        }
        literals = (uint8_t*)malloc((size_t)lit_count + 16);
        if (!literals) { fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); return 0; }
        /* ctx array: prev literal byte, or 256 for inactive (order-0) */
        unsigned* ctxs = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
        unsigned* lit_dec = (unsigned*)malloc((size_t)lit_count * sizeof(unsigned));
        if (!ctxs || !lit_dec) { free(ctxs); free(lit_dec); fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); free(literals); return 0; }
        /* rebuild ctx array on the fly while decoding (prev = decoded byte) */
        int prev = 0;
        /* we need ctx[i] before decoding i; build it incrementally in the loop */
        /* decode with a temp ctx that we fill as we go */
        {
            /* first pass: we decode sequentially, ctx[i] = prev (already decoded) */
            const uint8_t* sp = lp;
            if ((size_t)(lpe - lp) < 8) { /* stream needs >= 4 */ }
            /* use fse_decode_ctx with a ctx array we fill progressively:
             * decode symbol i using ctx[i] = prev; then prev = symbol. */
            /* fse_decode_ctx takes the whole ctx array; we build it as we
             * decode by decoding one at a time is not supported — instead
             * we decode into lit_dec and track prev ourselves via a custom loop.
             * Simplest: decode the stream manually here. */
            uint32_t stream_len = (uint32_t)sp[0] | ((uint32_t)sp[1] << 8) | ((uint32_t)sp[2] << 16) | ((uint32_t)sp[3] << 24);
            sp += 4;
            if ((size_t)(lpe - sp) < stream_len) { free(ctxs); free(lit_dec); fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); free(literals); return 0; }
            const uint8_t* sp_end = sp + stream_len;
            uint32_t x = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) | ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
            sp += 4;
            for (int i = 0; i < lit_count; i++) {
                const fse_ctx_table* t = active[prev] ? &tables[prev] : &gtab;
                unsigned slot_idx = x & (t->M - 1);
                unsigned s = t->dtab[slot_idx];
                unsigned f = t->freq[s];
                if (f == 0) { free(ctxs); free(lit_dec); fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); free(literals); return 0; }
                lit_dec[i] = s;
                x = f * (x >> t->L_bits) + (x & (t->M - 1)) - t->cumul[s];
                while (x < 0x10000u) {
                    if (sp >= sp_end) { free(ctxs); free(lit_dec); fse_ctx_table_free(&gtab); for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]); free(literals); return 0; }
                    x = (x << 8) | *sp++;
                }
                literals[i] = (uint8_t)s;
                prev = (int)s;
            }
        }
        free(ctxs);
        free(lit_dec);
        fse_ctx_table_free(&gtab);
        for (int k = 0; k < 256; k++) fse_ctx_table_free(&tables[k]);
        p += lit_csize;
    } else {
        return 0;
    }

    int sc = (int)r32le(&p);
    if (sc < 0 || sc > (1 << 24)) { free(literals); return 0; }

    unsigned* ll_syms = (unsigned*)malloc((size_t)sc * sizeof(unsigned));
    unsigned* ml_syms = (unsigned*)malloc((size_t)sc * sizeof(unsigned));
    unsigned* of_syms = (unsigned*)malloc((size_t)sc * sizeof(unsigned));
    if (!ll_syms || !ml_syms || !of_syms) { free(ll_syms); free(ml_syms); free(of_syms); free(literals); return 0; }

    /* FSE decode 3 streams */
    unsigned counts[256]; int rmax, rL;
    for (int st = 0; st < 3; st++) {
        size_t hdr = fse_read_table(p, (size_t)(end - p), counts, &rmax, &rL);
        if (hdr == 0) goto fail;
        uint32_t slen = p[hdr] | (p[hdr+1]<<8) | (p[hdr+2]<<16) | (p[hdr+3]<<24);
        size_t total = hdr + 4 + slen;
        unsigned* dst_syms = st==0 ? ll_syms : st==1 ? ml_syms : of_syms;
        if (fse_decode(p, total, (size_t)sc, rmax, dst_syms)) goto fail;
        p += total;
    }

    /* rep flags */
    int rep_bytes = (sc * 2 + 7) / 8;
    if ((size_t)(end - p) < (size_t)rep_bytes) goto fail;
    const uint8_t* rep_flags = p;
    p += rep_bytes;

    /* per-bucket top8 streams */
    unsigned* resid_top8[OF_CODES];
    size_t resid_n[OF_CODES];
    int b;
    for (b = 0; b < OF_CODES; b++) { resid_top8[b] = NULL; resid_n[b] = 0; }
    for (;;) {
        if ((size_t)(end - p) < 5) goto fail;
        int bid = p[0];
        uint32_t rsz = (uint32_t)p[1] | ((uint32_t)p[2]<<8) | ((uint32_t)p[3]<<16) | ((uint32_t)p[4]<<24);
        p += 5;
        if (rsz == 0) break;
        if (bid < 0 || bid >= OF_CODES || (size_t)(end - p) < rsz) goto fail;
        unsigned cnt = 0;
        for (int j = 0; j < sc; j++) {
            int pcj = (rep_flags[(size_t)j * 2 / 8] >> ((j * 2) % 8)) & 3;
            if (pcj == 0 && ml_syms[j] > 0 && (int)of_syms[j] == bid) cnt++;
        }
        if (cnt == 0) { p += rsz; continue; }
        unsigned* top8 = (unsigned*)malloc(cnt * sizeof(unsigned));
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
    p += low_size;
    uint32_t low_acc = 0; int low_nbits = 0;

    /* extra bits (ll/ml) */
    const uint8_t* extra_p = p;
    uint32_t bit_acc = 0; int bit_nbits = 0;

    int op = 0, lp = 0;
    int rp[3] = {0, 0, 0};
    size_t resid_pos[OF_CODES];
    for (b = 0; b < OF_CODES; b++) resid_pos[b] = 0;
    uint8_t* out = (uint8_t*)dst;
    for (int i = 0; i < sc; i++) {
        int ll = LL_base[ll_syms[i]];
        int le = LL_extra[ll_syms[i]];
        if (le > 0) ll += (int)br_bits(&bit_acc, &bit_nbits, &extra_p, le);
        int ml = 0;
        if (ml_syms[i] > 0) {
            int real = (int)ml_syms[i] - 1;
            ml = ML_base[real];
            int me = ML_extra[real];
            if (me > 0) ml += (int)br_bits(&bit_acc, &bit_nbits, &extra_p, me);
        }
        int pc = (rep_flags[(size_t)i * 2 / 8] >> ((i * 2) % 8)) & 3;
        int md = 0, is_rep = 0;
        if (ml_syms[i] > 0 && pc == 0) {
            int bid = (int)of_syms[i];
            if (bid < 0 || bid >= OF_CODES) goto fail;
            if (resid_pos[bid] >= resid_n[bid]) goto fail;
            unsigned top8 = resid_top8[bid][resid_pos[bid]++];
            int nlow = bid >= 8 ? (bid - 8) : 0;
            int low = nlow > 0 ? (int)br_bits(&low_acc, &low_nbits, &low_p, nlow) : 0;
            md = OF_base[bid] + ((int)top8 << nlow) + low;
        } else if (ml_syms[i] > 0) {
            int ri = pc - 1;
            md = rp[ri];
            is_rep = 1;
            if (ri > 0) {
                int t = rp[ri];
                for (int k = ri; k > 0; k--) rp[k] = rp[k-1];
                rp[0] = t;
            }
        }
        if (lp + ll > lit_count || op + ll > isize) goto fail;
        memcpy(out + op, literals + lp, (size_t)ll);
        op += ll; lp += ll;
        if (ml == 0) break;
        if (md < 1 || md > op || op + ml > isize) goto fail;
        if (md == 1) memset(out + op, out[op - 1], (size_t)ml);
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
    free(ll_syms); free(ml_syms); free(of_syms); free(literals);
    return (size_t)op;

fail:
    for (b = 0; b < OF_CODES; b++) free(resid_top8[b]);
    free(ll_syms); free(ml_syms); free(of_syms); free(literals);
    return 0;
}
