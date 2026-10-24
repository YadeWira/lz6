/* mkblock LEVEL IN OUT: a raw block (LEVEL 0: LZ6_compress_default, else LZ6_compress_HC) behind the 2-byte capacity prefix fz_block_dec reads */
#include <stdio.h>
#include <stdlib.h>
#include "lz6.h"
#include "lz6hc.h"
int main(int argc, char** argv) {
  int lvl = atoi(argv[1]); FILE* f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); int n = (int)ftell(f); fseek(f, 0, SEEK_SET);
  char* s = malloc(n); if (fread(s, 1, n, f) != (size_t)n) return 2; fclose(f);
  int cap = LZ6_compressBound(n); char* c = malloc(cap + 2);
  int cs = lvl ? LZ6_compress_HC(s, c + 2, n, cap, lvl) : LZ6_compress_default(s, c + 2, n, cap); if (cs <= 0) return 3;
  int units = (n + 7) / 8; c[0] = units & 255; c[1] = units >> 8;
  f = fopen(argv[3], "wb"); fwrite(c, 1, cs + 2, f); fclose(f); return 0; }
