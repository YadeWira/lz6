#!/bin/sh
# bench_seq.sh - measure lz6seq against the AIT training set.
# Usage: bench_seq.sh [level] [dir]
LEVEL=${1:-15}
DIR=${2:-/tmp/ait_train}
total=0
for f in A B C D E F G H; do
    [ -f "$DIR/$f" ] || continue
    out=$(./programs/lz6seqcli c "$DIR/$f" "/tmp/$f.seq" $LEVEL 2>/dev/null)
    sz=$(echo "$out" | sed 's/.*-> *\([0-9]*\) bytes.*/\1/')
    [ -n "$sz" ] && [ "$sz" -gt 0 ] 2>/dev/null || { echo "$f: FAIL"; continue; }
    ./programs/lz6seqcli d "/tmp/$f.seq" "/tmp/$f.orig" >/dev/null 2>&1
    if cmp -s "$DIR/$f" "/tmp/$f.orig"; then
        echo "$f: $sz  rt=OK"
    else
        echo "$f: $sz  rt=FAIL"
    fi
    total=$((total + sz))
done
echo "total=$total  (orig=13136617, saving=$(python3 -c "print(f'{(1-$total/13136617)*100:.1f}%')" 2>/dev/null || echo "?"))"
