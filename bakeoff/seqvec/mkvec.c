/* SPDX-License-Identifier: MIT */
/*
 * mkvec.c - hand assembler of lz6 seq blocks (format 2), for the cases the library's encoder never writes.
 *
 * It is independent of the library's encoder: the tANS tables, the backward bitstream, the bit-packed
 * headers, the Huffman code and the LEB128 counts are implemented here from lz6_Seq_format.md. The only
 * library function it calls is LZ6_decompress_seq, to check every block it has just written.
 *
 *   mkvec OUTDIR               writes the built-in cases as OUTDIR/a_NAME.seq and OUTDIR/a_NAME.orig, and
 *                              prints one line NAME<TAB>what it is per case
 *   mkvec -f DESC OUT.seq OUT.orig   assembles one block from a text description (see parse_desc)
 *   mkvec -big OUT.orig        writes the original of a_big_offsets (33.5 MB; it is not kept in the set)
 *
 * How a block is described (blk_t): the literal bytes and how they are cut into literal sections (mode 0,
 * 5 or 7; more than one section, or `force8`, gives mode 8), a list of sequences (ll, ml, offset; the
 * offset symbol is chosen by `how`), and the epoch layout (sequences per epoch and, per table, the table
 * log, "same as the previous epoch", and optionally the Rice parameter of its header). The original is
 * what the sequences produce: the assembler executes them itself.
 *
 * The tANS encoder is derived from the DECODE table of the specification, not from an encoder table: for
 * a symbol s and the state N the decoder must reach next, it looks for the entry u of s with
 * newState(u) <= N < newState(u) + 2^nbBits(u); u is the state before, and N - newState(u) the bits.
 * The state of the last sequence of an epoch is free (any entry of its symbol): the lowest one is taken.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "lz6seq.h"

static void die(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("mkvec: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---- byte buffer ---- */
typedef struct { uint8_t* d; size_t n, cap; } bb_t;
static void bb_room(bb_t* b, size_t more)
{
    if (b->n + more <= b->cap) return;
    b->cap = (b->n + more) * 2 + 64;
    b->d = (uint8_t*)realloc(b->d, b->cap);
    if (!b->d) die("out of memory");
}
static void bb_put(bb_t* b, unsigned v) { bb_room(b, 1); b->d[b->n++] = (uint8_t)v; }
static void bb_bytes(bb_t* b, const uint8_t* p, size_t n) { bb_room(b, n); if (n) memcpy(b->d + b->n, p, n); b->n += n; }
static void bb_le32(bb_t* b, uint32_t v) { for (int i = 0; i < 4; i++) bb_put(b, v >> (8 * i)); }
static void bb_leb(bb_t* b, uint32_t v)
{
    if (v >= (1u << 30)) die("count %u does not fit a LEB128 count", v);
    while (v >= 128) { bb_put(b, v | 128); v >>= 7; }
    bb_put(b, v);
}
static void bb_free(bb_t* b) { free(b->d); b->d = NULL; b->n = b->cap = 0; }

/* ---- LSB-first bit writer of the bit-packed headers ---- */
typedef struct { bb_t* b; unsigned acc, n; } hw_t;
static void hw_bits(hw_t* w, unsigned v, unsigned nb)
{
    for (unsigned i = 0; i < nb; i++) {
        w->acc |= ((v >> i) & 1u) << w->n;
        if (++w->n == 8) { bb_put(w->b, w->acc); w->acc = 0; w->n = 0; }
    }
}
static void hw_rice(hw_t* w, unsigned v, unsigned k)
{
    for (unsigned q = v >> k; q; q--) hw_bits(w, 1, 1);
    hw_bits(w, 0, 1);
    hw_bits(w, v & ((1u << k) - 1), k);
}
static unsigned hibit(uint32_t v) { unsigned b = 0; while (v >> (b + 1)) b++; return b; }
static void hw_gamma(hw_t* w, unsigned g)   /* nb zeros, a one, then the nb low bits of g from the top */
{
    const unsigned nb = hibit(g);
    if (g < 1 || nb > 8) die("gamma code of %u", g);
    hw_bits(w, 0, nb);
    hw_bits(w, 1, 1);
    for (int i = (int)nb - 1; i >= 0; i--) hw_bits(w, (g >> i) & 1u, 1);
}
static void hw_end(hw_t* w) { if (w->n) { bb_put(w->b, w->acc); w->acc = 0; w->n = 0; } }

/* ---- the code tables of the specification ---- */
static const int LL_base[36] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15, 16,18,20,22,24,28,32,36,40,48,56,64,80,96,112,128, 160,192,224,256 };
static const int ML_base[36] = { 3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18, 19,21,23,25,27,31,35,39,43,51,59,67,83,99,115,131, 163,195,227,259 };
static const int LM_extra[36] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 1,1,1,1,2,2,2,2,3,3,3,4,4,4,4,5,5,5,5,24 };
static const int alpha_max[3] = { 35, 36, 193 };
enum { T_LL, T_ML, T_OF };

static int lm_code(const int* base, uint32_t v)
{
    int c = 35;
    while (c > 0 && (uint32_t)base[c] > v) c--;
    if (v < (uint32_t)base[0] || v - (uint32_t)base[c] >= (1u << LM_extra[c])) die("length %u has no code", v);
    return c;
}
/* new-offset code and its raw bits */
static int of_code(uint32_t off, uint32_t* extra, int* nb)
{
    if (off < 1 || off >= (1u << 25)) die("offset %u outside 1 .. 2^25 - 1", off);
    if (off < 32) { *extra = 0; *nb = 0; return (int)off - 1; }
    const int b = (int)hibit(off), j = (int)(off & 7);
    *nb = b - 3;
    *extra = (off - (1u << b) - (uint32_t)j) >> 3;
    return 31 + 8 * (b - 5) + j;
}

/* ---- tANS ---- */
/* counts of the symbols 0..alphaMax -> normalised counts summing to 2^log, every symbol that occurs >= 1.
 * Returns maxSym. (Any such table is valid; this is not the library's rounding.) */
static int normalize(const unsigned* cnt, int alphaMax, int log, uint16_t* norm)
{
    const unsigned M = 1u << log;
    uint64_t total = 0;
    int ms = -1, present = 0;
    for (int s = 0; s <= alphaMax; s++) if (cnt[s]) { total += cnt[s]; ms = s; present++; }
    if (ms < 0) die("empty table");
    if ((unsigned)present > M) die("%d symbols do not fit a table of log %d", present, log);
    unsigned sum = 0;
    for (int s = 0; s <= ms; s++) {
        unsigned v = cnt[s] ? (unsigned)(((uint64_t)cnt[s] * M) / total) : 0;
        if (cnt[s] && !v) v = 1;
        norm[s] = (uint16_t)v;
        sum += v;
    }
    while (sum != M) {
        int best = -1;
        for (int s = 0; s <= ms; s++) {
            if (!norm[s] || (sum > M && norm[s] <= 1)) continue;
            if (best < 0 || norm[s] > norm[best]) best = s;
        }
        if (sum < M) { norm[best]++; sum++; } else { norm[best]--; sum--; }
    }
    return ms;
}
typedef struct { int log; uint8_t sym[4096], nb[4096]; uint16_t base[4096]; } dtab_t;
static void build_dtab(dtab_t* t, const uint16_t* norm, int maxSym, int log)
{
    const unsigned size = 1u << log, step = (size >> 1) + (size >> 3) + 3;
    unsigned pos = 0, next[256];
    for (int s = 0; s <= maxSym; s++) {
        next[s] = norm[s];
        for (unsigned i = 0; i < norm[s]; i++) { t->sym[pos] = (uint8_t)s; pos = (pos + step) & (size - 1); }
    }
    if (pos != 0) die("spread did not close");
    for (unsigned u = 0; u < size; u++) {
        const unsigned x = next[t->sym[u]]++;
        const unsigned nb = (unsigned)log - hibit(x);
        t->nb[u] = (uint8_t)nb;
        t->base[u] = (uint16_t)((x << nb) - size);
    }
    t->log = log;
}
/* the state u of symbol s from which the decoder goes to state N; N < 0: any state of s */
static int enc_state(const dtab_t* t, int s, int N, uint32_t* bits, int* nb)
{
    const int size = 1 << t->log;
    for (int u = 0; u < size; u++) {
        if (t->sym[u] != s) continue;
        if (N < 0) { *bits = 0; *nb = 0; return u; }
        if (N >= t->base[u] && N < t->base[u] + (1 << t->nb[u])) { *bits = (uint32_t)(N - t->base[u]); *nb = t->nb[u]; return u; }
    }
    die("symbol %d is not in its table", s);
    return 0;
}
/* [log:4][k:4][maxSym:8], counts of 0..maxSym-1 as Rice(k), each zero followed by gamma(further zeros + 1) */
static unsigned table2_bits(const uint16_t* norm, int maxSym, unsigned k)
{
    unsigned bits = 16;
    for (int s = 0; s < maxSym; s++) {
        bits += (norm[s] >> k) + 1 + k;
        if (!norm[s]) { int run = 0; while (s + 1 < maxSym && !norm[s + 1]) { s++; run++; } bits += 2 * hibit((unsigned)run + 1) + 1; }
    }
    return bits;
}
static void write_table2(bb_t* b, const uint16_t* norm, int maxSym, int log, int k)
{
    if (k < 0) {
        unsigned best = ~0u;
        for (unsigned kk = 0; kk <= 11; kk++) { const unsigned v = table2_bits(norm, maxSym, kk); if (v < best) { best = v; k = (int)kk; } }
    }
    hw_t w = { b, 0, 0 };
    hw_bits(&w, (unsigned)log, 4);
    hw_bits(&w, (unsigned)k, 4);
    hw_bits(&w, (unsigned)maxSym, 8);
    for (int s = 0; s < maxSym; s++) {
        hw_rice(&w, norm[s], (unsigned)k);
        if (!norm[s]) { int run = 0; while (s + 1 < maxSym && !norm[s + 1]) { s++; run++; } hw_gamma(&w, (unsigned)run + 1); }
    }
    hw_end(&w);
}

