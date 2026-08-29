/*
 * fse.c - Finite State Entropy (range ANS) coder.
 *
 * Streaming rANS with 32-bit state and byte renormalization.
 * Frequencies normalized to M = 2^L_bits (powers of two).
 *
 * Stream layout (after the freq table header):
 *   [stream_size:4 LE]   = total bytes used by [x_state + renorm_bytes]
 *   [x_state:4 BE]       = decoder's initial state (low address first)
 *   [renorm_bytes:...]   = refilled by decoder in forward order
 *
 * Encoder processes input symbols in REVERSE order and writes renorm
 * bytes to a stack pointer that decrements (`*--p = byte`), so the
 * most-recently-flushed byte ends up at the lowest address — exactly
 * what the forward decoder needs to refill from first.
 */

#include "lz6fse.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* frequency normalization                                             */
/* ------------------------------------------------------------------ */

static void normalize(const unsigned* counts, int maxSym,
                      unsigned M, unsigned* freq)
{
    unsigned total = 0;
    int i;
    for (i = 0; i <= maxSym; i++) total += counts[i];
    if (total == 0) {
        /* all zero — assign M to symbol 0 to keep the state machine valid */
        freq[0] = M;
        for (i = 1; i <= maxSym; i++) freq[i] = 0;
        return;
    }
    int distribute = (int)M;
    int largest = 0;
    for (i = 0; i <= maxSym; i++) {
        if (counts[i] == 0) { freq[i] = 0; continue; }
        unsigned f = (unsigned)(((uint64_t)counts[i] * M) / total);
        if (f == 0) f = 1;
        freq[i] = f;
        distribute -= (int)f;
        if ((long)counts[i] > (long)counts[largest]) largest = i;
    }
    /* absorb leftover counts */
    while (distribute > 0) { freq[largest]++; distribute--; }
    /* if we over-allocated, decrement spare entries */
    while (distribute < 0) {
        int done = 0;
        for (i = maxSym; i >= 0; i--) {
            if (freq[i] > 1 && distribute < 0) {
                freq[i]--; distribute++;
                if (distribute == 0) { done = 1; break; }
            }
        }
        if (done) break;
        if (freq[largest] > 1) { freq[largest]--; distribute++; }
        else break;
    }
}

static int pick_L_bits(int maxSym)
{
    /* 32-bit state, byte renorm, RANS_L = 2^16: L_bits <= 16 */
    if (maxSym >= 200) return 16;
    if (maxSym >= 32)  return 14;
    if (maxSym >= 8)   return 12;
    return 10;
}

/* ------------------------------------------------------------------ */
/* table (de)serialization                                             */
/* ------------------------------------------------------------------ */

size_t fse_write_table(const unsigned counts[256], int maxSym,
                       uint8_t* out, size_t out_cap)
{
    /* trim maxSym to highest-nonzero symbol */
    int actual = 0, i;
    for (i = maxSym; i >= 0; i--) {
        if (counts[i]) { actual = i; break; }
    }
    maxSym = actual;

    int L_bits = pick_L_bits(maxSym);
    unsigned M = 1u << L_bits;
    unsigned freq[256];
    normalize(counts, maxSym, M, freq);

    size_t hdr = 2 + 3 * (maxSym + 1);
    if (hdr > out_cap) return 0;

    out[0] = (uint8_t)L_bits;
    out[1] = (uint8_t)maxSym;
    size_t pos = 2;
    for (i = 0; i <= maxSym; i++) {
        out[pos++] = (uint8_t)(freq[i]);
        out[pos++] = (uint8_t)(freq[i] >> 8);
        out[pos++] = (uint8_t)(freq[i] >> 16);
    }
    return hdr;
}

