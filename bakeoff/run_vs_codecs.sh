#!/bin/sh
# run_vs_codecs.sh - head-to-head of lz6 (seq + HC) against lz4/lz4hc, lz5,
# lizard, zstd, misa77, brotli, xz and zlib inside ONE harness (lzbench), so
# every number comes from the same timing loop and buffer handling.
#
# lzbench ships plugins for the externals; lz6 and lz5 are added by
# lzbench_lz6.patch (codec wrappers + registration + Makefile rules), with our
# sources copied into lz/lz6 and lz/lz5. lz5 v1.5 comes from the repo's
# baseline commit via bakeoff/setup_competitors.sh (cached in /tmp/lz5src).
#
#   bakeoff/run_vs_codecs.sh setup          fetch + patch + build lzbench
#   bakeoff/run_vs_codecs.sh ait            AIT A-H sweep   (~15 min)
#   bakeoff/run_vs_codecs.sh silesia        Silesia sweep   (~35 min)
#   bakeoff/run_vs_codecs.sh bench          both, sequentially
#
# Raw dumps land in /tmp/vs_ait_raw.txt and /tmp/vs_silesia_raw.txt; parse
# them with bakeoff/parse_lzbench.py. Never run two sweeps at once - the
# timings are single-threaded and CPU contention invalidates them.
set -e
cd "$(dirname "$0")/.."
ROOT=$PWD
LB=${LZBENCH:-/tmp/lzbench}
LZ5SRC=${LZ5SRC:-/tmp/lz5src}
SILESIA=${SILESIA:-/mnt/IA_LAB/agentes/FLZMA2/silesia}
AIT=${AIT:-/tmp/ait_train}

setup() {
    [ -d "$LB" ] || git clone --depth 1 https://github.com/inikep/lzbench "$LB"
    [ -d "$LZ5SRC" ] || bakeoff/setup_competitors.sh "$AIT"

    mkdir -p "$LB/lz/lz6/entropy" "$LB/lz/lz5"
    cp lib/*.c lib/*.h "$LB/lz/lz6/"
    cp lib/entropy/*.c lib/entropy/*.h "$LB/lz/lz6/entropy/"
    cp "$LZ5SRC"/lib/*.c "$LZ5SRC"/lib/*.h "$LB/lz/lz5/"
    rm -f "$LB/lz/lz5/Makefile" "$LB"/lz/lz5/*.o

    ( cd "$LB" && git checkout -- . && git apply "$ROOT/bakeoff/lzbench_lz6.patch" )
    ( cd "$LB" && make -j4 >/dev/null )
    echo "lzbench with lz6+lz5 plugins ready -> $LB/lzbench"
}

bench_ait() {
    mkdir -p "$AIT"
    cp -n bakeoff/corpora/[A-H] "$AIT/" 2>/dev/null || true
    ( cd "$LB" && ./lzbench -t1,1 \
        -elz4/lz5/lz5hc,1,6,9,12,15/lz6,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15/lz6hc,6,12,15/lizard,10,20,30,40,45,49/zstd,1,3,5,9,12,15,19/misa77,-1,0,1,2,3,4/brotli,1,4,6,9,11/xz,1,3,6,9/zlib,1,6,9 \
        "$AIT"/[A-H] ) > /tmp/vs_ait_raw.txt 2>&1
    echo "AIT -> /tmp/vs_ait_raw.txt"
}

bench_silesia() {
    [ -d "$SILESIA" ] || { echo "no Silesia corpus at $SILESIA"; exit 1; }
    ( cd "$LB" && ./lzbench -t1,1 \
        -elz4/lz5/lz5hc,1,6,15/lz6,1,2,4,7,8,10,12,13,15/lz6hc,15/lizard,30,49/zstd,1,5,19/misa77,-1,1,3/brotli,1,11/xz,1,9/zlib,1,6 \
        "$SILESIA"/* ) > /tmp/vs_silesia_raw.txt 2>&1
    echo "Silesia -> /tmp/vs_silesia_raw.txt"
}

case "${1:-bench}" in
    setup)   setup ;;
    ait)     bench_ait ;;
    silesia) bench_silesia ;;
    bench)   setup; bench_ait; bench_silesia ;;
    *) echo "usage: run_vs_codecs.sh [setup|ait|silesia|bench]"; exit 1 ;;
esac
