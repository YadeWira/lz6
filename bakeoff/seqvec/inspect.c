/* SPDX-License-Identifier: MIT */
/*
 * inspect.c - walks one lz6 seq block and prints what it is made of.
 *
 * Written from lz6_Seq_format.md alone: it includes nothing from lib/ and shares no code with the
 * library's decoder. It parses every header that is needed to find the end of each part (vlq and
 * LEB128 counts, the byte and the bit-packed table headers, the Huffman headers, the rANS tables)
 * and checks the limits the specification gives for them. It does not decode symbols, so it says
 * nothing about the entropy-coded payloads themselves (literal streams, the sequence bitstream).
 *
 *   inspect FILE        the tree, one line per part; the last line is "OK: <summary>"
 *   inspect -s FILE     only the one-line summary
 * Exit status 0 when the block could be walked to its end, 1 otherwise ("ERROR: ..." names the byte).
 *
 * Summary grammar (one line):
 *   stored isize=N | prng isize=N shift=S seed=X
 *   x86 isize=N nr=R { <block> }
 *   plane isize=N stride=S { lane ... } with a lane as raw | huf(k,nsym,maxlen) | blk{ <block> }, "+delta" appended
 *   fmt1|fmt2 isize=N lit=<lit> sc=N <tables>
 *     <lit>    m<mode>:<count>, for mode 8 followed by [n=<sections>: m<mode>x<how many> ...]
 *     <tables> fmt1: ll=<log>/<maxSym> ml=.. of=..
 *              fmt2: ep=<epochs> ll{logs seen} ml{..} of{..} rep=<tables written as "same as before">
 *   "+trail=N" when N bytes follow the block.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

static int g_verbose = 1;
static const uint8_t* g_base;
static char g_err[256];
static char* g_sum;
static size_t g_sumlen, g_sumcap;

static void sum(const char* fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (g_sumlen + (size_t)n + 1 > g_sumcap) {
        g_sumcap = (g_sumcap + (size_t)n + 1) * 2;
        g_sum = (char*)realloc(g_sum, g_sumcap);
        if (!g_sum) { fprintf(stderr, "out of memory\n"); exit(2); }
    }
    memcpy(g_sum + g_sumlen, tmp, (size_t)n + 1);
    g_sumlen += (size_t)n;
}
static void say(int depth, const char* fmt, ...)
{
    if (!g_verbose) return;
    va_list ap;
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}
static int fail(const uint8_t* at, const char* fmt, ...)
{
    if (g_err[0]) return 0;   /* keep the first, innermost message */
    char tmp[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    snprintf(g_err, sizeof g_err, "at byte %ld: %s", (long)(at - g_base), tmp);
    return 0;
}

static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* 255-continuation count: stops quietly at the end of the input and at 2^30 */
static uint32_t rd_vlq(const uint8_t** pp, const uint8_t* end)
{
    uint32_t v = 0;
    while (*pp < end) {
        const unsigned b = *(*pp)++;
        v += b;
        if (b != 255 || v >= (1u << 30)) break;
    }
    return v;
}
/* LEB128: at most 5 bytes, below 2^30; -1 otherwise */
static long rd_leb(const uint8_t** pp, const uint8_t* end)
{
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        if (*pp >= end) return -1;
        const unsigned b = *(*pp)++;
        v |= (uint32_t)(b & 127) << (7 * i);   /* of a fifth byte only bits 0-3 survive the 32-bit shift */
        if (!(b & 128)) return v < (1u << 30) ? (long)v : -1;
    }
    return -1;
}
static long rd_count(const uint8_t** pp, const uint8_t* end, int v2) { return v2 ? rd_leb(pp, end) : (long)rd_vlq(pp, end); }