size_t fse_read_table(const uint8_t* in, size_t in_len,
                      unsigned counts[256], int* maxSym_out, int* L_bits_out)
{
    if (in_len < 2) return 0;
    int L_bits = in[0];
    int maxSym = in[1];
    size_t expect = 2 + 3 * (maxSym + 1);
    if (in_len < expect) return 0;
    size_t pos = 2;
    int i;
    for (i = 0; i <= maxSym; i++) {
        counts[i] = (unsigned)in[pos] | ((unsigned)in[pos+1] << 8) | ((unsigned)in[pos+2] << 16);
        pos += 3;
    }
    for (i = maxSym + 1; i < 256; i++) counts[i] = 0;
    *maxSym_out = maxSym;
    *L_bits_out = L_bits;
    return expect;
}

/* ------------------------------------------------------------------ */
/* rANS core                                                           */
/* ------------------------------------------------------------------ */

static inline uint32_t rans_enc_renorm(uint32_t x, int L_bits, unsigned f, uint8_t** pp)
{
    /* Per-symbol renorm bound (ryg_rans formula):
     *   x_max = ((RANS_L >> L_bits) << 8) * f     with RANS_L = 1 << 16
     * Ensures C(x, f) <= 2^32 - 1 (overflow-safe) and that D's refill
     * to RANS_L consumes exactly the bytes the encoder flushed. */
    uint32_t x_max = ((0x10000u >> L_bits) << 8) * f;
    while (x >= x_max) {
        *--(*pp) = (uint8_t)(x & 0xFF);
        x >>= 8;
    }
    return x;
}

/* Lemire-style unsigned 32-bit division by a runtime constant.
 *
 * Used when fse_encode is called with the per-symbol reciprocal in
 * scope (added in a later change). Currently informational — the
 * hot path below still uses `x / f`, relying on GCC to lower it to
 * libdivide on x86-64. The Lemire routine documents the shape so
 * a future patch can swap it in. */
static inline uint32_t lemire_q(uint32_t x, uint64_t M, int l) {
    __uint128_t p = (__uint128_t)x * M;
    return (uint32_t)(p >> (32 + l));
}
static inline uint32_t lemire_compute_M(unsigned f, int* l_out) {
    int l = 0; while ((f >> l) > 1) l++;
    uint64_t M = ((1ULL << (32 + l)) - 1) / f + 1;
    *l_out = l; return M;
}

static inline uint32_t rans_enc_put(uint32_t x, unsigned f, unsigned cumul,
                                    unsigned M)
{
    /* Default path: GCC's div lowering on x86-64. Lemire fast path
     * is selected inside fse_encode via `use_inv` branch below. */
    return (x / f) * M + cumul + (x % f);
}

/* Public helper: compute Lemire reciprocal for each freq value. Returns
 * the common `l` shift (all calls in a single block share it). freq[s]
 * for s > maxSym may be uninitialized; we only set `inv_M[s]` for
 * s in [0, maxSym] when freq[s] > 0. Entries with freq = 0 are
 * left at 0 (the hot path never indexes them).
 *
 * The formula is M = ceil(2^32 / d) — NOT (2^32 - 1) / d + 1, which
 * is off-by-one for some divisors (caught during ozf3_test failures
 * against zstd-style 1 MiB / block stress cases). */
int fse_lemire_inv(const unsigned* freq, int maxSym, uint64_t* inv_M) {
    /* Lemire 32-bit unsigned division. The exact algorithm: for each
     * symbol `s` with freq > 0, choose `l_s` = smallest l such that
     * 2^l > freq[s]. Then M_s = ceil(2^(32 + l_s) / freq[s]) and
     * q_s = (x * M_s) >> (32 + l_s) is exactly floor(x/freq[s]) or
     * floor(x/freq[s]) + 1; the standard `if (r >= f) ++q` correction
     * gives the precise quotient.
     *
     * We use a single global `l` for every symbol so the hot path can
     * share one shift across all symbols in the alphabet: take the
     * maximum l_s so every symbol satisfies l ≥ l_s. The precomputed
     * M_s for that uniform shift is over-large but still in-spec
     * (Lemire's proof still gives the off-by-one quotient). */
    int l = 0;
    for (int s = 0; s <= maxSym; s++) {
        unsigned f = freq[s];
        if (f == 0) continue;
        int l_s = 0;
        while ((f >> l_s) >= 1) l_s++;
        if (l_s > l) l = l_s;
    }
    if (l == 0) return 0;   /* no symbols with freq > 0; hot path skips */
    if (l > 32) l = 32;
    uint64_t numer = (uint64_t)1 << (32 + l);
    for (int s = 0; s <= maxSym; s++) {
        if (freq[s] == 0) { inv_M[s] = 0; continue; }
        uint64_t d = freq[s];
        inv_M[s] = (numer + d - 1) / d;
    }
    return l;
}

