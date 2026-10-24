/* libFuzzer: LZ6_decompress_seq on arbitrary bytes. Source and destination are exact-size heap blocks so ASAN sees any overrun.
 * Invariants: the result never exceeds the capacity, and the output does not depend on how much spare room the caller gave. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lib/entropy/lz6seq.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
  if (n < 5) return 0;
  uint32_t isz = data[1] | data[2] << 8 | data[3] << 16 | (uint32_t)data[4] << 24;
  if (isz > (16u << 20)) isz %= (1u << 20);
  uint8_t* e = malloc(n); memcpy(e, data, n);
  size_t caps[3] = { isz, isz ? isz - 1 : 0, (size_t)isz + 7 };
  uint8_t* first = NULL; size_t firstR = 0;
  for (int v = 0; v < 3; v++) {
    uint8_t* d = malloc(caps[v] ? caps[v] : 1);
    size_t r = LZ6_decompress_seq((const char*)e, n, (char*)d, caps[v]);
    if (r > caps[v]) __builtin_trap();
    if (v == 0) { first = d; firstR = r; }
    else {
      if (v == 2 && firstR && r != firstR) __builtin_trap();                 /* same stream, more room: same result */
      if (v == 2 && firstR && memcmp(first, d, firstR)) __builtin_trap();
      free(d);
    }
  }
  free(first); free(e);
  return 0;
}
