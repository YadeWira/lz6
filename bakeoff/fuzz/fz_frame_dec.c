/* libFuzzer: the streaming frame decoder LZ6F_decompress on arbitrary bytes, with input and output chunk sizes taken from the input. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lz6frame.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
  if (n < 2) return 0;
  LZ6F_decompressionContext_t ctx;
  if (LZ6F_isError(LZ6F_createDecompressionContext(&ctx, LZ6F_VERSION))) return 0;
  uint8_t* e = malloc(n - 1); memcpy(e, data + 1, n - 1);
  const uint8_t* src = e; size_t len = n - 1;
  size_t chunk = (data[0] & 1) ? 1 + (data[0] >> 1) : 1 + (size_t)(data[0] >> 1) * 257;
  size_t outCap = (size_t)1 << (6 + ((data[0] >> 3) & 7) * 2);            /* 64 B .. 1 MB */
  uint8_t* out = malloc(outCap);
  if ((data[0] & 4) && len) {                                                /* sometimes ask for the frame info first */
    LZ6F_frameInfo_t fi; size_t sz = len < 32 ? len : 32;
    size_t r = LZ6F_getFrameInfo(ctx, &fi, src, &sz);
    if (!LZ6F_isError(r)) { src += sz; len -= sz; }
  }
  size_t total = 0;
  while (len) {
    size_t sz = len < chunk ? len : chunk, dsz = outCap;
    size_t r = LZ6F_decompress(ctx, out, &dsz, src, &sz, NULL);
    if (LZ6F_isError(r)) break;
    src += sz; len -= sz; total += dsz;
    if (total > ((size_t)64 << 20)) break;
    if (sz == 0 && dsz == 0) break;
  }
  free(out); free(e); LZ6F_freeDecompressionContext(ctx);
  return 0;
}