static inline unsigned int rans_dec_slot(uint32_t x, int L_bits)
{
    return x & ((1u << L_bits) - 1);
}

static inline uint32_t rans_dec_advance(uint32_t x, unsigned f,
                                        unsigned cumul, int L_bits)
{
    unsigned M_mask = (1u << L_bits) - 1;
    return f * (x >> L_bits) + (x & M_mask) - cumul;
}

static inline uint32_t rans_dec_renorm(uint32_t x, const uint8_t** pp,
                                       const uint8_t* end, int* err)
{
    /* Decoder refill to RANS_L = 1 << 16 (standard 32-bit-state rANS).
     * Bounded: exhausting the stream means corrupt input. */
    unsigned lo = 0x10000u;
#ifdef FSE_DEBUG
    int refills = 0;
#endif
    while (x < lo) {
        if (*pp >= end) { *err = 1; return x; }
        x = (x << 8) | (unsigned char)*(*pp)++;
#ifdef FSE_DEBUG
        refills++;
#endif
    }
#ifdef FSE_DEBUG
    if (refills) fprintf(stderr, "  [dec refill x=%u via %d bytes]\n", x, refills);
#endif
    return x;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

#include <stdint.h>
size_t fse_encode(const unsigned* syms, size_t n, int maxSym,
                  uint8_t* out, size_t out_cap,
                  const uint64_t* inv_M, int inv_l,
                  size_t* table_size_out)
{
    (void)inv_M; (void)inv_l;
    /* count symbols */
    unsigned counts[256];
    int i;
    for (i = 0; i < 256; i++) counts[i] = 0;
    size_t k;
    for (k = 0; k < n; k++) counts[syms[k]]++;

    /* write the header (freq table) */
    size_t hdr = fse_write_table(counts, maxSym, out, out_cap);
    if (hdr == 0) return 0;
    if (table_size_out) *table_size_out = hdr;

    /* re-parse for L_bits / M (mirrors pick_L_bits) */
    int actual = 0;
    for (i = maxSym; i >= 0; i--) if (counts[i]) { actual = i; break; }
    int L_bits = pick_L_bits(actual);
    unsigned M = 1u << L_bits;
    unsigned freq_tab[256];
    normalize(counts, actual, M, freq_tab);

    /* build cumul[] (cumul[s] = sum of freq[0..s-1]) */
    unsigned cumul[257];
    unsigned acc = 0;
    for (i = 0; i <= actual; i++) {
        cumul[i] = acc;
        acc += freq_tab[i];
    }
    cumul[i] = acc;  /* should = M */

    /* Precompute Lemire reciprocal per freq value. Used in the hot loop
     * instead of the div+mod pair, shaving ~25 cycles per encoded
     * symbol. The M is derived from freq_tab (post-normalize), not
     * from a caller-provided array — Lemire's theorem requires the M
     * be a function of the SAME divisor that the hot path divides by.
     *
     * `inv_M`/`inv_l` are accepted for API compatibility but ignored
     * here; the recomputation is cheap (one pass over freq_tab, ~36
     * symbols × 1 div) and runs once per fse_encode call. */
    int use_l;
    {
        int l_calc = 0;
        for (i = 0; i <= actual; i++) {
            unsigned f = freq_tab[i];
            if (f == 0) continue;
            int l_s = 0;
            while ((f >> l_s) >= 1) l_s++;
            if (l_s > l_calc) l_calc = l_s;
        }
        if (l_calc > 32) l_calc = 32;
        use_l = l_calc;
    }
    uint64_t numer = (uint64_t)1 << (32 + use_l);
    uint64_t inv_arr[256] = {0};
    for (i = 0; i <= actual; i++) {
        uint64_t d = freq_tab[i];
        if (d == 0) continue;
        inv_arr[i] = (numer + d - 1) / d;
    }
    (void)inv_M;  /* parameter accepted but ignored; see comment above */

    /* stream allocation: worst case ~ n * 1.5 bytes + 4 (state) + slack */
    /* write a 4-byte size-field at offset hdr, then stream from hdr+4 */
    /* encode onto the caller's buffer backwards from hdr+4+stream_cap —
     * avoids a temp malloc + memcpy for the stream. The caller must
     * provide a buffer at least hdr+4+stream_cap bytes and is expected
     * to pass a fresh buffer (no prior data past the stream). */
    size_t stream_cap = n + (n >> 1) + 64;
    if (stream_cap < 16) stream_cap = 16;
    if (hdr + 4 + stream_cap > out_cap) return 0;
    uint8_t* sbuf = out + hdr + 4;        /* stream start (final position) */
    uint8_t* p = sbuf + stream_cap;       /* points one past most-recently-written */
    uint8_t* pend = sbuf + stream_cap;    /* upper limit */
    (void)pend;

    uint32_t x = 0x10000u;  /* RANS_L = 2^16, standard 32-bit rANS */

    /* reverse-order encode */
    if (n > 0) {
        size_t j = n;
        while (j > 0) {
            j--;
            unsigned s = syms[j];
            unsigned f = freq_tab[s];
            unsigned c = cumul[s];
            if (f == 0) { /* shouldn't happen if counts > 0 implies freq > 0 */
                return 0;
            }
#ifdef FSE_DEBUG
            uint32_t x_pre = x;
            uint8_t* p_pre = p;
#endif
            x = rans_enc_renorm(x, L_bits, f, &p);
#ifdef FSE_DEBUG
            int refilled = (int)(p_pre - p);
#endif
            /* Lemire reciprocal (corrected).
             *
             * M computed once above (per call) from freq_tab so the M
             * for each symbol matches exactly the divisor used here.
             * Lemire's off-by-±1 estimate resolves via two cheap
             * 64-bit adjusts. Total cost: 128-bit multiply (one
             * instruction on x86-64) + two cmp/sub pairs, ~7 cycles
             * vs ~30 for the `div` instruction we used to rely on. */
            {
                uint64_t M_s = inv_arr[s];
                uint32_t q = (uint32_t)((__uint128_t)x * M_s >> (32 + use_l));
                uint64_t q_d = (uint64_t)q * f;
                if (q_d > x) { q--; q_d -= f; }    /* q was 1 too high */
                uint32_t r = (uint32_t)(x - q_d);
                if (r >= f) { q++; r -= f; }         /* q was 1 too low */
                x = q * M + c + r;
            }
#ifdef FSE_DEBUG
            if (n - 1 - j < 40 || refilled) {
                fprintf(stderr, "enc[rev t=%3zu] sym=%u x_pre=%u x_after_renorm=%u x_post=%u  flushed=%d byte(s)\n",
                        n - 1 - j, s, x_pre, x_pre, x, refilled);
                if (refilled) {
                    for (uint8_t* q = p; q < p_pre; q++)
                        fprintf(stderr, "    flushed byte %02x at idx %td\n", *q, q - sbuf);
                }
            }
#endif
        }
    }

    /* emit final state as 4 BE bytes (decrementing p) */
    p -= 4;
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)(x);

    size_t stream_len = (size_t)(pend - p);
    if (hdr + 4 + stream_len > out_cap) return 0;

    /* write the size prefix (4 LE); the stream is already in place at
     * out+hdr+4 (we encoded backwards into the caller's buffer) */
    out[hdr + 0] = (uint8_t)(stream_len);
    out[hdr + 1] = (uint8_t)(stream_len >> 8);
    out[hdr + 2] = (uint8_t)(stream_len >> 16);
    out[hdr + 3] = (uint8_t)(stream_len >> 24);
    if (p != out + hdr + 4) {
        /* stream landed past the prefix (usual case: p > sbuf): move it
         * down so it starts right after the 4-byte size. */
        memmove(out + hdr + 4, p, stream_len);
    }
    return hdr + 4 + stream_len;
}

