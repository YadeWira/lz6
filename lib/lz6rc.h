/*
 * rangecoder.h - Adaptive order-1 range coder for literals.
 *
 * Classic 32-bit arithmetic range coder (Subbotin-style):
 *   - 32-bit low/range state, byte-by-byte output.
 *   - Adaptive frequency model per context: 256 contexts (previous
 *     byte), each with 256 symbol frequencies + total.
 *   - No header: the decoder replicates the model adaptation exactly.
 *   - Symbols are bytes 0..255; escape not needed (full alphabet).
 *
 * API:
 *   rc_enc_ctx  — encoder state (init once per stream)
 *   rc_enc_lit  — encode one literal under context `ctx`
 *   rc_enc_flush— finalize, returns bytes written to out
 *   rc_dec_ctx  — decoder state (init from the same stream)
 *   rc_dec_lit  — decode one literal under context `ctx`
 */
#ifndef OZIP_RANGECODER_H
#define OZIP_RANGECODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_TOP      (1u << 24)
#define RC_BOT      (1u << 24)
#define RC_NUM_CTX  256

typedef struct {
    uint64_t low;      /* 64-bit: carry lives in the emitted bytes */
    uint32_t range;
    uint8_t* out;      /* write pointer */
    uint8_t* out_start;/* buffer start (for size computation) */
    uint8_t* out_end;
    uint8_t cache;
    uint64_t cache_size;
    int carry;
} rc_enc_ctx;

typedef struct {
    uint32_t low;
    uint32_t range;
    const uint8_t* in; /* read pointer */
    const uint8_t* in_end;
    uint32_t code;
} rc_dec_ctx;

/* frequency model: per-context cumulative-free counts, adaptive */
typedef struct {
    uint16_t freq[RC_NUM_CTX][256];   /* per context, per symbol */
    uint16_t total[RC_NUM_CTX];       /* sum of freq[ctx][*] */
    uint32_t cum[RC_NUM_CTX][256];    /* cumulative sums (for decode) */
    uint8_t  dirty[RC_NUM_CTX];       /* cum[ctx] needs rebuild */
} rc_model;

void rc_model_init(rc_model* m);

/* Encoder */
void rc_enc_init(rc_enc_ctx* e, uint8_t* out, size_t out_cap);
int  rc_enc_lit(rc_enc_ctx* e, rc_model* m, int ctx, unsigned sym);
size_t rc_enc_flush(rc_enc_ctx* e);

/* Decoder */
void rc_dec_init(rc_dec_ctx* d, const uint8_t* in, size_t in_len);
int  rc_dec_lit(rc_dec_ctx* d, rc_model* m, int ctx, unsigned* sym);

#ifdef __cplusplus
}
#endif

#endif /* OZIP_RANGECODER_H */
