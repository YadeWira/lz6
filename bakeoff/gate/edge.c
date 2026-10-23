/* edge.c: synthetic edge-case round trips (tiny sizes, runs, repeats, random) at several levels, exact-size destination */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st=0x9E3779B97F4A7C15ull; static uint64_t rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
static int rt(const uint8_t* s, size_t n, int lvl, const char* what){
  size_t cap = n + n/2 + 1024; char* c = malloc(cap);
  size_t cs = LZ6_compress_seq((const char*)s, n, c, cap, lvl);
  if (!cs) { fprintf(stderr, "compress failed %s n=%zu L%d\n", what, n, lvl); return 1; }
  char* d = malloc(n ? n : 1);   /* exact size: ASAN sees every overrun */
  size_t r = LZ6_decompress_seq(c, cs, d, n);
  int bad = (r != n) || (n && memcmp(d, s, n));
  if (bad) fprintf(stderr, "ROUND TRIP FAIL %s n=%zu L%d r=%zu cs=%zu\n", what, n, lvl, r, cs);
  free(c); free(d); return bad;
}
int main(int argc, char** argv){
  int fails = 0; long cases = 0;
  static const int lv[] = {-2,-1,1,2,3,4,5,6,7,9,10,11,12,13};   /* the fast tier and levels 1-7, 9-13 (v1.6.13 changed the parsers of 2-7, v1.6.14 the tree of 9-13) */
  for (size_t n = 0; n <= 300; n++) for (int k = 0; k < 4; k++) {
    uint8_t* b = malloc(n + 1);
    for (size_t i = 0; i < n; i++) b[i] = k==0 ? (uint8_t)rnd() : k==1 ? 'a' : k==2 ? "abcabcabd"[i%9] : (uint8_t)(rnd()%3);
    for (unsigned li = 0; li < sizeof lv/sizeof *lv; li++) { fails += rt(b, n, lv[li], "tiny"); cases++; }
    free(b);
  }
  size_t big[] = {1000, 4095, 4096, 4097, 20000, 65535, 65536, 100000, 300000, 1000000};
  for (unsigned bi = 0; bi < sizeof big/sizeof *big; bi++) for (int k = 0; k < 8; k++) {
    size_t n = big[bi]; uint8_t* b = malloc(n);
    for (size_t i = 0; i < n; i++) {
      switch (k) {
        case 0: b[i] = (uint8_t)rnd(); break;                      /* incompressible */
        case 1: b[i] = 0; break;                                   /* one long run */
        case 2: b[i] = (uint8_t)(i % 251); break;                  /* short period */
        case 3: b[i] = (uint8_t)((i * i) >> 7); break;
        case 4: b[i] = (i % 5000 < 4900) ? (uint8_t)rnd() : 7; break;        /* long literal runs with short matches */
        case 5: b[i] = (i < n/2) ? (uint8_t)rnd() : b[i - n/2]; break;      /* far match (offset n/2) */
        case 6: b[i] = (i % 1000 == 0) ? (uint8_t)rnd() : "the quick brown fox"[i % 19]; break;
        default: b[i] = (uint8_t)(rnd() % 4 + (i/37)%3); break;
      }
    }
    for (unsigned li = 0; li < sizeof lv/sizeof *lv; li++) { fails += rt(b, n, lv[li], "big"); cases++; }
    free(b);
  }
  printf("edge: %ld cases, %d failures\n", cases, fails);
  return fails != 0;
}