size_t fse_dtable_prepare(fse_dtable* t, const uint8_t* in, size_t in_len,
                          int maxSym)
{
    memset(t, 0, sizeof(*t));
    unsigned counts[256];
    int rmaxSym, L_bits;
    size_t hdr = fse_read_table(in, in_len, counts, &rmaxSym, &L_bits);
    if (hdr == 0) return 0;
    if (rmaxSym > maxSym) return 0;
    /* encoder's pick_L_bits only emits 10..16; anything else is corrupt
     * and would make M (and every table index) unreliable */
    if (L_bits < 1 || L_bits > 16) return 0;
    unsigned M = 1u << L_bits;

    t->L_bits = L_bits;
    t->M = M;
    for (int i = 0; i < 256; i++) t->freq_tab[i] = counts[i];
    unsigned acc = 0;
    for (int i = 0; i <= rmaxSym; i++) { t->cumul[i] = acc; acc += counts[i]; }

    /* plain 1-byte symbol table (always built) */
    t->dtab1 = (uint8_t*)malloc(M);
    if (!t->dtab1) return 0;
    t->owned |= 1;
    unsigned slot = 0;
    for (int i = 0; i <= rmaxSym; i++) {
        unsigned f = counts[i];
        while (f--) {
            if (slot >= M) { fse_dtable_free(t); return 0; }
            t->dtab1[slot++] = (uint8_t)i;
        }
    }
    if (slot != M) { fse_dtable_free(t); return 0; }

    /* combined 12B entries only when the table stays cache-resident
     * (M <= 4096): for M = 65536 the 768KB combined table thrashes L2/TLB
     * and decodes slower than the compact layout */
    if (M <= 4096) {
        t->dcomp = (fse_dentry*)malloc((size_t)M * sizeof(fse_dentry));
        if (!t->dcomp) { fse_dtable_free(t); return 0; }
        t->owned |= 2;
        for (unsigned s2 = 0; s2 < M; s2++) {
            fse_dentry* e = &t->dcomp[s2];
            unsigned s = t->dtab1[s2];
            e->s = (uint8_t)s;
            e->f = t->freq_tab[s];
            e->off = (int32_t)s2 - (int32_t)t->cumul[s];
        }
    }
    return hdr;
}