/* LSB-first bit reader of the bit-packed headers */
typedef struct { const uint8_t* p; const uint8_t* end; unsigned bit; int err; } hb_t;
static unsigned hb_bit(hb_t* r)
{
    if (r->p >= r->end) { r->err = 1; return 0; }
    const unsigned b = (*r->p >> r->bit) & 1u;
    if (++r->bit == 8) { r->bit = 0; r->p++; }
    return b;
}
static unsigned hb_bits(hb_t* r, unsigned nb) { unsigned v = 0; for (unsigned i = 0; i < nb; i++) v |= hb_bit(r) << i; return v; }
static const uint8_t* hb_end(const hb_t* r) { return r->bit ? r->p + 1 : r->p; }
static int hb_gamma(hb_t* r, unsigned* g)
{
    unsigned nb = 0;
    while (!hb_bit(r)) { if (r->err || ++nb > 8) return 0; }
    unsigned v = 1;
    for (unsigned i = 0; i < nb; i++) v = (v << 1) | hb_bit(r);
    *g = v;
    return !r->err;
}

typedef struct { int log, maxSym, k, present, maxrun; } tinfo_t;

/* format 1 table: [log:1][maxSym:1] then counts as 1- or 2-byte varints, a zero count followed by a run byte */
static const uint8_t* tans1(const uint8_t* p, const uint8_t* end, int alphaMax, tinfo_t* ti)
{
    const uint8_t* const p0 = p;
    if (end - p < 2) { fail(p, "table header cut"); return NULL; }
    const int lg = p[0], ms = p[1];
    p += 2;
    if (lg < 5 || lg > 12) { fail(p0, "table log %d outside 5..12", lg); return NULL; }
    if (ms > alphaMax) { fail(p0, "maxSym %d above %d", ms, alphaMax); return NULL; }
    unsigned sum_ = 0;
    ti->present = 0; ti->maxrun = 0;
    for (int s = 0; s <= ms; s++) {
        if (p >= end) { fail(p, "table counts cut"); return NULL; }
        unsigned v = *p++;
        if (v == 0) {
            if (p >= end) { fail(p, "table counts cut"); return NULL; }
            const int run = *p++;
            if (s + run > ms) { fail(p, "zero run passes maxSym"); return NULL; }
            if (run + 1 > ti->maxrun) ti->maxrun = run + 1;
            s += run;
            continue;
        }
        if (v & 128) {
            if (p >= end) { fail(p, "table counts cut"); return NULL; }
            v = (v & 127) | (unsigned)*p++ << 7;
        }
        sum_ += v; ti->present++;
        if (sum_ > (1u << lg)) { fail(p, "counts above 2^log"); return NULL; }
    }
    if (sum_ != (1u << lg)) { fail(p0, "counts sum to %u, not 2^%d", sum_, lg); return NULL; }
    ti->log = lg; ti->maxSym = ms; ti->k = -1;
    return p;
}
/* format 2 table: [log:4][k:4][maxSym:8], counts of 0..maxSym-1 as Rice(k), each zero followed by gamma(r+1) */
static const uint8_t* tans2(const uint8_t* p, const uint8_t* end, int alphaMax, tinfo_t* ti)
{
    hb_t r = { p, end, 0, 0 };
    const int lg = (int)hb_bits(&r, 4);
    const unsigned k = hb_bits(&r, 4);
    const int ms = (int)hb_bits(&r, 8);
    if (r.err) { fail(p, "table header cut"); return NULL; }
    if (lg < 5 || lg > 12) { fail(p, "table log %d outside 5..12", lg); return NULL; }
    if (k > 11) { fail(p, "Rice parameter %u above 11", k); return NULL; }
    if (ms > alphaMax) { fail(p, "maxSym %d above %d", ms, alphaMax); return NULL; }
    const unsigned M = 1u << lg;
    unsigned sum_ = 0;
    ti->present = 0; ti->maxrun = 0;
    for (int s = 0; s < ms; s++) {
        unsigned q = 0;
        while (hb_bit(&r)) { if (++q > M) { fail(r.p, "Rice quotient above 2^log"); return NULL; } }
        const unsigned v = (q << k) | hb_bits(&r, k);
        if (r.err) { fail(r.p, "table counts cut"); return NULL; }
        if (v > M - sum_) { fail(r.p, "counts above 2^log"); return NULL; }
        sum_ += v;
        if (v) { ti->present++; continue; }
        unsigned g;
        if (!hb_gamma(&r, &g)) { fail(r.p, "bad gamma code in a table"); return NULL; }
        if (s + (int)(g - 1) >= ms) { fail(r.p, "zero run reaches maxSym"); return NULL; }
        if ((int)g > ti->maxrun) ti->maxrun = (int)g;
        s += (int)(g - 1);
    }
    if (sum_ >= M) { fail(p, "no count left for maxSym"); return NULL; }
    ti->present++;
    ti->log = lg; ti->maxSym = ms; ti->k = (int)k;
    return hb_end(&r);
}

