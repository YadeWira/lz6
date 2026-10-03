/* xfuzz2 N STREAM...: mutate valid seq streams (bit flips, truncation, random runs, header bytes) and decode each into an exact-size
 * destination from an EXACT-size heap copy of the mutated stream, so ASAN sees reads past the end of the stream as well as writes
 * past the end of the output. Build with -fsanitize=address,undefined; bytes 1..4 of a stream are its output size. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st=0x9E3779B97F4A7C15ull; static uint64_t rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
int main(int argc,char**argv){ long N=atol(argv[1]); long ok=0;
 for(long it=0;it<N;it++){ const char* fn=argv[2+rnd()%(argc-2)]; FILE*f=fopen(fn,"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  uint8_t* b=malloc(n); if(fread(b,1,n,f)!=(size_t)n) return 1; fclose(f);
  int kind=rnd()%4; long len=n;
  if(kind==0){ int k=1+rnd()%8; for(int j=0;j<k;j++) b[rnd()%n]^=1u<<(rnd()%8); }
  else if(kind==1){ len=rnd()%n; }
  else if(kind==2){ long p=rnd()%n; long l=1+rnd()%64; for(long j=p;j<p+l&&j<n;j++) b[j]=rnd(); }
  else { b[5+rnd()%(n>64?64:n-5)]^=rnd(); }  /* header region */
  uint32_t isz = n>=5 ? (b[1]|b[2]<<8|b[3]<<16|(uint32_t)b[4]<<24) : 0; if(isz>(64u<<20)) isz=rnd()%(1<<20);
  uint8_t* d=malloc(isz?isz:1);
  uint8_t* e=malloc(len?len:1); memcpy(e,b,len);   /* exact-size source: ASAN sees reads past the stream end */
  size_t r=LZ6_decompress_seq((const char*)e,len,(char*)d,isz); if(r) ok++;
  free(e); free(d); free(b); }
 printf("xfuzz: %ld iters, %ld decoded, no crash\n",N,ok); return 0; }