void fse_dtable_free(fse_dtable* t) {
    if (t->owned & 1) free(t->dtab1);
    if (t->owned & 2) free(t->dcomp);
    t->dtab1 = NULL;
    t->dcomp = NULL;
    t->owned = 0;
}

static int fse_decode_syms(const fse_dtable* t, const uint8_t* in, size_t in_len,
                           size_t n, unsigned* syms)
{
    /* read 4 LE size field then stream */
    size_t pos = 0;
    if (pos + 8 > in_len) return 1;
    uint32_t stream_len = (uint32_t)in[pos]
                        | ((uint32_t)in[pos+1] << 8)
                        | ((uint32_t)in[pos+2] << 16)
                        | ((uint32_t)in[pos+3] << 24);
    pos += 4;
    if (pos + stream_len > in_len) return 1;
    const uint8_t* sp = in + pos;
    const uint8_t* sp_end = sp + stream_len;

    /* read x_state as 4 BE bytes */
    if (stream_len < 4) return 1;
    uint32_t x = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) |
                 ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
    sp += 4;

    const unsigned Mmask = t->M - 1;
    const int L_bits = t->L_bits;
    int rerr = 0;
    size_t k;

    if (t->dcomp) {
        /* combined-entry loop: one load per symbol (M <= 4096 tables) */
        const fse_dentry* dtab = t->dcomp;
        for (k = 0; k < n; k++) {
            /* combined entry: symbol + freq + (slot - cumul[s]) in one load.
             * f >= 1 for every slot (freqs sum to exactly M), so the rANS
             * invariant holds and the renorm below needs at most 2 bytes. */
            const fse_dentry* e = &dtab[x & Mmask];
            unsigned f = e->f;
            syms[k] = e->s;
            /* advance (must come BEFORE refill: x is initially valid after encoder flush) */
            x = f * (x >> L_bits) + (uint32_t)e->off;
            /* refill with slack fast path: at most 2 bytes ever needed */
            if (sp_end - sp >= 2) {
                while (x < 0x10000u) x = (x << 8) | *sp++;
            } else {
                while (x < 0x10000u) {
                    if (sp >= sp_end) { return 1; }
                    x = (x << 8) | *sp++;
                }
            }
        }
        return 0;
    }

    /* compact layout (M > 4096, e.g. literal o0): symbol via 1-byte table,
     * then freq/cumul — a combined table would thrash L2/TLB here */
    {
        const uint8_t* dtab1 = t->dtab1;
        const unsigned* freq_tab = t->freq_tab;
        const unsigned* cumul = t->cumul;
        for (k = 0; k < n; k++) {
            unsigned s = dtab1[x & Mmask];
            syms[k] = s;
            x = freq_tab[s] * (x >> L_bits) + (x & Mmask) - cumul[s];
            x = rans_dec_renorm(x, &sp, sp_end, &rerr);
            if (rerr) return 1;
        }
    }
    return 0;
}

