/* libFuzzer: LZ6_compress_seq then LZ6_decompress_seq must give back the input, at a level chosen by the first byte. Also: the documented
 * capacity (bound + 65536) always suffices, and a too-small capacity returns 0 or fits without overrunning. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "lz6.h"
#include "lib/entropy/lz6seq.h"
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t n) {
  static const int lv[16] = { -2, -1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11, 12, 13 };
  if (n < 1) return 0;
  int idx = data[0] & 15; if (idx > 11 && (data[0] >> 4) != 15) idx %= 12;   /* the slow levels only now and then */
  int level = lv[idx];
  const uint8_t* src = data + 1; size_t sn = n - 1;
  if (level >= 8 && sn > 6000) sn = 6000;                                    /* keep the slow levels fast enough to fuzz */
  uint8_t* s = malloc(sn ? sn : 1); memcpy(s, src, sn);
  size_t cap = LZ6_COMPRESSBOUND(sn) + 65536;
  char* c = malloc(cap);
  size_t cs = LZ6_compress_seq((const char*)s, sn, c, cap, level);
  if (!cs || cs > cap) __builtin_trap();
  char* d = malloc(sn ? sn : 1);
  size_t r = LZ6_decompress_seq(c, cs, d, sn);
  if (r != sn || (sn && memcmp(s, d, sn))) __builtin_trap();
  size_t small = cs / 2; char* c2 = malloc(small ? small : 1);
  size_t cs2 = LZ6_compress_seq((const char*)s, sn, c2, small, level);
  if (cs2 > small) __builtin_trap();
  free(c2); free(d); free(c); free(s);
  return 0;
}