/* ---- Huffman (literal modes 5 and 7, plane lanes of mode 2) ---- */
static void huf_build_lengths(const uint8_t* d, size_t n, uint8_t* len)
{
    uint64_t w[511];
    int parent[511], alive[511], nn = 0, leaf[256];
    size_t cnt[256] = { 0 };
    for (size_t i = 0; i < n; i++) cnt[d[i]]++;
    memset(len, 0, 256);
    for (int s = 0; s < 256; s++) { leaf[s] = -1; if (cnt[s]) { leaf[s] = nn; w[nn] = cnt[s]; parent[nn] = -1; alive[nn] = 1; nn++; } }
    if (nn == 0) die("Huffman code of no literals");
    if (nn == 1) { for (int s = 0; s < 256; s++) if (cnt[s]) len[s] = 1; return; }
    for (int left = nn; left > 1; left--) {
        int a = -1, b = -1;
        for (int i = 0; i < nn; i++) {
            if (!alive[i]) continue;
            if (a < 0 || w[i] < w[a]) { b = a; a = i; } else if (b < 0 || w[i] < w[b]) b = i;
        }
        w[nn] = w[a] + w[b]; parent[nn] = -1; alive[nn] = 1;
        alive[a] = alive[b] = 0; parent[a] = parent[b] = nn;
        nn++;
    }
    for (int s = 0; s < 256; s++) if (leaf[s] >= 0) {
        int l = 0;
        for (int i = leaf[s]; parent[i] >= 0; i = parent[i]) l++;
        if (l > 16) die("Huffman code longer than 16 bits: give the lengths by hand");
        len[s] = (uint8_t)l;
    }
}
/* canonical codes, numbered from the longest length down */
static void huf_codes(const uint8_t* len, uint16_t* code)
{
    int count[18] = { 0 }, first[18] = { 0 };
    for (int s = 0; s < 256; s++) count[len[s]]++;
    first[16] = 0;
    for (int l = 15; l >= 1; l--) first[l] = (first[l + 1] + count[l + 1]) >> 1;
    for (int s = 0; s < 256; s++) if (len[s]) code[s] = (uint16_t)first[len[s]]++;
}
/* [total_bits:4][MSB-first bitstream] */
static void huf_stream(bb_t* b, const uint8_t* d, size_t n, const uint8_t* len, const uint16_t* code)
{
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++) { if (!len[d[i]]) die("literal %02x has no code", d[i]); total += len[d[i]]; }
    bb_le32(b, (uint32_t)total);
    unsigned acc = 0, nb = 0;
    for (size_t i = 0; i < n; i++)
        for (int k = len[d[i]] - 1; k >= 0; k--) {
            acc = (acc << 1) | ((code[d[i]] >> k) & 1u);
            if (++nb == 8) { bb_put(b, acc); acc = 0; nb = 0; }
        }
    if (nb) bb_put(b, acc << (8 - nb));
}
/* format 2 header: [k:4], then per symbol 0 + gamma(run of symbols without a code) or 1 + Rice1(zigzag(delta)).
 * split > 0 cuts every run into pieces of at most `split` symbols (allowed, never written by the library). */
static void huf_header2(bb_t* b, const uint8_t* len, int k, int split)
{
    hw_t w = { b, 0, 0 };
    int prev = 8;
    hw_bits(&w, (unsigned)k, 4);
    for (int s = 0; s < 256; ) {
        if (!len[s]) {
            int run = 1;
            while (s + run < 256 && !len[s + run]) run++;
            if (split > 0 && run > split) run = split;
            hw_bits(&w, 0, 1);
            hw_gamma(&w, (unsigned)run);
            s += run;
            continue;
        }
        const int d = len[s] - prev;
        prev = len[s];
        hw_bits(&w, 1, 1);
        hw_rice(&w, d < 0 ? (unsigned)(-2 * d - 1) : (unsigned)(2 * d), 1);
        s++;
    }
    hw_end(&w);
}

/* one literal section of a format 2 block. len: 256 code lengths, or NULL to build a code; k: the lookup width
 * written in the header (0: the longest code, at most 12) */
typedef struct { int mode; size_t n; const uint8_t* len; int k; int split; } lsec_t;
static void lit_section(bb_t* b, const lsec_t* sec, const uint8_t* d)
{
    const size_t n = sec->n;
    bb_put(b, (unsigned)sec->mode);
    bb_leb(b, (uint32_t)n);
    if (sec->mode == 0) { bb_bytes(b, d, n); return; }
    if (sec->mode != 5 && sec->mode != 7) die("literal mode %d is not assembled here", sec->mode);
    uint8_t lenbuf[256];
    uint16_t code[256];
    const uint8_t* len = sec->len;
    if (!len) { huf_build_lengths(d, n, lenbuf); len = lenbuf; }
    int k = sec->k;
    if (!k) { for (int s = 0; s < 256; s++) if (len[s] > k) k = len[s]; if (k > 12) k = 12; }
    huf_codes(len, code);
    bb_t t = { 0, 0, 0 };
    huf_header2(&t, len, k, sec->split);
    if (sec->mode == 5) huf_stream(&t, d, n, len, code);
    else {
        const size_t seg = (n + 3) / 4;
        for (size_t j = 0; j < 4; j++) {
            const size_t a = j * seg < n ? j * seg : n;
            huf_stream(&t, d + a, n - a < seg ? n - a : seg, len, code);
        }
    }
    bb_leb(b, (uint32_t)t.n);
    bb_bytes(b, t.d, t.n);
    bb_free(&t);
}

/* ---- the block description ---- */
enum { AUTO = 0, NEWOFF, REP0, REP1, REP2 };
/* how: AUTO = a rep code when the offset is on the rep stack, else a new-offset code; NEWOFF = a new-offset
 * code even when the offset is on the stack; REP0..2 = that rep code (off is ignored: the stack gives it).
 * ml = 0: the literals-only sequence (must be the last one). */
typedef struct { uint32_t ll, ml, off; int how; } seq_t;
/* lg[t] = 0: table t is "the same as the previous epoch" (byte 0); k[t] < 0: the cheapest Rice parameter */
typedef struct { int n; int lg[3]; int k[3]; } ep_t;
typedef struct {
    const uint8_t* lits; size_t nlits;
    const lsec_t* sec; int nsec; int force8;
    const seq_t* seq; int nseq;
    const ep_t* ep; int nep;
} blk_t;

