#!/bin/sh
# build_windows.sh - cross-compile the lz6 CLI for win64/win32 with mingw.
# Mirrors programs/Makefile flags: -O3 for LZ/frame/CLI, -O2 for the seq
# codec (miscompiles under -O3). Static: no runtime DLLs needed on Win7+.
# Usage: build_windows.sh outdir
set -e
cd "$(dirname "$0")"
OUT=${1:-/tmp/lz6rel}
mkdir -p "$OUT"

build_arch() {
    CC=$1
    TAG=$2
    BASE="-std=gnu99 -Ilib -Iprograms -DXXH_NAMESPACE=LZ6_ -D_WIN32_WINNT=0x0601 -DWINVER=0x0601"
    OBJS=""
    for f in lz6 lz6hc lz6frame; do
        $CC $BASE -O3 -c lib/$f.c -o "$OUT/${f}_${TAG}.o"
        OBJS="$OBJS $OUT/${f}_${TAG}.o"
    done
    $CC $BASE -O3 -c lib/xxhash.c -o "$OUT/xxhash_${TAG}.o"
    OBJS="$OBJS $OUT/xxhash_${TAG}.o"
    for f in lz6fse lz6rc lz6seq lz6huf; do
        $CC $BASE -O2 -c lib/entropy/$f.c -o "$OUT/${f}_${TAG}.o"
        OBJS="$OBJS $OUT/${f}_${TAG}.o"
    done
    for f in bench lz6io lz6cli; do
        $CC $BASE -O3 -c programs/$f.c -o "$OUT/${f}_${TAG}.o"
        OBJS="$OBJS $OUT/${f}_${TAG}.o"
    done
    $CC $OBJS -o "$OUT/lz6_win$TAG.exe" -s -static -static-libgcc -lm
    echo "built $OUT/lz6_win$TAG.exe"
}

build_arch x86_64-w64-mingw32-gcc 64
build_arch i686-w64-mingw32-gcc 32
