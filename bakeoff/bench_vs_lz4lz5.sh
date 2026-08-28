#!/bin/sh
# bench_vs_lz4lz5.sh - lz4 vs lz5 vs lz6 head-to-head on the AIT training set.
#
# All frame codecs are benchmarked with their own built-in -b (the same
# TIMELOOP/XXH-verified mem-to-mem machinery — the three CLIs share lineage),
# single-threaded (lz4 v1.10 defaults to multithread, forced -T1). The lz6
# seq codec has no -b path; it uses bakeoff/bench_seq.c (mem-to-mem, output-
# size-corrected). LZ5 v1.5 is built from the repo's baseline commit
# a8a4ac0 ("baseline: LZ5 v1.5 (inikep, 2016) as-is").
#
# Usage: bench_vs_lz4lz5.sh [dir]    (default /tmp/ait_train)
set -e
cd "$(dirname "$0")/.."

DIR=${1:-/tmp/ait_train}
FILES=""
for f in A B C D E F G H; do [ -f "$DIR/$f" ] && FILES="$FILES $DIR/$f"; done
[ -n "$FILES" ] || { echo "no input files in $DIR (A..H expected)"; exit 1; }

LZ4=$(command -v lz4 || echo /usr/bin/lz4)

# LZ5 v1.5 from the baseline commit (cached in /tmp)
LZ5=/tmp/lz5src/programs/lz5
if [ ! -x "$LZ5" ]; then
    rm -rf /tmp/lz5src && mkdir -p /tmp/lz5src
    git archive a8a4ac0 | tar -x -C /tmp/lz5src
    make -C /tmp/lz5src/programs lz5 >/dev/null 2>&1
fi

make -C programs lz6 >/dev/null 2>&1
LZ6=$PWD/programs/lz6

# bench_seq binary at -O2 (same object set as programs/Makefile's lz6seq_test)
BS=/tmp/lz6_bench_seq_vs.$$
make -C programs lz6 >/dev/null 2>&1
cc -O2 -std=c99 -Wall -I. bakeoff/bench_seq.c \
    lib/lz6.o lib/lz6hc.o lib/entropy/lz6fse.o lib/entropy/lz6rc.o \
    lib/entropy/lz6seq.o lib/entropy/lz6huf.o -lm -o "$BS"
trap 'rm -f "$BS"' EXIT

# sum per-file "-b" lines: name : orig -> csize (pct%), enc MB/s , dec MB/s
# (progress redraws use \r; after tr, iteration fragments repeat per file —
# keep only the last complete line per file name, then size-weight the means)
sum_lines() {
    tr '\r' '\n' | awk '
        /: *[0-9]+ +-> *[0-9]+ \([0-9.]+%\), *[0-9.]+ MB\/s *, *[0-9.]+ MB\/s/ {
            name = $1
            if (name ~ /^[0-9]+-/) next   # iteration snapshot (1-A, 2-A, ...)
            if (name ~ /^[0-9]+$/ || name == "TOTAL") next   # aggregate lines
            for (i = 1; i <= NF; i++) {
                if ($i == "->") { O[name] = $(i - 1); C[name] = $(i + 1) }
                if ($i == "MB/s" && E[name] == "") E[name] = $(i - 1)
                if ($i == "MB/s" && E[name] != "" && $(i - 1) ~ /^[0-9.]+$/) D[name] = $(i - 1)
            }
        }
        END {
            for (k in O) {
                to += O[k]; tc += C[k]; te += O[k] / E[k]; td += O[k] / D[k]; n++
            }
            if (n) printf "files=%d orig=%d csize=%d (%.2f%%) enc=%.1f MB/s dec=%.1f MB/s\n", \
                n, to, tc, 100 * tc / to, to / te, to / td
        }'
}

# lz4 multi-file aggregate: "N files : O -> C (F), E MB/s, D MB/s"
sum_lz4() {
    tr '\r' '\n' |
    grep -oE '[0-9]+ files *: *[0-9]+ +-> *[0-9]+ \([0-9.]+\), *[0-9.]+ MB/s, *[0-9.]+ MB/s' |
    tail -1 | awk '{
        for (i = 1; i <= NF; i++) {
            if ($i == "->") { o = $(i - 1); c = $(i + 1) }
            if ($i == "MB/s,") e = $(i - 1)
            if ($i == "MB/s" && e != "") d = $(i - 1)
        }
        printf "orig=%s csize=%s (%.2f%%) enc=%s MB/s dec=%s MB/s\n", o, c, 100 * c / o, e, d
    }'
}

echo "== lz4 default (fast, -T1)"
$LZ4 -T1 -b $FILES 2>&1 | sum_lz4
echo "== lz4 -12 (HC, -T1)"
$LZ4 -T1 -12 -b $FILES 2>&1 | sum_lz4
echo "== lz5 v1.5 default"
$LZ5 -b $FILES 2>&1 | sum_lines
echo "== lz5 v1.5 -15"
$LZ5 -15 -b $FILES 2>&1 | sum_lines
echo "== lz6 frame default"
$LZ6 -b $FILES 2>&1 | sum_lines
echo "== lz6 frame -15"
$LZ6 -15 -b $FILES 2>&1 | sum_lines
echo "== lz6 --seq L2 (bench_seq)"
"$BS" --level 2 $FILES | grep -E "^TOTAL"
echo "== lz6 --seq L15 (bench_seq)"
"$BS" --level 15 $FILES | grep -E "^TOTAL"