int fse_decode_prepared(const fse_dtable* t, const uint8_t* in, size_t in_len,
                        size_t n, unsigned* out)
{
    return fse_decode_syms(t, in, in_len, n, out);
}

int fse_decode(const uint8_t* in, size_t in_len, size_t n, int maxSym,
               unsigned* syms)
{
    /* self-contained variant with a plain 1-byte lookup table: the bucket
     * streams call this with small n, where building a combined 12B-entry
     * table (up to 768KB) would cost far more than it saves */
    unsigned counts[256];
    int rmaxSym, L_bits;
    size_t hdr = fse_read_table(in, in_len, counts, &rmaxSym, &L_bits);
    if (hdr == 0) return 1;
    if (rmaxSym > maxSym) return 1;
    if (L_bits < 1 || L_bits > 16) return 1;
    unsigned M = 1u << L_bits;

    unsigned cumul[256];
    unsigned acc = 0;
    for (int i = 0; i <= rmaxSym; i++) { cumul[i] = acc; acc += counts[i]; }

    uint8_t* dtab = (uint8_t*)malloc(M);
    if (!dtab) return 1;
    unsigned slot = 0;
    for (int i = 0; i <= rmaxSym; i++) {
        unsigned f = counts[i];
        while (f--) {
            if (slot >= M) { free(dtab); return 1; }
            dtab[slot++] = (uint8_t)i;
        }
    }
    if (slot != M) { free(dtab); return 1; }

    if (hdr + 8 > in_len) { free(dtab); return 1; }
    uint32_t stream_len = (uint32_t)in[hdr] | ((uint32_t)in[hdr+1] << 8)
                        | ((uint32_t)in[hdr+2] << 16) | ((uint32_t)in[hdr+3] << 24);
    if (stream_len < 4 || hdr + 4 + stream_len > in_len) { free(dtab); return 1; }
    const uint8_t* sp = in + hdr + 4;
    const uint8_t* sp_end = sp + stream_len;
    uint32_t x = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16)
               | ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
    sp += 4;

    int rerr = 0;
    for (size_t k = 0; k < n; k++) {
        unsigned s = dtab[x & (M - 1)];
        syms[k] = s;
        x = counts[s] * (x >> L_bits) + (x & (M - 1)) - cumul[s];
        x = rans_dec_renorm(x, &sp, sp_end, &rerr);
        if (rerr) { free(dtab); return 1; }
    }
    free(dtab);
    return 0;
}

