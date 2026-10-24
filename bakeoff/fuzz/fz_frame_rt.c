/* libFuzzer: frame compression (one-shot, streaming, streaming with flushes) with random preferences, then streaming decompression with
 * random chunking: the output must equal the input and the frame must end exactly where the compressed data does. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lz6frame.h"
static uint8_t* gbuf; static size_t glen, gcap;
static void put(const void* p, size_t n) { if (glen + n > gcap) { gcap = (glen + n) * 2 + 64; gbuf = realloc(gbuf, gcap); } memcpy(gbuf + glen, p, n); glen += n; }
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
  if (n < 8) return 0;
  LZ6F_preferences_t p; memset(&p, 0, sizeof p);
  unsigned b0 = data[0], bs = b0 & 7;
  if (bs > 4 && (data[6] & 15)) bs -= 4;                                      /* the 16-256 MB blocks only now and then */
  p.frameInfo.blockSizeID = (LZ6F_blockSizeID_t)bs;
  p.frameInfo.blockMode = (LZ6F_blockMode_t)((b0 >> 3) & 1);
  p.frameInfo.contentChecksumFlag = (LZ6F_contentChecksum_t)((b0 >> 4) & 1);
  p.frameInfo.blockCodec = (LZ6F_blockCodec_t)((b0 >> 5) & 1);
  p.autoFlush = (b0 >> 6) & 1;
  p.compressionLevel = data[1] % 16;
  const uint8_t* src = data + 8; size_t sn = n - 8;
  if (p.compressionLevel >= 8 && sn > 4096) sn = 4096;                       /* keep the slow levels fast enough to fuzz */
  if (data[2] & 1) p.frameInfo.contentSize = sn;
  int mode = data[5] & 3; if (mode == 3) mode = 0;
  glen = 0;
  LZ6F_compressionContext_t cc = NULL;
  if (mode == 0) {
    size_t cap = LZ6F_compressFrameBound(sn, &p); uint8_t* c = malloc(cap);
    size_t r = LZ6F_compressFrame(c, cap, src, sn, &p);
    if (LZ6F_isError(r)) __builtin_trap();
    put(c, r); free(c);
  } else {
    if (LZ6F_isError(LZ6F_createCompressionContext(&cc, LZ6F_VERSION))) __builtin_trap();
    uint8_t hdr[64]; size_t r = LZ6F_compressBegin(cc, hdr, sizeof hdr, &p);
    if (LZ6F_isError(r)) __builtin_trap();
    put(hdr, r);
    size_t chunk = 1 + (data[3] | (size_t)data[4] << 8), off = 0; int k = 0;
    while (off < sn) {
      size_t sz = sn - off < chunk ? sn - off : chunk;
      size_t cap = LZ6F_compressBound(sz, &p); uint8_t* c = malloc(cap);
      r = LZ6F_compressUpdate(cc, c, cap, src + off, sz, NULL);
      if (LZ6F_isError(r)) __builtin_trap();
      put(c, r);
      if (mode == 2 && (++k & 1)) { size_t fc = LZ6F_compressBound(0, &p) + (1u << 20); uint8_t* f = malloc(fc); r = LZ6F_flush(cc, f, fc, NULL); if (LZ6F_isError(r)) __builtin_trap(); put(f, r); free(f); }
      free(c); off += sz;
    }
    size_t fc = LZ6F_compressBound(0, &p) + 64 + (1u << 20); uint8_t* f = malloc(fc);
    r = LZ6F_compressEnd(cc, f, fc, NULL);
    if (LZ6F_isError(r)) __builtin_trap();
    put(f, r); free(f); LZ6F_freeCompressionContext(cc);
  }
  LZ6F_decompressionContext_t dc;
  if (LZ6F_isError(LZ6F_createDecompressionContext(&dc, LZ6F_VERSION))) __builtin_trap();
  uint8_t* out = malloc(sn ? sn : 1); size_t produced = 0, pos = 0, ichunk = 1 + (data[7] | (size_t)data[3] << 8), ret = 1;
  size_t ocap = (size_t)1 << (6 + (data[7] >> 4) % 10);
  while (pos < glen) {
    size_t sz = glen - pos < ichunk ? glen - pos : ichunk, dsz = ocap;
    if (dsz > sn - produced) dsz = sn - produced;
    uint8_t dummy; uint8_t* dp = dsz ? out + produced : &dummy;
    ret = LZ6F_decompress(dc, dp, &dsz, gbuf + pos, &sz, NULL);
    if (LZ6F_isError(ret)) __builtin_trap();
    pos += sz; produced += dsz;
    if (sz == 0 && dsz == 0) __builtin_trap();
  }
  if (ret != 0 || produced != sn || (sn && memcmp(out, src, sn))) __builtin_trap();
  free(out); LZ6F_freeDecompressionContext(dc);
  return 0;
}
