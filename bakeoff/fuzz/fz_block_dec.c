/* libFuzzer: LZ6_decompress_safe (the plain block format) on arbitrary bytes; the first two bytes pick the output capacity. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lz6.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
  if (n < 2) return 0;
  int cap = (data[0] | data[1] << 8) * 8;
  char* src = malloc(n - 2); if (n > 2) memcpy(src, data + 2, n - 2);   /* exact size, even 0 */
  char* d = malloc(cap ? cap : 1);
  int r = LZ6_decompress_safe(src, d, (int)(n - 2), cap);
  if (r > cap) __builtin_trap();
  free(d); free(src);
  return 0;
}
