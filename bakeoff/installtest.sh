#!/bin/bash
# installtest.sh : `make -C lib install` into a scratch DESTDIR from a copy of the tree, then check what a user gets: the files, the pkg-config file (name, version,
# libs), and a program that includes every public header and links with what pkg-config says, against the shared and the static library (block, HC, seq at 8 levels,
# and the frame API round trip). Prints one line per check and a final PASS or FAIL; exit status 0 on a pass.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; W="$(mktemp -d "${TMPDIR:-/tmp}/lz6install.XXXXXX")"; ID="$W/inst"; B="$W/tree"; mkdir -p "$ID" "$B"
fail=0; ok() { echo "PASS $1"; }; bad() { echo "FAIL $1"; fail=1; }
(cd "$ROOT" && cp -r lib "$B/lib") && cd "$B/lib" && make -s clean > /dev/null 2>&1
out=$(make -s PREFIX=/usr DESTDIR="$ID" install 2>&1); [ $? -eq 0 ] && ok "make install" || { bad "make install: $out"; exit 1; }
[ -z "$(echo "$out" | grep -E 'warning|error')" ] && ok "build without warnings (the Makefile's -Wall -Wextra -Wundef -Wshadow -pedantic)" || bad "build warnings: $(echo "$out" | grep -E 'warning|error' | head -3)"
for f in usr/include/lz6.h usr/include/lz6hc.h usr/include/lz6frame.h usr/include/lz6seq.h usr/lib/liblz6.a usr/lib/liblz6.so usr/lib/pkgconfig/liblz6.pc; do [ -e "$ID/$f" ] && ok "installed $f" || bad "missing $f"; done
VER=$(sed -n '/define LZ6_VERSION_MAJOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' "$ROOT/lib/lz6.h").$(sed -n '/define LZ6_VERSION_MINOR/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' "$ROOT/lib/lz6.h").$(sed -n '/define LZ6_VERSION_RELEASE/s/.*[[:blank:]]\([0-9][0-9]*\).*/\1/p' "$ROOT/lib/lz6.h")
ar t "$ID/usr/lib/liblz6.a" | grep -q '\.o$' && ! ar t "$ID/usr/lib/liblz6.a" | grep -q '\.c$' && ok "liblz6.a holds object files" || bad "liblz6.a does not hold objects"
readelf -d "$ID"/usr/lib/liblz6.so.* 2>/dev/null | grep -q 'SONAME.*liblz6.so.1' && ok "soname liblz6.so.1" || bad "soname"
export PKG_CONFIG_PATH="$ID/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$ID"
[ "$(pkg-config --modversion liblz6 2>&1)" = "$VER" ] && ok "pkg-config version $VER" || bad "pkg-config version: $(pkg-config --modversion liblz6 2>&1) (want $VER)"
pkg-config --libs liblz6 2>&1 | grep -q -- '-llz6' && ! pkg-config --libs liblz6 2>&1 | grep -q 'lz4' && ok "pkg-config --libs gives -llz6" || bad "pkg-config --libs: $(pkg-config --libs liblz6 2>&1)"
cat > "$W/api.c" <<'EOT'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz6.h"
#include "lz6hc.h"
#include "lz6frame.h"
#include "lz6seq.h"
int main(void) {
  char src[20000]; for (int i = 0; i < 20000; i++) src[i] = (char)("lz6 stable "[i % 11] + (i / 997 % 3));
  char* c = malloc(LZ6_compressBound(20000) + 65536); char* d = malloc(20000); int ok = 1;
  int n = LZ6_compress_default(src, c, 20000, LZ6_compressBound(20000)); ok &= n > 0 && LZ6_decompress_safe(c, d, n, 20000) == 20000 && !memcmp(src, d, 20000);
  n = LZ6_compress_HC(src, c, 20000, LZ6_compressBound(20000), 9); ok &= n > 0 && LZ6_decompress_safe(c, d, n, 20000) == 20000 && !memcmp(src, d, 20000);
  { static const int lv[] = {-2,-1,1,2,3,5,8,13}; for (unsigned i = 0; i < 8; i++) { size_t m = LZ6_compress_seq(src, 20000, c, LZ6_compressBound(20000) + 65536, lv[i]); ok &= m > 0 && LZ6_decompress_seq(c, m, d, 20000) == 20000 && !memcmp(src, d, 20000); } }
  size_t fb = LZ6F_compressFrameBound(20000, NULL); char* f = malloc(fb); size_t fn = LZ6F_compressFrame(f, fb, src, 20000, NULL); ok &= !LZ6F_isError(fn);
  LZ6F_decompressionContext_t dc; LZ6F_createDecompressionContext(&dc, LZ6F_VERSION); size_t dn = 20000, sn = fn; size_t r = LZ6F_decompress(dc, d, &dn, f, &sn, NULL); ok &= !LZ6F_isError(r) && dn == 20000 && !memcmp(src, d, 20000); LZ6F_freeDecompressionContext(dc);
  printf("%s %s\n", ok ? "api-ok" : "api-FAILED", LZ6_VERSION); return !ok; }
EOT
cd "$W"
gcc -Wall -Wextra -O2 api.c $(pkg-config --cflags --libs liblz6) -o api_shared 2>"$W/err.txt" && [ "$(LD_LIBRARY_PATH="$ID/usr/lib" ./api_shared)" = "api-ok v$VER" ] && ok "a program with the four public headers links and runs against the shared library" || bad "shared link/run: $(head -3 "$W/err.txt")"
gcc -Wall -Wextra -O2 api.c $(pkg-config --cflags liblz6) "$ID/usr/lib/liblz6.a" $(pkg-config --libs-only-l --static liblz6 | sed 's/-llz6//') -o api_static 2>"$W/err.txt" && [ "$(./api_static)" = "api-ok v$VER" ] && ok "the same program links and runs against the static library" || bad "static link/run: $(head -3 "$W/err.txt")"
rm -rf "$W"; [ $fail -eq 0 ] && echo "installtest: PASS" || echo "installtest: FAIL"; exit $fail