typedef struct { int k, nsym, maxlen, kraft; } hinfo_t;   /* kraft: 0 complete, 1 the one-symbol code, 2 anything else */
static void huf_info(const uint8_t* len, hinfo_t* hi)
{
    uint32_t kraft = 0;
    hi->nsym = 0; hi->maxlen = 0;
    for (int s = 0; s < 256; s++) if (len[s]) {
        hi->nsym++;
        if (len[s] > hi->maxlen) hi->maxlen = len[s];
        kraft += 1u << (16 - len[s]);
    }
    hi->kraft = kraft == 65536 ? 0 : (hi->nsym == 1 && hi->maxlen == 1) ? 1 : 2;
}
/* format 1 Huffman header: [k:1][256 code lengths] */
static const uint8_t* huf1(const uint8_t* p, const uint8_t* end, hinfo_t* hi)
{
    if (end - p < 257) { fail(p, "Huffman header cut"); return NULL; }
    if (p[0] < 1 || p[0] > 12) { fail(p, "Huffman k %d outside 1..12", p[0]); return NULL; }
    for (int s = 0; s < 256; s++) if (p[1 + s] > 16) { fail(p + 1 + s, "code length above 16"); return NULL; }
    hi->k = p[0];
    huf_info(p + 1, hi);
    return p + 257;
}
/* format 2 Huffman header: [k:4], then per symbol 0 + gamma(run) or 1 + Rice1(zigzag(length - previous)) */
static const uint8_t* huf2(const uint8_t* p, const uint8_t* end, hinfo_t* hi)
{
    hb_t r = { p, end, 0, 0 };
    uint8_t len[256];
    const unsigned k = hb_bits(&r, 4);
    int prev = 8;
    memset(len, 0, sizeof len);
    for (int s = 0; s < 256; ) {
        if (!hb_bit(&r)) {
            unsigned g;
            if (!hb_gamma(&r, &g)) { fail(r.p, "bad gamma code in a Huffman header"); return NULL; }
            if (s + (int)g > 256) { fail(r.p, "run passes symbol 255"); return NULL; }
            s += (int)g;
            continue;
        }
        unsigned q = 0;
        while (hb_bit(&r)) { if (++q > 40) { fail(r.p, "Rice quotient above 40"); return NULL; } }
        const unsigned z = (q << 1) | hb_bit(&r);
        if (r.err) { fail(r.p, "Huffman header cut"); return NULL; }
        prev += (z & 1) ? -(int)((z + 1) >> 1) : (int)(z >> 1);
        if (prev < 1 || prev > 16) { fail(r.p, "code length %d outside 1..16", prev); return NULL; }
        len[s++] = (uint8_t)prev;
    }
    if (r.err) { fail(r.p, "Huffman header cut"); return NULL; }
    if (k < 1 || k > 12) { fail(p, "Huffman k %u outside 1..12", k); return NULL; }
    hi->k = (int)k;
    huf_info(len, hi);
    return hb_end(&r);
}
static const char* kraft_name(int k) { return k == 0 ? "complete" : k == 1 ? "one symbol" : "NOT complete"; }