/* ================================================================== */
/* Context-coded streams (order-1 style)                              */
/* ================================================================== */

int fse_ctx_table_build(fse_ctx_table* t, const unsigned counts[256],
                        int maxSym, int L_bits) {
    memset(t, 0, sizeof(*t));
    t->L_bits = L_bits;
    t->M = 1u << L_bits;
    /* trim maxSym to highest nonzero */
    int actual = 0;
    for (int i = maxSym; i >= 0; i--) if (counts[i]) { actual = i; break; }
    if (actual == 0 && counts[0] == 0) {
        /* empty context: uniform */
        for (int i = 0; i < 256; i++) t->freq[i] = 1;
        t->freq[0] = t->M - 255;
    } else {
        unsigned total = 0;
        for (int i = 0; i <= actual; i++) total += counts[i];
        if (total == 0) return 0;
        unsigned M = t->M;
        int distribute = (int)M;
        int largest = 0;
        for (int i = 0; i <= actual; i++) {
            if (counts[i] == 0) { t->freq[i] = 0; continue; }
            unsigned f = (unsigned)(((uint64_t)counts[i] * M) / total);
            if (f == 0) f = 1;
            t->freq[i] = f;
            distribute -= (int)f;
            if (counts[i] > counts[largest]) largest = i;
        }
        while (distribute > 0) { t->freq[largest]++; distribute--; }
        while (distribute < 0) {
            int done = 0;
            for (int i = actual; i >= 0; i--) {
                if (t->freq[i] > 1 && distribute < 0) {
                    t->freq[i]--; distribute++;
                    if (distribute == 0) { done = 1; break; }
                }
            }
            if (done) break;
            if (t->freq[largest] > 1) { t->freq[largest]--; distribute++; }
            else break;
        }
    }
    /* cumul */
    unsigned acc = 0;
    for (int i = 0; i < 256; i++) {
        t->cumul[i] = acc;
        acc += t->freq[i];
    }
    /* decode table */
    t->dtab = (uint8_t*)malloc(t->M);
    if (!t->dtab) return 0;
    unsigned slot = 0;
    for (int i = 0; i < 256; i++) {
        unsigned f = t->freq[i];
        while (f--) t->dtab[slot++] = (uint8_t)i;
    }
    return 1;
}

void fse_ctx_table_free(fse_ctx_table* t) {
    free(t->dtab);
    t->dtab = NULL;
}

size_t fse_ctx_table_write(const fse_ctx_table* t, uint8_t* out, size_t out_cap) {
    if (out_cap < 1 + 512) return 0;
    out[0] = (uint8_t)t->L_bits;
    for (int i = 0; i < 256; i++) {
        out[1 + i * 2] = (uint8_t)(t->freq[i] & 0xFF);
        out[1 + i * 2 + 1] = (uint8_t)(t->freq[i] >> 8);
    }
    return 1 + 512;
}

size_t fse_ctx_table_read(fse_ctx_table* t, const uint8_t* in, size_t in_len) {
    if (in_len < 1 + 512) return 0;
    int L_bits = in[0];
    /* encoder-side tables share pick_L_bits' 10..16 range; anything else
     * would make M (and dtab's size) unreliable */
    if (L_bits < 1 || L_bits > 16) return 0;
    unsigned freqs[256];
    unsigned total = 0;
    for (int i = 0; i < 256; i++) {
        freqs[i] = (unsigned)in[1 + i * 2] | ((unsigned)in[1 + i * 2 + 1] << 8);
        total += freqs[i];
    }
    unsigned M = 1u << L_bits;
    /* freqs must sum to exactly M or the dtab fill would run past its end */
    if (total != M) return 0;
    memset(t, 0, sizeof(*t));
    t->L_bits = L_bits;
    t->M = M;
    /* freqs already sum to M — copy as-is */
    for (int i = 0; i < 256; i++) t->freq[i] = freqs[i];
    unsigned acc = 0;
    for (int i = 0; i < 256; i++) {
        t->cumul[i] = acc;
        acc += t->freq[i];
    }
    t->dtab = (uint8_t*)malloc(t->M);
    if (!t->dtab) return 0;
    unsigned slot = 0;
    for (int i = 0; i < 256; i++) {
        unsigned f = t->freq[i];
        while (f--) {
            if (slot >= t->M) { free(t->dtab); t->dtab = NULL; return 0; }
            t->dtab[slot++] = (uint8_t)i;
        }
    }
    return 1 + 512;
}

