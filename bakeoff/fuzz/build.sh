#!/bin/bash
# build.sh [OUTDIR]: build the libFuzzer harnesses (clang, ASAN+UBSan) and the seed tools into OUTDIR (default: ./build).
# Needs clang with the libFuzzer runtime. Run a target, for example:
#   ./build/fz_seq_dec CORPUS_DIR -max_len=70000 -rss_limit_mb=3500 -max_total_time=600
# Replay the regression inputs (each used to crash a build before the fix named in NEWS):
#   for f in regress/block_*.bin; do ./build/fz_block_dec $f; done
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; OUT="${1:-$HERE/build}"; mkdir -p "$OUT"
LIB="$ROOT/lib/lz6.c $ROOT/lib/lz6hc.c $ROOT/lib/lz6frame.c $ROOT/lib/xxhash.c $ROOT/lib/entropy/lz6fse.c $ROOT/lib/entropy/lz6rc.c $ROOT/lib/entropy/lz6seq.c $ROOT/lib/entropy/lz6huf.c"
INC="-I$ROOT -I$ROOT/lib -I$ROOT/lib/entropy"
for t in fz_seq_dec fz_block_dec fz_frame_dec fz_seq_rt fz_frame_rt; do
  clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -DXXH_NAMESPACE=LZ6_ $INC -pthread "$HERE/$t.c" $LIB -o "$OUT/$t" -lm
done
clang -O2 -DXXH_NAMESPACE=LZ6_ $INC "$ROOT/bakeoff/gate/mkseq.c" $LIB -o "$OUT/mkseq" -lm
clang -O2 -DXXH_NAMESPACE=LZ6_ $INC "$HERE/mkblock.c" $LIB -o "$OUT/mkblock" -lm
echo "built in $OUT"