/* rANS order-0 table: [L_bits:1][maxSym:1][3 bytes per symbol 0..maxSym] */
static const uint8_t* rans0_table(const uint8_t* p, const uint8_t* end, int* lbits, int* maxsym)
{
    if (end - p < 2) { fail(p, "rANS table cut"); return NULL; }
    const int L = p[0], ms = p[1];
    if (L < 1 || L > 16) { fail(p, "rANS L_bits %d outside 1..16", L); return NULL; }
    if ((size_t)(end - p) < 2 + 3 * ((size_t)ms + 1)) { fail(p, "rANS table cut"); return NULL; }
    uint64_t s_ = 0;
    for (int s = 0; s <= ms; s++) s_ += (uint32_t)p[2 + 3 * s] | (uint32_t)p[3 + 3 * s] << 8 | (uint32_t)p[4 + 3 * s] << 16;
    if (s_ != ((uint64_t)1 << L)) { fail(p, "rANS frequencies sum to %lu, not 2^%d", (unsigned long)s_, L); return NULL; }
    *lbits = L; *maxsym = ms;
    return p + 2 + 3 * ((size_t)ms + 1);
}
/* rANS context table of modes 6 and 3: [L_bits:1][256 x freq:2] */
static const uint8_t* ctx_table(const uint8_t* p, const uint8_t* end, int* lbits)
{
    if (end - p < 513) { fail(p, "context table cut"); return NULL; }
    const int L = p[0];
    if (L < 1 || L > 16) { fail(p, "context table L_bits %d outside 1..16", L); return NULL; }
    uint32_t s_ = 0;
    for (int s = 0; s < 256; s++) s_ += (uint32_t)p[1 + 2 * s] | (uint32_t)p[2 + 2 * s] << 8;
    if (s_ != (1u << L)) { fail(p, "context table sums to %u, not 2^%d", s_, L); return NULL; }
    *lbits = L;
    return p + 513;
}
/* [slen:4][x:4 BE][slen-4 bytes] inside [p, end) */
static const uint8_t* rans_stream(const uint8_t* p, const uint8_t* end, uint32_t* slen)
{
    if (end - p < 8) { fail(p, "rANS stream cut"); return NULL; }
    *slen = le32(p);
    if (*slen < 4 || (size_t)(end - p - 4) < *slen) { fail(p, "rANS slen %u does not fit", *slen); return NULL; }
    return p + 4 + *slen;
}

/* One literal section. `cap` bounds its count (isize, or what is left of the outer count inside mode 8).
 * Returns the byte behind it, and its mode and count. */