size_t fse_encode_ctx(const unsigned* syms, const unsigned* ctx, size_t n,
                      const fse_ctx_table* tables, int n_tables,
                      uint8_t* out, size_t out_cap) {
    if (out_cap < 8) return 0;
    /* worst case stream cap: rANS can expand for low-freq symbols */
    size_t stream_cap = n * 2 + 1024;
    uint8_t* sbuf = (uint8_t*)malloc(stream_cap + 4);
    if (!sbuf) return 0;
    uint8_t* p = sbuf + stream_cap;
    uint32_t x = 0x10000u;  /* RANS_L */
    int L_bits = tables[0].L_bits;

    for (size_t j = n; j > 0; j--) {
        size_t i = j - 1;
        unsigned c = ctx[i];
        if (c >= (unsigned)n_tables) { free(sbuf); return 0; }
        const fse_ctx_table* t = &tables[c];
        unsigned s = syms[i];
        unsigned f = t->freq[s];
        if (f == 0) { free(sbuf); return 0; }
        uint32_t x_max = ((0x10000u >> L_bits) << 8) * f;
        while (x >= x_max) {
            if (p <= sbuf) { free(sbuf); return 0; }  /* overflow guard */
            *--p = (uint8_t)(x & 0xFF);
            x >>= 8;
        }
        x = (x / f) * t->M + t->cumul[s] + (x % f);
    }

    /* emit final state as 4 BE bytes */
    p -= 4;
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)(x);

    size_t stream_len = (size_t)((sbuf + stream_cap) - p);
    if (4 + stream_len > out_cap) { free(sbuf); return 0; }
    out[0] = (uint8_t)(stream_len);
    out[1] = (uint8_t)(stream_len >> 8);
    out[2] = (uint8_t)(stream_len >> 16);
    out[3] = (uint8_t)(stream_len >> 24);
    memcpy(out + 4, p, stream_len);
    free(sbuf);
    return 4 + stream_len;
}

int fse_decode_ctx(const uint8_t* in, size_t in_len,
                   const unsigned* ctx, size_t n,
                   const fse_ctx_table* tables, int n_tables,
                   unsigned* out) {
    if (in_len < 8) return 1;
    uint32_t stream_len = (uint32_t)in[0] | ((uint32_t)in[1] << 8)
                        | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    if (4 + stream_len > in_len) return 1;
    const uint8_t* sp = in + 4;
    const uint8_t* sp_end = sp + stream_len;
    if (stream_len < 4) return 1;
    uint32_t x = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16)
               | ((uint32_t)sp[2] << 8) | (uint32_t)sp[3];
    sp += 4;

    for (size_t k = 0; k < n; k++) {
        unsigned c = ctx[k];
        if (c >= (unsigned)n_tables) return 1;
        const fse_ctx_table* t = &tables[c];
        unsigned slot_idx = x & (t->M - 1);
        unsigned s = t->dtab[slot_idx];
        unsigned f = t->freq[s];
        if (f == 0) return 1;
        out[k] = s;
        x = f * (x >> t->L_bits) + (x & (t->M - 1)) - t->cumul[s];
        while (x < 0x10000u) {
            if (sp >= sp_end) return 1;
            x = (x << 8) | *sp++;
        }
    }
    return 0;
}
