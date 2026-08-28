#!/bin/sh
# bench_seq_speed.sh - build bakeoff/bench_seq.c and run it over the AIT training set.
# Usage: bench_seq_speed.sh [--level N] [--fuzz N] [--asan] [--save FILE] [dir]
#
# Baseline/delta workflow:
#   ./bakeoff/bench_seq_speed.sh --save bakeoff/SEQ_SPEED_BASELINE.txt   (once)
#   ./bakeoff/bench_seq_speed.sh                                         (after changes)
set -e
cd "$(dirname "$0")/.."

LEVEL=2
FUZZ=0
ASAN=""
SAVE=""
DIR=/tmp/ait_train
while [ $# -gt 0 ]; do
    case "$1" in
        --level) LEVEL=$2; shift 2 ;;
        --fuzz)  FUZZ=$2; shift 2 ;;
        --asan)  ASAN="-fsanitize=address -g"; shift ;;
        --save)  SAVE=$2; shift 2 ;;
        *)       DIR=$1; shift ;;
    esac
done

# lib objects at -O2 (the seq codec miscompiles under -O3; see programs/Makefile)
make -C programs lz6 >/dev/null 2>&1

BIN=/tmp/lz6_bench_seq.$$
trap 'rm -f "$BIN"' EXIT
cc -O2 -std=c99 -Wall -I. $ASAN bakeoff/bench_seq.c \
    lib/lz6.o lib/lz6hc.o lib/entropy/lz6fse.o lib/entropy/lz6rc.o \
    lib/entropy/lz6seq.o lib/entropy/lz6huf.o -lm -o "$BIN"

FILES=""
for f in A B C D E F G H; do
    [ -f "$DIR/$f" ] && FILES="$FILES $DIR/$f"
done
[ -n "$FILES" ] || { echo "no input files in $DIR (A..H expected)"; exit 1; }

OUT=$(mktemp /tmp/lz6_bench_seq_out.XXXXXX)
trap 'rm -f "$BIN" "$OUT"' EXIT

if [ "$FUZZ" -gt 0 ]; then
    # shellcheck disable=SC2086
    "$BIN" --fuzz "$FUZZ" $FILES | tee "$OUT"
else
    # shellcheck disable=SC2086
    "$BIN" --level "$LEVEL" $FILES | tee "$OUT"
fi

if [ -n "$SAVE" ]; then
    {
        echo "# SEQ_SPEED_BASELINE  level=$LEVEL  date=$(date -u +%Y-%m-%dT%H:%M:%SZ)  rev=$(git rev-parse --short HEAD)"
        cat "$OUT"
    } > "$SAVE"
    echo "saved -> $SAVE"
fi