static const uint8_t* lit_section(const uint8_t* p, const uint8_t* end, long cap, int v2, int nested, int depth,
                                  int* mode_out, long* count_out)
{
    const uint8_t* const p0 = p;
    if (p >= end) { fail(p, "literal section cut"); return NULL; }
    const int mode = *p++;
    const long count = rd_count(&p, end, v2);
    if (count < 0) { fail(p0, "bad literal count"); return NULL; }
    if (count > cap) { fail(p0, "lit_count %ld above %ld", count, cap); return NULL; }
    *mode_out = mode; *count_out = count;
    if (mode == 0) {
        if (end - p < count) { fail(p, "raw literals cut"); return NULL; }
        say(depth, "literals: mode 0 (raw), count %ld", count);
        return p + count;
    }
    if (mode == 8 && v2 && !nested) {
        const long size = rd_leb(&p, end);
        if (size < 0 || end - p < size) { fail(p, "literal epochs: bad size"); return NULL; }
        const uint8_t* const se = p + size;
        const long n = rd_leb(&p, se);
        if (n < 1 || n > 256) { fail(p, "literal epochs: %ld sections (1..256)", n); return NULL; }
        say(depth, "literals: mode 8 (epochs), count %ld, %ld sections, %ld bytes", count, n, size);
        long left = count;
        int per_mode[9] = { 0 };
        for (long i = 0; i < n; i++) {
            int m; long c;
            p = lit_section(p, se, left, 1, 1, depth + 1, &m, &c);
            if (!p) return NULL;
            left -= c;
            per_mode[m]++;
        }
        if (left != 0) { fail(p, "literal epochs: section counts fall %ld short of lit_count", left); return NULL; }
        if (p != se) { fail(p, "literal epochs: sections do not end at size"); return NULL; }
        sum("m8:%ld[n=%ld:", count, n);
        for (int m = 0; m < 9; m++) if (per_mode[m]) sum(" m%dx%d", m, per_mode[m]);
        sum("]");
        return p;
    }
    if (mode != 1 && mode != 3 && mode != 5 && mode != 6 && mode != 7) { fail(p0, "literal mode %d", mode); return NULL; }
    const long csize = rd_count(&p, end, v2);
    if (csize < 0 || end - p < csize) { fail(p, "literal csize does not fit"); return NULL; }
    const uint8_t* const ce = p + csize;
    if (mode == 1) {
        int L, ms; uint32_t slen;
        const uint8_t* q = rans0_table(p, ce, &L, &ms);
        if (!q || !(q = rans_stream(q, ce, &slen))) return NULL;
        say(depth, "literals: mode 1 (rANS order 0), count %ld, csize %ld, L_bits %d, maxSym %d, slen %u%s",
            count, csize, L, ms, slen, q != ce ? " (csize has spare bytes)" : "");
    } else if (mode == 5 || mode == 7) {
        hinfo_t hi;
        const uint8_t* q = v2 ? huf2(p, ce, &hi) : huf1(p, ce, &hi);
        if (!q) return NULL;
        if (mode == 5) {
            if (!v2 && csize < 261) { fail(p, "mode 5 csize below 261"); return NULL; }
            if (ce - q < 4) { fail(q, "Huffman total_bits cut"); return NULL; }
            const uint32_t total = le32(q);
            const size_t nb = ((size_t)total + 7) / 8;
            if ((size_t)(ce - q - 4) < nb) { fail(q, "Huffman stream does not fit in csize"); return NULL; }
            say(depth, "literals: mode 5 (Huffman), count %ld, csize %ld, header %ld bytes, k %d, %d symbols, longest code %d (%s), total_bits %u%s",
                count, csize, (long)(q - p), hi.k, hi.nsym, hi.maxlen, kraft_name(hi.kraft), total,
                (size_t)(ce - q - 4) != nb ? " (csize has spare bytes)" : "");
        } else {
            uint32_t tot[4];
            for (int j = 0; j < 4; j++) {
                if (ce - q < 4) { fail(q, "mode 7 stream %d cut", j); return NULL; }
                tot[j] = le32(q);
                const size_t nb = ((size_t)tot[j] + 7) / 8;
                if ((size_t)(ce - q - 4) < nb) { fail(q, "mode 7 stream %d does not fit", j); return NULL; }
                q += 4 + nb;
            }
            if (q != ce) { fail(q, "mode 7 streams do not end at csize"); return NULL; }
            say(depth, "literals: mode 7 (Huffman, 4 streams), count %ld, csize %ld, k %d, %d symbols, longest code %d (%s), total_bits %u %u %u %u",
                count, csize, hi.k, hi.nsym, hi.maxlen, kraft_name(hi.kraft), tot[0], tot[1], tot[2], tot[3]);
        }
    } else if (mode == 6) {
        const uint8_t* q = p;
        int L = 0; uint32_t slen;
        if (ce - q < 1) { fail(q, "mode 6 cut"); return NULL; }
        q++;   /* the leading L_bits byte is not used */
        for (int c = 0; c < 16; c++) if (!(q = ctx_table(q, ce, &L))) return NULL;
        if (!(q = rans_stream(q, ce, &slen))) return NULL;
        say(depth, "literals: mode 6 (rANS order 1, 16 contexts), count %ld, csize %ld, L_bits %d, slen %u", count, csize, L, slen);
    } else {
        const uint8_t* q = p;
        int L = 0, active = 0; uint32_t slen;
        if (ce - q < 33) { fail(q, "mode 3 cut"); return NULL; }
        for (int c = 0; c < 256; c++) active += (q[c >> 3] >> (c & 7)) & 1;
        q += 33;   /* bitmap, then the unused L_bits byte */
        for (int c = 0; c < active + 1; c++) if (!(q = ctx_table(q, ce, &L))) return NULL;
        if (!(q = rans_stream(q, ce, &slen))) return NULL;
        say(depth, "literals: mode 3 (rANS order 1, 256 contexts), count %ld, csize %ld, %d active contexts, L_bits %d, slen %u",
            count, csize, active, L, slen);
    }
    if (!nested) sum("m%d:%ld", mode, count);
    return ce;
}