typedef struct { uint32_t v; uint8_t nb; } field_t;
static struct { unsigned seen[3][194]; unsigned newoff_eq_rep0; uint32_t maxoff; } g_cov;

static void assemble(bb_t* out, const blk_t* b, bb_t* orig)
{
    const int n = b->nseq;
    int* sy[3];
    uint32_t* ex[3];
    uint8_t* exnb[3];
    for (int t = 0; t < 3; t++) {
        sy[t] = (int*)calloc((size_t)n + 1, sizeof(int));
        ex[t] = (uint32_t*)calloc((size_t)n + 1, sizeof(uint32_t));
        exnb[t] = (uint8_t*)calloc((size_t)n + 1, 1);
        if (!sy[t] || !ex[t] || !exnb[t]) die("out of memory");
    }
    memset(&g_cov, 0, sizeof g_cov);

    /* 1. the symbols, and the output the sequences produce */
    uint32_t rep[3] = { 0, 0, 0 };
    size_t lp = 0;
    orig->n = 0;
    for (int i = 0; i < n; i++) {
        const seq_t* s = &b->seq[i];
        if (lp + s->ll > b->nlits) die("sequence %d: the literals run out", i);
        bb_bytes(orig, b->lits + lp, s->ll);
        lp += s->ll;
        const int lc = lm_code(LL_base, s->ll);
        sy[T_LL][i] = lc; ex[T_LL][i] = s->ll - (uint32_t)LL_base[lc]; exnb[T_LL][i] = (uint8_t)LM_extra[lc];
        if (s->ml == 0) {
            if (i != n - 1) die("sequence %d: a literals-only sequence must be the last one", i);
            sy[T_ML][i] = 0; sy[T_OF][i] = -1;
            continue;
        }
        const int mc = lm_code(ML_base, s->ml);
        sy[T_ML][i] = mc + 1; ex[T_ML][i] = s->ml - (uint32_t)ML_base[mc]; exnb[T_ML][i] = (uint8_t)LM_extra[mc];
        int how = s->how;
        uint32_t off = s->off;
        if (how == AUTO) how = off == rep[0] ? REP0 : off == rep[1] ? REP1 : off == rep[2] ? REP2 : NEWOFF;
        if (how == NEWOFF) {
            int nb;
            sy[T_OF][i] = of_code(off, &ex[T_OF][i], &nb);
            exnb[T_OF][i] = (uint8_t)nb;
            if (off == rep[0]) g_cov.newoff_eq_rep0++;
            rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = off;      /* always pushed, also when it equals rep0 */
        } else {
            const int r = how - REP0;
            off = rep[r];
            sy[T_OF][i] = 191 + r;
            if (r == 2) rep[2] = rep[1];
            if (r >= 1) rep[1] = rep[0];
            rep[0] = off;
        }
        if (off < 1 || off > orig->n) die("sequence %d: offset %u beyond the %lu bytes produced", i, off, (unsigned long)orig->n);
        if (off > g_cov.maxoff) g_cov.maxoff = off;
        bb_room(orig, s->ml);
        for (uint32_t k = 0; k < s->ml; k++) orig->d[orig->n + k] = orig->d[orig->n + k - off];
        orig->n += s->ml;
    }
    if (lp != b->nlits) die("%lu literals are not used by any sequence", (unsigned long)(b->nlits - lp));
    if (orig->n > 0x7FFFFFFFu) die("block too large");

    /* 2. header and literals */
    bb_put(out, 0x20);
    bb_le32(out, (uint32_t)orig->n);
    if (b->nsec == 1 && !b->force8) {
        if (b->sec[0].n != b->nlits) die("the literal section does not cover the literals");
        lit_section(out, &b->sec[0], b->lits);
    } else {
        bb_t t = { 0, 0, 0 };
        size_t at = 0;
        if (b->nsec < 1 || b->nsec > 256) die("1 to 256 literal sections");
        bb_leb(&t, (uint32_t)b->nsec);
        for (int i = 0; i < b->nsec; i++) { lit_section(&t, &b->sec[i], b->lits + at); at += b->sec[i].n; }
        if (at != b->nlits) die("the literal sections do not cover the literals");
        bb_put(out, 8);
        bb_leb(out, (uint32_t)b->nlits);
        bb_leb(out, (uint32_t)t.n);
        bb_bytes(out, t.d, t.n);
        bb_free(&t);
    }
    bb_le32(out, (uint32_t)n);
    if (n == 0) goto done;

    /* 3. epochs: tables, then the fields of the bitstream in the order the decoder reads them */
    {
        int tot = 0;
        if (b->nep < 1 || b->nep > 1024) die("1 to 1024 epochs");
        for (int e = 0; e < b->nep; e++) { if (b->ep[e].n < 1) die("empty epoch"); tot += b->ep[e].n; }
        if (tot != n) die("the epochs hold %d sequences, the block %d", tot, n);
        /* the of symbol of a literals-only sequence is not used by the decoder: take one its table has anyway */
        if (sy[T_OF][n - 1] < 0) sy[T_OF][n - 1] = b->ep[b->nep - 1].n > 1 ? sy[T_OF][n - 2] : 0;
        for (int i = 0; i < n; i++) for (int t = 0; t < 3; t++) g_cov.seen[t][sy[t][i]]++;

        field_t* F = (field_t*)malloc(((size_t)n * 6 + (size_t)b->nep * 3 + 1) * sizeof(field_t));
        size_t nf = 0;
        static dtab_t dt[3];
        static uint16_t norm[3][256];
        int ms[3] = { 0, 0, 0 };
        int* st[3];
        for (int t = 0; t < 3; t++) st[t] = (int*)malloc(((size_t)n + 1) * sizeof(int));
        if (!F || !st[0] || !st[1] || !st[2]) die("out of memory");
        bb_leb(out, (uint32_t)b->nep);
        int a = 0;
        for (int e = 0; e < b->nep; e++) {
            const ep_t* ep = &b->ep[e];
            const int z = a + ep->n;
            bb_leb(out, (uint32_t)ep->n);
            for (int t = 0; t < 3; t++) {
                if (ep->lg[t] == 0) {
                    if (e == 0) die("the first epoch cannot repeat a table");
                    bb_put(out, 0);
                    continue;
                }
                if (ep->lg[t] < 5 || ep->lg[t] > 12) die("table log %d", ep->lg[t]);
                /* the table also serves the epochs that repeat it */
                unsigned cnt[194] = { 0 };
                int q = a;
                for (int f = e; f < b->nep && (f == e || b->ep[f].lg[t] == 0); f++)
                    for (int i = 0; i < b->ep[f].n; i++) cnt[sy[t][q++]]++;
                ms[t] = normalize(cnt, alpha_max[t], ep->lg[t], norm[t]);
                build_dtab(&dt[t], norm[t], ms[t], ep->lg[t]);
                write_table2(out, norm[t], ms[t], ep->lg[t], ep->k[t]);
            }
            /* states, from the last sequence of the epoch back */
            uint32_t* ubits[3]; int* unb[3];
            for (int t = 0; t < 3; t++) {
                ubits[t] = (uint32_t*)malloc((size_t)ep->n * sizeof(uint32_t));
                unb[t] = (int*)malloc((size_t)ep->n * sizeof(int));
                if (!ubits[t] || !unb[t]) die("out of memory");
                st[t][z - 1] = enc_state(&dt[t], sy[t][z - 1], -1, &ubits[t][ep->n - 1], &unb[t][ep->n - 1]);
                for (int i = z - 2; i >= a; i--) st[t][i] = enc_state(&dt[t], sy[t][i], st[t][i + 1], &ubits[t][i - a], &unb[t][i - a]);
            }
            /* read order: the three initial states (ll, of, ml), then per sequence the offset bits, the ml
             * extra bits, the ll extra bits and, unless it is the last of the epoch, the updates of ll, ml, of */
            F[nf].v = (uint32_t)st[T_LL][a]; F[nf++].nb = (uint8_t)dt[T_LL].log;
            F[nf].v = (uint32_t)st[T_OF][a]; F[nf++].nb = (uint8_t)dt[T_OF].log;
            F[nf].v = (uint32_t)st[T_ML][a]; F[nf++].nb = (uint8_t)dt[T_ML].log;
            for (int i = a; i < z; i++) {
                if (sy[T_ML][i]) {
                    if (sy[T_OF][i] <= 190) { F[nf].v = ex[T_OF][i]; F[nf++].nb = exnb[T_OF][i]; }
                    F[nf].v = ex[T_ML][i]; F[nf++].nb = exnb[T_ML][i];
                }
                F[nf].v = ex[T_LL][i]; F[nf++].nb = exnb[T_LL][i];
                if (i < z - 1) {
                    F[nf].v = ubits[T_LL][i - a]; F[nf++].nb = (uint8_t)unb[T_LL][i - a];
                    F[nf].v = ubits[T_ML][i - a]; F[nf++].nb = (uint8_t)unb[T_ML][i - a];
                    F[nf].v = ubits[T_OF][i - a]; F[nf++].nb = (uint8_t)unb[T_OF][i - a];
                }
            }
            for (int t = 0; t < 3; t++) { free(ubits[t]); free(unb[t]); }
            a = z;
        }
        /* 4. the bitstream: written forward LSB-first in the reverse of the read order, closed by a 1 bit */
        bb_t bits = { 0, 0, 0 };
        uint64_t acc = 0;
        unsigned nacc = 0;
        for (size_t i = nf; i-- > 0; ) {
            if (F[i].nb && (F[i].v >> F[i].nb)) die("field does not fit its width");
            acc |= (uint64_t)F[i].v << nacc;
            nacc += F[i].nb;
            while (nacc >= 8) { bb_put(&bits, (unsigned)acc); acc >>= 8; nacc -= 8; }
        }
        acc |= (uint64_t)1 << nacc;
        bb_put(&bits, (unsigned)acc);
        bb_le32(out, (uint32_t)bits.n + 8);
        for (int i = 0; i < 8; i++) bb_put(out, 0);
        bb_bytes(out, bits.d, bits.n);
        bb_free(&bits);
        free(F);
        for (int t = 0; t < 3; t++) free(st[t]);
    }
done:
    for (int t = 0; t < 3; t++) { free(sy[t]); free(ex[t]); free(exnb[t]); }
}

