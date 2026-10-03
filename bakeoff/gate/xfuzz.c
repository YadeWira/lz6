/* xfuzz N STREAM...: mutate valid seq streams and decode each into an exact-size destination (ASAN). The source is the mutated
 * buffer itself, so a truncated stream still sits inside a larger allocation and source over-reads are invisible here:
 * xfuzz2.c decodes from an exact-size copy and is the check that sees them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "lib/entropy/lz6seq.h"
static uint64_t st=88172645463325252ull; static uint64_t rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
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
  size_t r=LZ6_decompress_seq((const char*)b,len,(char*)d,isz); if(r) ok++;
  free(d); free(b); }
 printf("xfuzz: %ld iters, %ld decoded, no crash\n",N,ok); return 0; }