static const int alpha_max[3] = { 35, 36, 193 };
static const char* const tname[3] = { "ll", "ml", "of" };

static const uint8_t* walk(const uint8_t* b, const uint8_t* end, int depth, int ctx);

/* a normal, stored or PRNG block (what a plane lane of mode 1 may hold) */
static const uint8_t* walk_plain(const uint8_t* b, const uint8_t* end, int depth)
{
    const int flags = b[0];
    const uint32_t isize = le32(b + 1);
    const uint8_t* p = b + 5;
    if (isize > 0x7FFFFFFFu) { fail(b + 1, "isize above 2^31 - 1"); return NULL; }
    if (flags & 0x04) {
        if (end - p < 2) { fail(p, "PRNG seed cut"); return NULL; }
        say(depth, "PRNG block (flags %02x): isize %u, shift %d bits, seed %u", flags, isize, 8 * (flags & 3), p[0] | p[1] << 8);
        sum("prng isize=%u shift=%d seed=%u", isize, 8 * (flags & 3), p[0] | p[1] << 8);
        return p + 2;
    }
    if (flags & 0x01) {
        if ((size_t)(end - p) < isize) { fail(p, "stored data cut"); return NULL; }
        say(depth, "stored block (flags %02x): isize %u", flags, isize);
        sum("stored isize=%u", isize);
        return p + isize;
    }
    const int v2 = (flags & 0x20) != 0;
    say(depth, "normal block, format %d (flags %02x): isize %u", v2 ? 2 : 1, flags, isize);
    sum("fmt%d isize=%u lit=", v2 ? 2 : 1, isize);
    int mode; long count;
    if (!(p = lit_section(p, end, (long)isize, v2, 0, depth + 1, &mode, &count))) return NULL;
    if (mode == 0) sum("m0:%ld", count);
    if (end - p < 4) { fail(p, "sc cut"); return NULL; }
    const uint32_t sc = le32(p);
    p += 4;
    if (sc > (1u << 24)) { fail(p - 4, "sc %u above 2^24", sc); return NULL; }
    sum(" sc=%u", sc);
    if (sc == 0) {
        if (isize != 0) { fail(p - 4, "sc = 0 with isize %u", isize); return NULL; }
        say(depth + 1, "sequences: sc 0 (the block ends here)");
        return p;
    }
    if (!v2) {
        tinfo_t ti[3];
        for (int t = 0; t < 3; t++) if (!(p = tans1(p, end, alpha_max[t], &ti[t]))) return NULL;
        say(depth + 1, "sequences: sc %u; tables ll log %d maxSym %d, ml log %d maxSym %d, of log %d maxSym %d", sc,
            ti[0].log, ti[0].maxSym, ti[1].log, ti[1].maxSym, ti[2].log, ti[2].maxSym);
        sum(" ll=%d/%d ml=%d/%d of=%d/%d", ti[0].log, ti[0].maxSym, ti[1].log, ti[1].maxSym, ti[2].log, ti[2].maxSym);
    } else {
        const long ne = rd_leb(&p, end);
        if (ne < 1 || ne > 1024 || (uint32_t)ne > sc) { fail(p, "epoch count %ld (1..1024, at most sc)", ne); return NULL; }
        say(depth + 1, "sequences: sc %u, %ld epoch%s", sc, ne, ne == 1 ? "" : "s");
        uint32_t left = sc;
        unsigned seen[3] = { 0, 0, 0 };
        int reps = 0;
        tinfo_t prev[3];
        for (long e = 0; e < ne; e++) {
            const long n = rd_leb(&p, end);
            if (n < 1 || (uint32_t)n > left) { fail(p, "epoch %ld: bad sequence count %ld", e, n); return NULL; }
            left -= (uint32_t)n;
            char line[256];
            int ln = snprintf(line, sizeof line, "epoch %ld: %ld seqs;", e, n);
            for (int t = 0; t < 3; t++) {
                if (p >= end) { fail(p, "epoch %ld: tables cut", e); return NULL; }
                if (*p == 0) {
                    if (e == 0) { fail(p, "first epoch repeats a table"); return NULL; }
                    p++; reps++;
                    ln += snprintf(line + ln, sizeof line - (size_t)ln, " %s repeat (log %d maxSym %d)%s", tname[t], prev[t].log, prev[t].maxSym, t < 2 ? "," : "");
                    continue;
                }
                if (!(p = tans2(p, end, alpha_max[t], &prev[t]))) return NULL;
                seen[t] |= 1u << prev[t].log;
                ln += snprintf(line + ln, sizeof line - (size_t)ln, " %s log %d maxSym %d (k %d, %d present, longest zero run %d)%s",
                               tname[t], prev[t].log, prev[t].maxSym, prev[t].k, prev[t].present, prev[t].maxrun, t < 2 ? "," : "");
            }
            say(depth + 2, "%s", line);
        }
        if (left) { fail(p, "epoch counts fall %u short of sc", left); return NULL; }
        sum(" ep=%ld", ne);
        for (int t = 0; t < 3; t++) {
            sum(" %s{", tname[t]);
            int first = 1;
            for (int l = 5; l <= 12; l++) if (seen[t] >> l & 1) { sum(first ? "%d" : ",%d", l); first = 0; }
            sum("}");
        }
        sum(" rep=%d", reps);
    }
    if (end - p < 4) { fail(p, "bits len cut"); return NULL; }
    const uint32_t blen = le32(p);
    p += 4;
    if (blen < 9 || (size_t)(end - p) < blen) { fail(p - 4, "bits len %u does not fit (at least 9)", blen); return NULL; }
    if (p[blen - 1] == 0) { fail(p + blen - 1, "last bitstream byte is 0 (no end marker)"); return NULL; }
    int padz = 1;
    for (int i = 0; i < 8; i++) if (p[i]) padz = 0;
    say(depth + 1, "bitstream: bits len %u (8 pad bytes%s + %u)", blen, padz ? "" : ", NOT all zero,", blen - 8);
    return p + blen;
}