/* ---- output and self check ---- */
/* what each built-in case is (printed as NAME<TAB>text, for the manifest) */
static const char* const DESC[][2] = {
    { "raw_lits", "raw literals (mode 0, count 63) next to 4 sequences; three tables of log 5" },
    { "sc0_empty", "sc = 0 with isize = 0: flags, isize, an empty raw literal section, sc; 11 bytes, no output" },
    { "empty_stored", "the empty block: stored, isize 0, 5 bytes" },
    { "lits_only", "sc = 1, literals only (ll 300): ml and of tables of the one symbol 0 (maxSym 0), ll table of the one symbol 35" },
    { "last_match", "the last sequence is a match that ends exactly at isize; no literals-only sequence" },
    { "empty_last_seq", "a match completes the output and an empty literals-only sequence (ll 0, ml symbol 0) follows" },
    { "newoff_rep0", "a new-offset code equal to rep0 (pushed again), then rep1 and rep2, which differ with and without the push" },
    { "reps", "rep0, rep1 and rep2 hits over three different offsets, 8 rep codes in 11 sequences" },
    { "logs", "6 epochs, table logs 5..12 mixed, 5, 6 and 12 in each of ll, ml, of" },
    { "table_repeat", "7 epochs; byte 0 (same table as the previous epoch) for ll alone, ml alone, of alone, all three, and twice in a row" },
    { "two_symbols", "tables of two symbols with maxSym 1 (ll 0/1, ml none/3, offsets 1/2 as new-offset codes)" },
    { "one_symbol", "tables of one symbol that is not symbol 0 (ll 4, ml 3, offset 4), logs 5, 6, 7; ends on a match" },
    { "zero_runs", "long zero runs in the table headers: gamma 34 (ll {0,35}), 35 (ml {0,36}), 188 (of {0,1,2,191,192,193}); ll and ml escape codes" },
    { "rice_k", "table headers with forced Rice parameters: k 0 on log 12 (long unary quotients), k 11 on log 12, k 5 on log 5" },
    { "all_codes", "every ll code 0..35, every ml symbol 0..36, every new-offset code 0..126 (offsets 1..31 and all 8 codes of buckets 5..16); 5 epochs, logs 6..12" },
    { "offset1_run", "one literal, then one match of 99999 bytes at offset 1 (ml escape code); ends on the match" },
    { "huf_one_symbol", "mode 5 with the one-symbol code (0xFF, one bit each; header = one run of 255, gamma 255); ll 70000 (ll escape code); match at offset 70000" },
    { "huf_deep", "mode 5 with code lengths 1, 2, .. 15, 16, 16 and lookup width k = 6" },
    { "huf_flat", "mode 5 with all 256 symbols at length 8" },
    { "huf_split_runs", "mode 5 whose header cuts the runs of absent symbols into pieces of at most 7 (valid, not what the library writes)" },
    { "huf4_tiny", "mode 7 with 5 literals: streams of 2, 2, 1 and 0 literals" },
    { "huf4_small", "mode 7 with 1001 literals (251, 251, 251, 248)" },
    { "lit8_mixed", "mode 8 with 5 sections of modes 0, 5, 7, 0, 5 (the last a one-symbol code)" },
    { "lit8_one", "mode 8 with a single section (mode 5)" },
    { "lit8_256", "mode 8 with 256 sections (the maximum) of 1 to 4 literals, raw and mode 5 in turn" },
    { "lit8_empty_sec", "mode 8 with raw sections of count 0 at the start, in the middle and at the end" },
    { "huf_jumps", "mode 5 whose lengths jump 16, 1, 16, 2, 15, 3, .. in symbol order (Rice quotients up to 14), k = 12" },
    { "plane2_flags0", "plane block of stride 2 written with flags 08 (bits 0-1 = 0); lane 0 a seq block that is a PRNG block (shift 8), lane 1 raw" },
    { "plane2_flags1", "the same with flags 09 (bits 0-1 = 1)" },
    { "plane2_flags3", "the same with flags 0B (bits 0-1 = 3)" },
    { "1024_epochs", "1024 epochs of one sequence each (the maximum; epochs = sc); two of three repeat all three tables; one-symbol tables of every log 5..12; ends on a match" },
    { "plane_mixed", "plane block, stride 4: lane 0 a seq block that is a stored block, lane 1 raw + delta, lane 2 a format 2 block + delta, lane 3 Huffman + delta" },
    { "x86_plane", "x86 block around the plane block of a_plane_mixed; one range of 5 bytes without E8/E9 (the filter changes nothing)" },
    { "x86_stored", "x86 block around a stored block, two ranges that touch; text without E8/E9 (the filter changes nothing)" },
    { "prng_shift24", "PRNG block with shift 24 (flags 07), seed 4242" },
    { "prng_seed0", "PRNG block with seed 0, shift 0" },
    { "big_offsets", "offsets above 2^24: 601 literals, the longest match (16777474) at offset 601, the longest match at offset 16777516, a match at the largest offset 2^25 - 1, 64 matches with every new-offset code 127..190 (buckets 17..24), 7 literals; original = byte i is ((i % 601) * 37) ^ ((i % 601) >> 3), not stored" },
};
static const char* g_outdir;
static void put_file(const char* name, const uint8_t* d, size_t n)
{
    FILE* f = fopen(name, "wb");
    if (!f || (n && fwrite(d, 1, n, f) != n) || fclose(f)) die("cannot write %s", name);
}
/* the library must decode the block to `orig` (an empty original cannot be checked: the API returns 0 for it and for an error) */
static void check(const char* name, const bb_t* blk, const bb_t* orig)
{
    uint8_t* out = (uint8_t*)malloc(orig->n + 64);
    if (!out) die("out of memory");
    const size_t r = LZ6_decompress_seq((const char*)blk->d, blk->n, (char*)out, orig->n);
    if (r != orig->n || (orig->n && memcmp(out, orig->d, orig->n))) die("%s: LZ6_decompress_seq does not give the original back (returned %lu of %lu)", name, (unsigned long)r, (unsigned long)orig->n);
    free(out);
}
static void emit_raw(const char* name, const bb_t* blk, const bb_t* orig, int keep_orig)
{
    char path[1024];
    check(name, blk, orig);
    snprintf(path, sizeof path, "%s/a_%s.seq", g_outdir, name);
    put_file(path, blk->d, blk->n);
    if (keep_orig) { snprintf(path, sizeof path, "%s/a_%s.orig", g_outdir, name); put_file(path, orig->d, orig->n); }
    const char* what = NULL;
    for (size_t i = 0; i < sizeof DESC / sizeof DESC[0]; i++) if (!strcmp(DESC[i][0], name)) what = DESC[i][1];
    if (!what) die("%s has no description", name);
    printf("a_%s\t%s\n", name, what);
}

