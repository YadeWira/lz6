/* mkseq LEVEL IN OUT : write the raw seq stream, plus the size in OUT.size (xfuzz reads b[1..4] as the original size) */
#include <stdio.h>
#include <stdlib.h>
#include "lib/entropy/lz6seq.h"
int main(int argc, char** argv){
  FILE* f = fopen(argv[2], "rb"); fseek(f,0,SEEK_END); size_t n = ftell(f); fseek(f,0,SEEK_SET);
  char* s = malloc(n); if(fread(s,1,n,f)!=n) return 2; fclose(f);
  size_t cap = n + n/2 + 65536; char* c = malloc(cap);
  size_t cs = LZ6_compress_seq(s, n, c, cap, atoi(argv[1])); if(!cs) return 3;
  f = fopen(argv[3], "wb"); fwrite(c,1,cs,f); fclose(f); return 0; }