/* ctx: 0 = top level, 1 = inside an x86 block (may be plane, not x86), 2 = plane lane (bits 4 and 3 not tested) */
static const uint8_t* walk(const uint8_t* b, const uint8_t* end, int depth, int ctx)
{
    if (end - b < 5) { fail(b, "block shorter than 5 bytes"); return NULL; }
    const int flags = b[0];
    const uint32_t isize = le32(b + 1);
    if (ctx != 2 && (flags & 0x10)) {
        const uint8_t* p = b + 5;
        if (ctx == 1) { fail(b, "x86 block inside an x86 block"); return NULL; }
        if (end - b < 19) { fail(b, "x86 block shorter than 19 bytes"); return NULL; }
        if (isize > 0x7FFFFFFFu) { fail(b + 1, "isize above 2^31 - 1"); return NULL; }
        const uint32_t nr = rd_vlq(&p, end);
        if (nr < 1 || nr > isize / 65536 + 1) { fail(p, "x86 nr %u outside 1..isize/65536+1", nr); return NULL; }
        if ((size_t)(end - p) < (size_t)nr * 8 + 5) { fail(p, "x86 ranges cut"); return NULL; }
        say(depth, "x86 block (flags %02x): isize %u, %u range%s", flags, isize, nr, nr == 1 ? "" : "s");
        uint32_t prev_end = 0;
        for (uint32_t k = 0; k < nr; k++, p += 8) {
            const uint32_t st = le32(p), ln = le32(p + 4);
            if (st < prev_end || ln < 5 || ln > isize || st > isize - ln) { fail(p, "x86 range %u (%u, %u) not valid", k, st, ln); return NULL; }
            prev_end = st + ln;
            say(depth + 1, "range %u: start %u, len %u", k, st, ln);
        }
        if (le32(p + 1) != isize) { fail(p + 1, "inner isize differs from the x86 block's"); return NULL; }
        sum("x86 isize=%u nr=%u { ", isize, nr);
        if (!(p = walk(p, end, depth + 1, 1))) return NULL;
        sum(" }");
        return p;
    }
    if (ctx != 2 && (flags & 0x08)) {
        const uint8_t* p = b + 5;
        const int stride = (flags & 3) == 2 ? 4 : 2;
        if (isize > 0x7FFFFFFFu) { fail(b + 1, "isize above 2^31 - 1"); return NULL; }
        if (isize % (uint32_t)stride) { fail(b + 1, "isize not a multiple of the stride"); return NULL; }
        const uint32_t ps = isize / (uint32_t)stride;
        say(depth, "plane block (flags %02x): isize %u, stride %d", flags, isize, stride);
        sum("plane isize=%u stride=%d {", isize, stride);
        for (int lane = 0; lane < stride; lane++) {
            if (p >= end) { fail(p, "lane %d cut", lane); return NULL; }
            const int mb = *p++, delta = mb & 0x80, mode = mb & 0x7f;
            sum(" ");
            if (mode == 0) {
                if ((size_t)(end - p) < ps) { fail(p, "raw lane %d cut", lane); return NULL; }
                say(depth + 1, "lane %d: mode 0 (raw)%s, %u bytes", lane, delta ? ", delta" : "", ps);
                sum("raw");
                p += ps;
            } else if (mode == 1 || mode == 2) {
                if (end - p < 4) { fail(p, "lane %d csize cut", lane); return NULL; }
                const uint32_t csize = le32(p);
                p += 4;
                if ((size_t)(end - p) < csize) { fail(p - 4, "lane %d csize %u does not fit", lane, csize); return NULL; }
                const uint8_t* const le = p + csize;
                if (mode == 2) {
                    hinfo_t hi;
                    const uint8_t* q = huf1(p, le, &hi);
                    if (!q) return NULL;
                    if (le - q < 4) { fail(q, "lane %d total_bits cut", lane); return NULL; }
                    const uint32_t total = le32(q);
                    if ((size_t)csize != 261 + ((size_t)total + 7) / 8) { fail(q, "lane %d: csize is not 261 + ceil(total_bits / 8)", lane); return NULL; }
                    say(depth + 1, "lane %d: mode 2 (Huffman, format 1 header)%s, csize %u, k %d, %d symbols, longest code %d (%s), total_bits %u",
                        lane, delta ? ", delta" : "", csize, hi.k, hi.nsym, hi.maxlen, kraft_name(hi.kraft), total);
                    sum("huf(k=%d,nsym=%d,maxlen=%d)", hi.k, hi.nsym, hi.maxlen);
                } else {
                    if (csize < 5) { fail(p, "lane %d block shorter than 5 bytes", lane); return NULL; }
                    if (le32(p + 1) != ps) { fail(p + 1, "lane %d: block isize is not isize / stride", lane); return NULL; }
                    say(depth + 1, "lane %d: mode 1 (seq block)%s, csize %u", lane, delta ? ", delta" : "", csize);
                    sum("blk{ ");
                    const uint8_t* q = walk(p, le, depth + 2, 2);
                    if (!q) return NULL;
                    if (q != le) { say(depth + 2, "(%ld spare bytes inside csize)", (long)(le - q)); sum(" +spare=%ld", (long)(le - q)); }
                    sum(" }");
                }
                p = le;
            } else { fail(p - 1, "lane mode %d", mode); return NULL; }
            if (delta) sum("+delta");
        }
        sum(" }");
        return p;
    }
    return walk_plain(b, end, depth);
}

int main(int argc, char** argv)
{
    int a = 1;
    if (a < argc && !strcmp(argv[a], "-s")) { g_verbose = 0; a++; }
    if (a + 1 != argc) { fprintf(stderr, "usage: inspect [-s] FILE.seq\n"); return 2; }
    FILE* f = fopen(argv[a], "rb");
    if (!f) { perror(argv[a]); return 2; }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = (uint8_t*)malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read error\n"); return 2; }
    fclose(f);
    g_base = d;
    sum("%s", "");
    const uint8_t* e = walk(d, d + n, 0, 0);
    if (!e) {
        printf("ERROR: %s\n", g_err[0] ? g_err : "cannot walk the block");
        return 1;
    }
    if (e != d + n) sum(" +trail=%ld", (long)(d + n - e));
    printf(g_verbose ? "OK: %s\n" : "%s\n", g_sum);
    return 0;
}