/* ---- case building ---- */
static uint64_t g_rs = 1;
static uint32_t rnd(void) { g_rs = g_rs * 6364136223846793005ULL + 1442695040888963407ULL; return (uint32_t)(g_rs >> 33); }

static bb_t L;                        /* literals */
static seq_t* SQ; static int nsq, capsq;
static ep_t EP[1024]; static int nep;
static lsec_t SEC[256]; static int nsec; static int force8;
static size_t produced;               /* bytes the sequences so far produce */

static void reset(uint64_t seed) { L.n = 0; nsq = 0; nep = 0; nsec = 0; force8 = 0; produced = 0; g_rs = seed; }
static void S(uint32_t ll, uint32_t ml, uint32_t off, int how)
{
    if (nsq == capsq) { capsq = capsq * 2 + 64; SQ = (seq_t*)realloc(SQ, (size_t)capsq * sizeof(seq_t)); if (!SQ) die("out of memory"); }
    SQ[nsq].ll = ll; SQ[nsq].ml = ml; SQ[nsq].off = off; SQ[nsq].how = how;
    nsq++;
    produced += ll + ml;
}
static void lits_rnd(size_t n) { for (size_t i = 0; i < n; i++) bb_put(&L, rnd() >> 8); }
static void lits_alpha(size_t n, const char* alphabet) { const size_t a = strlen(alphabet); for (size_t i = 0; i < n; i++) bb_put(&L, (unsigned char)alphabet[rnd() % a]); }
/* a sequence with ll random literals in front */
static void SR(uint32_t ll, uint32_t ml, uint32_t off, int how) { lits_rnd(ll); S(ll, ml, off, how); }
static void E(int n, int lgll, int lgml, int lgof)
{
    EP[nep].n = n; EP[nep].lg[0] = lgll; EP[nep].lg[1] = lgml; EP[nep].lg[2] = lgof;
    EP[nep].k[0] = EP[nep].k[1] = EP[nep].k[2] = -1;
    nep++;
}
static void K(int kll, int kml, int kof) { EP[nep - 1].k[0] = kll; EP[nep - 1].k[1] = kml; EP[nep - 1].k[2] = kof; }
static void SECT(int mode, size_t n, const uint8_t* len, int k, int split)
{
    SEC[nsec].mode = mode; SEC[nsec].n = n; SEC[nsec].len = len; SEC[nsec].k = k; SEC[nsec].split = split;
    nsec++;
}
static void build(bb_t* blk, bb_t* orig)
{
    blk_t b;
    if (nsec == 0) SECT(0, L.n, NULL, 0, 0);
    if (nep == 0 && nsq) E(nsq, 9, 9, 9);
    b.lits = L.d; b.nlits = L.n; b.sec = SEC; b.nsec = nsec; b.force8 = force8;
    b.seq = SQ; b.nseq = nsq; b.ep = EP; b.nep = nep;
    blk->n = 0;
    assemble(blk, &b, orig);
}
static void emit(const char* name)
{
    bb_t blk = { 0, 0, 0 }, orig = { 0, 0, 0 };
    build(&blk, &orig);
    emit_raw(name, &blk, &orig, 1);
    bb_free(&blk); bb_free(&orig);
}
/* `count` sequences from small alphabets (ll 0..15, ml 3..18, offsets 1..16: at most 16, 17 and 19 symbols, so
 * every table fits a log of 5); random literals */
static void small_seqs(int count)
{
    for (int i = 0; i < count; i++) {
        const uint32_t ll = produced ? rnd() % 16 : 8 + rnd() % 8;
        uint32_t off = 1 + rnd() % 16;
        if (off > produced + ll) off = (uint32_t)(produced + ll);
        SR(ll, 3 + rnd() % 16, off, AUTO);
    }
}

/* the original of a_big_offsets: period 601 (2^25 - 1 = 31 * 601 * 1801) */
static uint8_t big_byte(size_t i) { const unsigned r = (unsigned)(i % 601); return (uint8_t)((r * 37) ^ (r >> 3)); }
#define BIG_ML 16777474u   /* the longest match one sequence can code: ML_base[35] + 2^24 - 1 */
static void big_case(int write_block, const char* orig_path)
{
    /* 601 literals, then three matches whose offsets are multiples of 601, so the output is the pattern
     * big_byte(i) all the way: offset 601 (the longest match), offset 601 * 27916 = 16,777,516 (just above
     * 2^24, again the longest match), offset 2^25 - 1 = 601 * 55831 (the largest offset of the format), 64 short
     * matches that use every new-offset code of the buckets 17..24, then 7 literals. An offset that is not a
     * multiple of 601 breaks the pattern. */
    reset(1);
    S(601, BIG_ML, 601, NEWOFF);
    S(0, BIG_ML, 16777516u, NEWOFF);
    S(0, 1000, 33554431u, NEWOFF);
    /* every new-offset code of the buckets 17..24 (codes 127..190): a multiple k * 601 inside the bucket with
     * k = j mod 8, which is the offset's low 3 bits because 601 = 1 mod 8 */
    for (int bk = 17; bk <= 24; bk++) for (uint32_t j = 0; j < 8; j++) {
        const uint32_t lo = ((1u << bk) + 600) / 601, hi = ((2u << bk) - 1) / 601;
        uint32_t k = lo + rnd() % (hi - lo - 8);
        while (k % 8 != j) k++;
        S((uint32_t)(bk + (int)j) % 3, 3 + rnd() % 60, k * 601, NEWOFF);
    }
    S(7, 0, 0, AUTO);
    size_t pos = 0;
    for (int i = 0; i < nsq; i++) { for (uint32_t k = 0; k < SQ[i].ll; k++) bb_put(&L, big_byte(pos + k)); pos += SQ[i].ll + SQ[i].ml; }
    E(nsq, 7, 7, 8);
    bb_t blk = { 0, 0, 0 }, orig = { 0, 0, 0 };
    build(&blk, &orig);
    {
        int c = 0;
        for (int q = 127; q <= 190; q++) c += g_cov.seen[T_OF][q] != 0;
        if (c != 64) die("big: %d of the 64 offset codes 127..190", c);
    }
    for (size_t i = 0; i < orig.n; i++) if (orig.d[i] != big_byte(i)) die("big: the output is not the pattern at %lu", (unsigned long)i);
    if (write_block) emit_raw("big_offsets", &blk, &orig, 0);
    if (orig_path) put_file(orig_path, orig.d, orig.n);
    bb_free(&blk); bb_free(&orig);
}

/* a block that produces exactly n bytes (for plane lanes) */
static void block_of(size_t n, bb_t* blk, bb_t* orig)
{
    reset(77);
    while (produced + 64 < n) small_seqs(1);
    SR((uint32_t)(n - produced), 0, 0, AUTO);
    E(nsq, 6, 6, 6);
    build(blk, orig);
    if (orig->n != n) die("block_of");
}

