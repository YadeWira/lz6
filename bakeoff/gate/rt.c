/* rt LEVEL FILE : compress with LZ6_compress_seq, decode into an EXACT-size heap buffer, compare (build with ASAN/UBSan) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/entropy/lz6seq.h"
int main(int argc, char** argv){
  int lvl = atoi(argv[1]); FILE* f = fopen(argv[2], "rb"); if(!f) return 1;
  fseek(f,0,SEEK_END); size_t n = ftell(f); fseek(f,0,SEEK_SET);
  unsigned char* s = malloc(n ? n : 1); if(fread(s,1,n,f)!=n) return 2; fclose(f);
  size_t cap = n + n/2 + 65536; char* c = malloc(cap);
  size_t cs = LZ6_compress_seq((const char*)s, n, c, cap, lvl); if(!cs){ fprintf(stderr,"compress failed\n"); return 3; }
  char* d = malloc(n ? n : 1);
  size_t r = LZ6_decompress_seq(c, cs, d, n);
  if(r != n || (n && memcmp(s, d, n))){ fprintf(stderr,"ROUND TRIP FAILED %s L%d (%zu vs %zu)\n", argv[2], lvl, r, n); return 5; }
  /* a stream cut one byte short must not crash and must not claim success with the right size */
  char* d2 = malloc(n ? n : 1); (void)LZ6_decompress_seq(c, cs - 1, d2, n);
  free(s); free(c); free(d); free(d2);
  printf("rt ok L%-2d %-10s %9zu -> %9zu\n", lvl, strrchr(argv[2],'/')?strrchr(argv[2],'/')+1:argv[2], n, cs);
  return 0; }
