/*
 * rangecoder.c - Adaptive order-1 range coder for literals.
 *
 * LZMA-style range coder:
 *   - low 64-bit, range 32-bit; renorm when range < 2^24.
 *   - ShiftLow emits (cache + carry) with 0xFF-run handling — the
 *     canonical LZMA carry propagation.
 *   - Encoder renorms AFTER coding; decoder renorms BEFORE decoding
 *     and reads 5 initial bytes (encoder flushes 5 ShiftLows).
 * Model: 256 contexts (previous byte), each a 256-symbol adaptive
 * frequency table. No header: the decoder re-derives adaptation.
 */

#include "lz6rc.h"
#include <string.h>

#define RC_FREQ_INIT  1
#define RC_TOTAL_MAX  16384

void rc_model_init(rc_model* m) {
    int c, s;
    for (c = 0; c < RC_NUM_CTX; c++) {
        for (s = 0; s < 256; s++) m->freq[c][s] = RC_FREQ_INIT;
        m->total[c] = 256 * RC_FREQ_INIT;
        m->dirty[c] = 1;
    }
}

static void rc_adapt(rc_model* m, int ctx, int sym) {
    if (m->total[ctx] + 1 > RC_TOTAL_MAX) {
        uint32_t nt = 0;
        for (int s = 0; s < 256; s++) {
            uint32_t v = m->freq[ctx][s] >> 1;
            if (v == 0) v = 1;
            m->freq[ctx][s] = (uint16_t)v;
            nt += v;
        }
        m->total[ctx] = (uint16_t)nt;
        m->dirty[ctx] = 1;   /* halving requires full rebuild */
    } else {
        /* incremental: freq[sym]++ raises cum[s] for all s > sym */
        for (int s = sym + 1; s < 256; s++) m->cum[ctx][s]++;
    }
    m->freq[ctx][sym] = (uint16_t)(m->freq[ctx][sym] + 1);
    m->total[ctx] = (uint16_t)(m->total[ctx] + 1);
}

/* rebuild cum[ctx] from freq[ctx] (called when dirty) */
static void rc_rebuild_cum(rc_model* m, int ctx) {
    uint32_t acc = 0;
    for (int s = 0; s < 256; s++) {
        m->cum[ctx][s] = acc;
        acc += m->freq[ctx][s];
    }
    m->dirty[ctx] = 0;
}

/* --- encoder --- */

void rc_enc_init(rc_enc_ctx* e, uint8_t* out, size_t out_cap) {
    e->low = 0;
    e->range = 0xFFFFFFFFu;
    e->out = out;
    e->out_start = out;
    e->out_end = out + out_cap;
    e->cache = 0;
    e->cache_size = 1;   /* LZMA starts with cacheSize=1 */
    e->carry = 0;
}

/* LZMA RangeEnc_ShiftLow: emit cache+carry, then the 0xFF run */
static void rc_enc_shift_low(rc_enc_ctx* e) {
    if ((uint32_t)e->low < 0xFF000000u || (int)(e->low >> 32) != 0) {
        uint8_t temp = e->cache;
        do {
            if (e->out < e->out_end) {
                *e->out++ = (uint8_t)(temp + (uint8_t)(e->low >> 32));
            }
            temp = 0xFF;
        } while (--e->cache_size != 0);
        e->cache = (uint8_t)((uint32_t)e->low >> 24);
    }
    e->cache_size++;
    e->low = (uint32_t)e->low << 8;
}

int rc_enc_lit(rc_enc_ctx* e, rc_model* m, int ctx, unsigned sym) {
    if (ctx < 0 || ctx >= RC_NUM_CTX || sym > 255) return 0;
    uint32_t f = m->freq[ctx][sym];
    uint32_t total = m->total[ctx];
    if (total == 0 || f == 0) return 0;

    uint32_t cum = 0;
    for (int s = 0; s < (int)sym; s++) cum += m->freq[ctx][s];

    e->range /= total;
    e->low += (uint64_t)e->range * cum;
    e->range *= f;

    while (e->range < RC_TOP) {
        rc_enc_shift_low(e);
        e->range <<= 8;
    }

    rc_adapt(m, ctx, (int)sym);
    return 1;
}

size_t rc_enc_flush(rc_enc_ctx* e) {
    for (int i = 0; i < 5; i++) rc_enc_shift_low(e);
    return (size_t)(e->out - e->out_start);
}

/* --- decoder --- */

void rc_dec_init(rc_dec_ctx* d, const uint8_t* in, size_t in_len) {
    d->range = 0xFFFFFFFFu;
    d->in = in;
    d->in_end = in + in_len;
    d->code = 0;
    for (int i = 0; i < 5; i++) {
        if (d->in < d->in_end) d->code = (d->code << 8) | *d->in++;
    }
}

int rc_dec_lit(rc_dec_ctx* d, rc_model* m, int ctx, unsigned* sym) {
    if (ctx < 0 || ctx >= RC_NUM_CTX) return 0;
    uint32_t total = m->total[ctx];
    if (total == 0) return 0;

    /* renormalize BEFORE decoding (the encoder renormed after coding) */
    while (d->range < RC_TOP) {
        uint8_t b = 0;
        if (d->in < d->in_end) b = *d->in++;
        d->code = (d->code << 8) | b;
        d->range <<= 8;
    }

    d->range /= total;
    uint32_t target = d->code / d->range;
    if (target >= total) target = total - 1;
    if (m->dirty[ctx]) rc_rebuild_cum(m, ctx);
    /* binary search: find largest s with cum[s] <= target */
    int lo = 0, hi = 256;
    while (lo + 1 < hi) {
        int mid = (lo + hi) >> 1;
        if (m->cum[ctx][mid] <= target) lo = mid;
        else hi = mid;
    }
    int s = lo;
    if (s >= 256) return 0;
    *sym = (unsigned)s;

    d->code -= d->range * m->cum[ctx][s];
    d->range *= m->freq[ctx][s];

    rc_adapt(m, ctx, s);
    return 1;
}