static void cases(void)
{
    bb_t blk = { 0, 0, 0 }, orig = { 0, 0, 0 };

    /* raw literals (mode 0, count > 0) next to sequences; tables of log 5 */
    reset(1);
    {
        static const char text[] = "the quick brown fox jumps over the lazy dog, twice; then stops.";
        bb_bytes(&L, (const uint8_t*)text, 63);
        S(20, 10, 5, AUTO); S(8, 4, 30, AUTO); S(0, 3, 1, AUTO); S(35, 0, 0, AUTO);
        E(4, 5, 5, 5);
        emit("raw_lits");
    }
    /* sc = 0 with isize = 0: [20][isize 0][lit_mode 0][lit_count 0][sc 0], 11 bytes, no output */
    reset(1);
    emit("sc0_empty");
    /* the empty block of the encoder: stored, isize 0, 5 bytes */
    blk.n = 0; orig.n = 0;
    bb_put(&blk, 0x01); bb_le32(&blk, 0);
    emit_raw("empty_stored", &blk, &orig, 1);

    /* sc = 1, only literals: the ml and of tables have the one symbol 0 (maxSym 0, a 2-byte header), the ll
     * table the one symbol 35 (zero run of 35 counts in front of it) */
    reset(2);
    SR(300, 0, 0, AUTO);
    E(1, 5, 5, 5);
    emit("lits_only");

    /* the last sequence is a match that ends exactly at isize (no literals-only sequence) */
    reset(3);
    SR(8, 8, 8, AUTO); SR(0, 5, 3, AUTO);
    E(2, 5, 5, 5);
    emit("last_match");

    /* a match completes the output and an EMPTY literals-only sequence (ll 0, ml symbol 0) still follows it (the
     * encoder's parsers always end with a (lastRun, 0, 0) sequence; whether their lastRun can be 0 was not checked) */
    reset(31);
    SR(12, 9, 4, AUTO); SR(3, 6, 20, AUTO); S(0, 0, 0, AUTO);
    E(3, 5, 5, 5);
    emit("empty_last_seq");

    /* a new-offset code equal to rep0 is pushed: stack {13,13,7} after the third sequence, so rep1 is 13 (7
     * without the push) and rep2 is 7 */
    reset(4);
    SR(40, 5, 7, AUTO); SR(4, 6, 13, AUTO); SR(2, 4, 13, NEWOFF); SR(3, 5, 0, REP1); SR(1, 6, 0, REP2); SR(5, 0, 0, AUTO);
    E(6, 5, 5, 5);
    emit("newoff_rep0");

    /* rep0, rep1 and rep2 hits with three different offsets on the stack */
    reset(5);
    SR(50, 4, 10, AUTO); SR(5, 5, 23, AUTO); SR(5, 6, 37, AUTO);     /* {37,23,10} */
    SR(3, 4, 0, REP0);                                              /* 37 */
    SR(4, 5, 0, REP1);                                              /* 23 -> {23,37,10} */
    SR(2, 7, 0, REP2);                                              /* 10 -> {10,23,37} */
    SR(6, 3, 0, REP2);                                              /* 37 -> {37,10,23} */
    SR(0, 8, 0, REP1);                                              /* 10 -> {10,37,23} */
    SR(1, 9, 0, REP0);                                              /* 10 */
    SR(7, 5, 23, AUTO);                                             /* found as rep2 */
    SR(9, 0, 0, AUTO);
    E(11, 6, 6, 6);
    emit("reps");

    /* several epochs with different table logs, 5, 6 and 12 among them, in every table */
    reset(6);
    small_seqs(250);
    E(40, 5, 6, 12); E(40, 12, 5, 6); E(40, 6, 12, 5); E(40, 7, 8, 9); E(40, 10, 11, 7); E(50, 5, 5, 5);
    emit("logs");

    /* "same table as the previous epoch" (byte 0) for each of ll, ml, of, alone and together */
    reset(7);
    small_seqs(280);
    E(40, 8, 8, 8); E(40, 0, 8, 8); E(40, 8, 0, 8); E(40, 8, 8, 0); E(40, 0, 0, 0); E(40, 7, 0, 9); E(40, 0, 0, 0);
    emit("table_repeat");

    /* tables of two symbols, maxSym 1: ll 0 or 1, ml 3 or none, offsets 1 or 2 as new-offset codes */
    reset(8);
    SR(1, 3, 1, NEWOFF);
    for (int i = 0; i < 40; i++) SR(rnd() & 1, 3, 1 + (rnd() & 1), NEWOFF);
    SR(1, 0, 0, AUTO);
    E(nsq, 5, 5, 5);
    emit("two_symbols");

    /* tables of one symbol that is not symbol 0: every ll is 4, every ml 3, every offset 4 (new-offset code 3) */
    reset(9);
    for (int i = 0; i < 20; i++) SR(4, 3, 4, NEWOFF);
    E(nsq, 5, 6, 7);
    emit("one_symbol");

    /* long zero runs in the table headers: ll symbols {0, 35} (gamma 34), ml symbols {0, 36} (gamma 35),
     * of symbols {0, 1, 2, 191, 192, 193} (gamma 188: the longest a table can need is 193) */
    reset(10);
    SR(300, 259, 1, NEWOFF); SR(0, 260, 2, NEWOFF); SR(0, 300, 3, NEWOFF);
    SR(300, 259, 0, REP2); SR(0, 259, 0, REP1); SR(0, 270, 0, REP0); SR(300, 0, 0, AUTO);
    E(7, 5, 5, 5);
    emit("zero_runs");

    /* table headers with a forced Rice parameter: 0 (long unary quotients, log 12), 11, and 5 on a log 5 table */
    reset(11);
    small_seqs(150);
    E(50, 12, 12, 12); K(0, 0, 0);
    E(50, 12, 12, 12); K(11, 11, 11);
    E(50, 5, 5, 5); K(5, 5, 5);
    emit("rice_k");

    /* every ll code (0..35), every ml symbol (0..36), every new-offset code up to bucket 16 (0..126) */
    reset(12);
    {
        uint32_t tg[31 + 12 * 8];
        int nt = 0, done = 0;
        for (uint32_t o = 1; o <= 31; o++) tg[nt++] = o;
        for (int bk = 5; bk <= 16; bk++) for (uint32_t j = 0; j < 8; j++) {
            const int mb = bk - 3 < 9 ? bk - 3 : 9;
            tg[nt++] = (1u << bk) + j + ((rnd() % (1u << mb)) << 3);
        }
        for (int i = 0; done < nt || i < 72; i++) {
            const int lc = (i * 7 + 35) % 36, mc = (i * 5) % 36;
            const uint32_t ll = (uint32_t)LL_base[lc] + (lc == 35 ? rnd() % 64 : rnd() % (1u << LM_extra[lc]));
            const uint32_t ml = (uint32_t)ML_base[mc] + (mc == 35 ? rnd() % 64 : rnd() % (1u << LM_extra[mc]));
            uint32_t off;
            if (done < nt && tg[done] <= produced + ll) off = tg[done++];
            else { off = 1 + rnd() % 31; if (off > produced + ll) off = (uint32_t)(produced + ll); }
            SR(ll, ml, off, NEWOFF);
        }
        SR(17, 0, 0, AUTO);
        const int q = nsq / 5;
        E(q, 6, 6, 8); E(q, 7, 7, 9); E(q, 8, 9, 10); E(q, 9, 10, 11); E(nsq - 4 * q, 10, 11, 12);
        emit("all_codes");
        int c[3] = { 0, 0, 0 };
        for (int t = 0; t < 3; t++) for (int s = 0; s < 194; s++) c[t] += g_cov.seen[t][s] != 0;
        if (c[0] != 36 || c[1] != 37 || c[2] != 127) die("all_codes covers %d ll, %d ml, %d of symbols", c[0], c[1], c[2]);
    }

    /* one literal, then a match of the whole rest at offset 1 (ml escape code, 24 extra bits); ends on the match */
    reset(13);
    bb_put(&L, 'Z');
    S(1, 99999, 1, AUTO);
    E(1, 5, 5, 5);
    emit("offset1_run");

    /* Huffman literals with ONE symbol, 0xFF (code "0", one bit per literal; the header is one run of 255 absent
     * symbols, gamma 255), and ll = 70000 (ll escape code with a large extra value) */
    reset(14);
    {
        static uint8_t len[256];
        memset(len, 0, 256); len[255] = 1;
        for (int i = 0; i < 70000; i++) bb_put(&L, 0xFF);
        S(70000, 5000, 70000, AUTO);
        SECT(5, L.n, len, 1, 0);
        E(1, 5, 5, 5);
        emit("huf_one_symbol");
    }
    /* Huffman code with lengths 1, 2, ... 15, 16, 16 (the longest allowed) and a lookup width k of 6 */
    reset(15);
    {
        static uint8_t len[256];
        memset(len, 0, 256);
        for (int i = 0; i < 17; i++) len['a' + i] = (uint8_t)(i < 16 ? i + 1 : 16);
        for (int i = 0; i < 600; i++) { unsigned r = rnd(), s = 0; while (s < 16 && (r >> s & 1)) s++; bb_put(&L, 'a' + s); }
        for (int i = 0; i < 17; i++) bb_put(&L, 'a' + i);
        S(400, 20, 100, AUTO); S(200, 9, 3, AUTO); S(17, 0, 0, AUTO);
        SECT(5, L.n, len, 6, 0);
        E(3, 5, 5, 5);
        emit("huf_deep");
    }
    /* Huffman code of all 256 symbols at length 8 (no runs, every delta 0) */
    reset(16);
    {
        static uint8_t len[256];
        memset(len, 8, 256);
        for (int i = 0; i < 512; i++) bb_put(&L, (unsigned)(i * 167 + 13));
        S(512, 64, 256, AUTO);
        SECT(5, L.n, len, 8, 0);
        E(1, 5, 5, 5);
        emit("huf_flat");
    }
    /* Huffman header whose runs of absent symbols are cut into pieces of at most 7 (the format allows any
     * split; the library writes the longest runs) */
    reset(17);
    lits_alpha(500, "  aaz");
    S(500, 30, 77, AUTO);
    SECT(5, L.n, NULL, 0, 7);
    E(1, 5, 5, 5);
    emit("huf_split_runs");
    /* 4-stream Huffman (mode 7) with 5 literals: streams of 2, 2, 1 and 0 literals (the library writes mode 7
     * from 4096 literals up) */
    reset(18);
    lits_alpha(5, "abc");
    S(5, 6, 2, AUTO);
    SECT(7, L.n, NULL, 0, 0);
    E(1, 5, 5, 5);
    emit("huf4_tiny");
    /* mode 7 with 1001 literals (streams of 251, 251, 251, 248) */
    reset(19);
    lits_alpha(1001, "etaoin shrdlu,.ETAOIN");
    S(1001, 100, 500, AUTO);
    SECT(7, L.n, NULL, 0, 0);
    E(1, 5, 5, 5);
    emit("huf4_small");
    /* literal epochs (mode 8) with sections of modes 0, 5, 7, 0, 5 (the library nests one mode per block) */
    reset(20);
    {
        static uint8_t one[256];
        memset(one, 0, 256); one[0] = 1;
        lits_rnd(100); lits_alpha(300, "0123456789 "); lits_alpha(2000, "acgtn\n"); lits_rnd(50);
        for (int i = 0; i < 64; i++) bb_put(&L, 0);
        S(1000, 40, 900, AUTO); S(1000, 300, 2, AUTO); S(514, 0, 0, AUTO);
        SECT(0, 100, NULL, 0, 0); SECT(5, 300, NULL, 0, 0); SECT(7, 2000, NULL, 0, 0); SECT(0, 50, NULL, 0, 0); SECT(5, 64, one, 1, 0);
        E(3, 5, 5, 5);
        emit("lit8_mixed");
    }
    /* literal epochs with a single section */
    reset(21);
    lits_alpha(200, "abcdefgh");
    S(200, 50, 150, AUTO);
    SECT(5, 200, NULL, 0, 0);
    force8 = 1;
    E(1, 5, 5, 5);
    emit("lit8_one");

    /* literal epochs with the most sections, 256, of 1 to 4 literals each, raw and Huffman in turn */
    reset(23);
    for (int i = 0; i < 256; i++) {
        const size_t c = 1 + (size_t)(i & 3);
        if (i & 1) lits_alpha(c, "xyz"); else lits_rnd(c);
        SECT(i & 1 ? 5 : 0, c, NULL, 0, 0);
    }
    S((uint32_t)L.n, 10, 3, AUTO);
    E(1, 5, 5, 5);
    emit("lit8_256");
    /* literal epochs with empty sections (raw, count 0) at the start, in the middle and at the end */
    reset(24);
    lits_alpha(120, "abcdefgh");
    S(120, 10, 3, AUTO);
    SECT(0, 0, NULL, 0, 0); SECT(5, 60, NULL, 0, 0); SECT(0, 0, NULL, 0, 0); SECT(0, 60, NULL, 0, 0); SECT(0, 0, NULL, 0, 0);
    E(1, 5, 5, 5);
    emit("lit8_empty_sec");
    /* Huffman header with large length jumps: lengths 16, 1, 16, 2, 15, 3, ... in symbol order (Rice quotients up
     * to 14), lookup width k = 12 */
    reset(25);
    {
        static uint8_t len[256];
        static const uint8_t order[17] = { 16, 1, 16, 2, 15, 3, 14, 4, 13, 5, 12, 6, 11, 7, 10, 8, 9 };
        memset(len, 0, 256);
        for (int i = 0; i < 17; i++) len[40 + 11 * i] = order[i];
        for (int i = 0; i < 17; i++) bb_put(&L, 40 + 11 * i);
        for (int i = 0; i < 400; i++) { unsigned r = rnd(), n1 = 0; while (n1 < 15 && (r >> n1 & 1)) n1++; for (int j = 0; j < 17; j++) if (order[j] == n1 + 1) { bb_put(&L, 40 + 11 * j); break; } }
        S((uint32_t)L.n, 12, 17, AUTO);
        SECT(5, L.n, len, 12, 0);
        E(1, 5, 5, 5);
        emit("huf_jumps");
    }

    /* 1024 epochs of one sequence each (the most a block may have; epochs = sc), two of three repeating the
     * tables of the epoch before */
    reset(22);
    for (int i = 0; i < 1024; i++) { const int g = i / 3; SR(1 + g % 5, 3 + g % 7, 1, NEWOFF); }
    for (int i = 0; i < 1024; i++) { if (i % 3 == 0) E(1, 5 + i / 3 % 8, 5 + i / 5 % 8, 5 + i / 7 % 8); else E(1, 0, 0, 0); }
    emit("1024_epochs");

    /* plane block, stride 4, made by hand: lane 0 = a seq block that is a STORED block; lane 1 = raw with the
     * delta bit; lane 2 = a format 2 block with the delta bit; lane 3 = a Huffman lane (format 1 header) with
     * the delta bit */
    {
        enum { N = 1500 };
        static uint8_t lane[4][N];
        bb_t pb = { 0, 0, 0 }, po = { 0, 0, 0 }, lb = { 0, 0, 0 }, lo = { 0, 0, 0 };
        bb_put(&pb, 0x08 | 2); bb_le32(&pb, 4 * N);
        g_rs = 99;
        for (int i = 0; i < N; i++) lane[0][i] = (uint8_t)(rnd() >> 8);
        bb_put(&pb, 1); bb_le32(&pb, 5 + N); bb_put(&pb, 0x01); bb_le32(&pb, N); bb_bytes(&pb, lane[0], N);
        {   /* raw + delta */
            uint8_t d[N], acc = 0;
            for (int i = 0; i < N; i++) { d[i] = (uint8_t)(rnd() % 5 - 2); acc = (uint8_t)(acc + d[i]); lane[1][i] = acc; }
            bb_put(&pb, 0x80); bb_bytes(&pb, d, N);
        }
        {   /* seq block + delta */
            uint8_t acc = 0;
            block_of(N, &lb, &lo);
            for (int i = 0; i < N; i++) { acc = (uint8_t)(acc + lo.d[i]); lane[2][i] = acc; }
            bb_put(&pb, 0x81); bb_le32(&pb, (uint32_t)lb.n); bb_bytes(&pb, lb.d, lb.n);
        }
        {   /* Huffman + delta: [k][256 lengths][total_bits][stream] */
            static const uint8_t alphabet[4] = { 0, 1, 255, 3 };
            uint8_t d[N], len[256], acc = 0;
            uint16_t code[256];
            bb_t hs = { 0, 0, 0 };
            g_rs = 98;
            for (int i = 0; i < N; i++) { const unsigned r = rnd() % 8; d[i] = alphabet[r < 4 ? 0 : r < 6 ? 1 : r < 7 ? 2 : 3]; acc = (uint8_t)(acc + d[i]); lane[3][i] = acc; }
            huf_build_lengths(d, N, len);
            huf_codes(len, code);
            bb_put(&hs, 3); bb_bytes(&hs, len, 256);
            huf_stream(&hs, d, N, len, code);
            bb_put(&pb, 0x82); bb_le32(&pb, (uint32_t)hs.n); bb_bytes(&pb, hs.d, hs.n);
            bb_free(&hs);
        }
        for (int i = 0; i < N; i++) for (int j = 0; j < 4; j++) bb_put(&po, lane[j][i]);
        emit_raw("plane_mixed", &pb, &po, 1);
        /* x86 block around that plane block (the encoder's inner blocks were always normal ones). One range of
         * 5 bytes without an E8 or E9 byte, so the filter changes nothing: this tests the wrapper, not the filter */
        {
            bb_t xb = { 0, 0, 0 };
            uint32_t at = 0;
            for (;; at++) {
                int hit = 0;
                for (int k = 0; k < 5; k++) hit |= (po.d[at + k] & 0xFE) == 0xE8;
                if (!hit) break;
            }
            bb_put(&xb, 0x10); bb_le32(&xb, 4 * N); bb_put(&xb, 1); bb_le32(&xb, at); bb_le32(&xb, 5);
            bb_bytes(&xb, pb.d, pb.n);
            emit_raw("x86_plane", &xb, &po, 1);
            bb_free(&xb);
        }
        bb_free(&pb); bb_free(&po); bb_free(&lb); bb_free(&lo);
    }

    /* plane blocks of stride 2 with each of the three flag values that mean stride 2 (bits 0-1 = 0, 1, 3; only 2
     * means stride 4). Lane 0 is a seq block that is a PRNG block (seed 7, shift 8), lane 1 is raw */
    for (unsigned sbits = 0; sbits < 4; sbits++) {
        if (sbits == 2) continue;
        char nm[32];
        enum { N2 = 800 };
        static uint8_t l0[N2], l1[N2];
        blk.n = 0; orig.n = 0;
        srandom(7);
        for (int i = 0; i < N2; i++) l0[i] = (uint8_t)(random() >> 8);
        g_rs = 50 + sbits;
        for (int i = 0; i < N2; i++) l1[i] = (uint8_t)(rnd() >> 8);
        bb_put(&blk, 0x08 | sbits); bb_le32(&blk, 2 * N2);
        bb_put(&blk, 1); bb_le32(&blk, 7); bb_put(&blk, 0x05); bb_le32(&blk, N2); bb_put(&blk, 7); bb_put(&blk, 0);
        bb_put(&blk, 0); bb_bytes(&blk, l1, N2);
        for (int i = 0; i < N2; i++) { bb_put(&orig, l0[i]); bb_put(&orig, l1[i]); }
        snprintf(nm, sizeof nm, "plane2_flags%u", sbits);
        emit_raw(nm, &blk, &orig, 1);
    }

    /* x86 block around a STORED block, two ranges that touch (start of the second = end of the first). The data
     * is text without E8/E9 bytes, so the filter changes nothing: this tests the wrapper, not the filter */
    reset(40);
    lits_alpha(66000, "abcdefghijklmnopqrstuvwxyz     \n.,");
    blk.n = 0; orig.n = 0;
    bb_bytes(&orig, L.d, L.n);
    bb_put(&blk, 0x10); bb_le32(&blk, 66000); bb_put(&blk, 2);
    bb_le32(&blk, 0); bb_le32(&blk, 30000); bb_le32(&blk, 30000); bb_le32(&blk, 36000);
    bb_put(&blk, 0x01); bb_le32(&blk, 66000); bb_bytes(&blk, L.d, L.n);
    emit_raw("x86_stored", &blk, &orig, 1);

    /* PRNG blocks the encoder does not write: shift 24 (flags 07) and seed 0. The original comes from the C
     * library's srandom()/random(), which must be glibc's for these two */
    for (int v = 0; v < 2; v++) {
        const unsigned seed = v ? 0 : 4242, shift = v ? 0 : 24;
        blk.n = 0; orig.n = 0;
        bb_put(&blk, 0x04 | shift / 8); bb_le32(&blk, 3000); bb_put(&blk, seed & 255); bb_put(&blk, seed >> 8);
        srandom(seed);
        for (int i = 0; i < 3000; i++) bb_put(&orig, (unsigned)(random() >> shift));
        emit_raw(v ? "prng_seed0" : "prng_shift24", &blk, &orig, 1);
    }

    /* offsets above 2^24, the largest offset, the longest match: see big_case. The original is not written */
    big_case(1, NULL);
    bb_free(&blk); bb_free(&orig);
}

