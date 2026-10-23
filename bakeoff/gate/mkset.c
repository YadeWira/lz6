/* mkset DIR : synthetic small/medium streams (several patterns, sizes, levels) for the mutation fuzzer */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st=0x1234567887654321ull; static uint64_t rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
int main(int argc, char** argv){
  int idx = 0; static const int lv[] = {-2,1,2,4,5,7,9,10,12};   /* the fast tier and levels 1, 2, 4, 5, 7, 9, 10, 12 */
  static const size_t sz[] = {19, 40, 100, 333, 1000, 3000, 20000, 100000};
  for (unsigned si = 0; si < sizeof sz/sizeof *sz; si++) for (int k = 0; k < 6; k++) for (unsigned li = 0; li < sizeof lv / sizeof *lv; li++) {
    size_t n = sz[si]; uint8_t* b = malloc(n);
    for (size_t i = 0; i < n; i++) switch (k) {
      case 0: b[i] = "the quick brown fox jumps over the lazy dog "[(i + (rnd()%7==0)) % 44]; break;
      case 1: b[i] = (i % 97 < 90) ? "abcdefgh"[i % 8] : (uint8_t)rnd(); break;
      case 2: b[i] = (uint8_t)(rnd() % 6); break;
      case 3: b[i] = (i % 311 == 0) ? (uint8_t)rnd() : 'x'; break;
      case 4: b[i] = (i < n/3) ? (uint8_t)rnd() : b[i - n/3]; break;
      default: b[i] = (uint8_t)((i * 7) ^ (i >> 3)); break;
    }
    size_t cap = n + n/2 + 1024; char* c = malloc(cap);
    size_t cs = LZ6_compress_seq((const char*)b, n, c, cap, lv[li]);
    if (cs && ((uint8_t)c[0] & 0x1f) == 0) {
      char fn[512]; snprintf(fn, sizeof fn, "%s/s%03d.seq", argv[1], idx++);
      FILE* f = fopen(fn, "wb"); fwrite(c, 1, cs, f); fclose(f);
    }
    free(b); free(c);
  }
  printf("%d streams\n", idx); return 0; }