/* ---- text descriptions ----
 *   # comment
 *   lits hex 48656c6c6f          literal bytes (appended)
 *   lits str some text           the rest of the line
 *   lits rnd SEED N              N pseudo-random bytes
 *   lit MODE [N]                 a literal section of mode 0, 5 or 7 over the next N literals (default: the rest);
 *                                several "lit" lines make literal epochs (mode 8); "lit8" forces mode 8 for one
 *   seq LL ML OFFSET [new|rep0|rep1|rep2]   ML 0 = literals only (last sequence)
 *   epoch N LGLL LGML LGOF       N sequences; a log of 0 = the same table as the previous epoch
 * Without "lit": one raw section. Without "epoch": one epoch with tables of log 9. */
static void parse_desc(const char* path)
{
    FILE* f = fopen(path, "r");
    char line[65536];
    size_t sec_at = 0;
    if (!f) die("cannot read %s", path);
    reset(1);
    while (fgets(line, sizeof line, f)) {
        char w[16];
        unsigned a, b2, c, d4;
        unsigned long long seed;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        if (!strncmp(line, "lits hex ", 9)) {
            for (const char* p = line + 9; p[0] && p[1]; p += 2) { unsigned v; if (sscanf(p, "%2x", &v) != 1) die("bad hex"); bb_put(&L, v); }
        } else if (!strncmp(line, "lits str ", 9)) bb_bytes(&L, (const uint8_t*)line + 9, strlen(line + 9));
        else if (sscanf(line, "lits rnd %llu %u", &seed, &a) == 2) { g_rs = seed; lits_rnd(a); }
        else if (!strcmp(line, "lit8")) force8 = 1;
        else if (sscanf(line, "lit %u %u", &a, &b2) == 2) { SECT((int)a, b2, NULL, 0, 0); sec_at += b2; }
        else if (sscanf(line, "lit %u", &a) == 1) { SECT((int)a, (size_t)-1, NULL, 0, 0); }
        else if (sscanf(line, "seq %u %u %u %15s", &a, &b2, &c, w) == 4)
            S(a, b2, c, !strcmp(w, "new") ? NEWOFF : !strcmp(w, "rep0") ? REP0 : !strcmp(w, "rep1") ? REP1 : !strcmp(w, "rep2") ? REP2 : AUTO);
        else if (sscanf(line, "seq %u %u %u", &a, &b2, &c) == 3) S(a, b2, c, AUTO);
        else if (sscanf(line, "epoch %u %u %u %u", &a, &b2, &c, &d4) == 4) E((int)a, (int)b2, (int)c, (int)d4);
        else die("%s: cannot read the line \"%s\"", path, line);
    }
    fclose(f);
    for (int i = 0; i < nsec; i++) if (SEC[i].n == (size_t)-1) { if (i != nsec - 1) die("only the last \"lit\" may leave its count out"); SEC[i].n = L.n - sec_at; }
}

int main(int argc, char** argv)
{
    if (!L.d) { L.d = (uint8_t*)malloc(64); L.cap = 64; }
    if (argc == 5 && !strcmp(argv[1], "-f")) {
        bb_t blk = { 0, 0, 0 }, orig = { 0, 0, 0 };
        parse_desc(argv[2]);
        build(&blk, &orig);
        check(argv[2], &blk, &orig);
        put_file(argv[3], blk.d, blk.n);
        put_file(argv[4], orig.d, orig.n);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "-big")) { big_case(0, argv[2]); return 0; }
    if (argc != 2) { fprintf(stderr, "usage: mkvec OUTDIR | mkvec -f DESC OUT.seq OUT.orig | mkvec -big OUT.orig\n"); return 2; }
    g_outdir = argv[1];
    cases();
    return 0;
}
