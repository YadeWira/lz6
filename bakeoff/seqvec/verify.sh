#!/bin/bash
# verify.sh - decodes every vector with three or four decoders and writes VERIFY.tsv.
#   bash verify.sh [WORK] [REFDIR]
# WORK is the directory mk.sh built into (default ${TMPDIR:-/tmp}/lz6-seqvec). Columns of VERIFY.tsv:
#   worktree  LZ6_decompress_seq of the working tree
#   v2.0.0    the library of tag v2.0.0 (every vector: format 2 has no feature newer than 2.0.0, and 2.0.0 reads format 1)
#   v1.7.1    the library of tag v1.7.1, format 1 vectors (f1_*) only
#   pyref     zpaq-std's Python reference decoder, when REFDIR (a directory with a copy of lz6seq_ref.py) is given;
#             a vector it does not accept in its default (strict) mode is tried again with --lenient
# The original of a_big_offsets is regenerated into WORK. Exit status 1 when any decoder disagrees.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-${TMPDIR:-/tmp}/lz6-seqvec}
REF=${2:-}
RUN=${RUN:-taskset -c 0-7}
V=$HERE/vectors
OUT=$HERE/VERIFY.tsv
[ -f "$WORK/a_big_offsets.orig" ] || "$WORK/mkvec" -big "$WORK/a_big_offsets.orig"
one() {   # one vector -> one line
    local name=$1 s=$V/$1.seq o=$V/$1.orig a b c=- p=-
    [ -f "$o" ] || o=$WORK/$name.orig
    a=$("$WORK/decchk" "$s" "$o" 2>&1)
    b=$("$WORK/decchk200" "$s" "$o" 2>&1)
    case $name in f1_*) c=$("$WORK/decchk171" "$s" "$o" 2>&1);; esac
    if [ -n "$REF" ]; then
        p=$(python3 "$HERE/pyref.py" "$REF" "$s" "$o" | cut -f2)
        [ "$p" = OK ] || p="strict: $p; lenient: $(python3 "$HERE/pyref.py" "$REF" --lenient "$s" "$o" | cut -f2)"
    fi
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$a" "$b" "$c" "$p"
}
export -f one
export HERE WORK REF V
printf 'name\tworktree\tv2.0.0\tv1.7.1\tpyref\n' > "$OUT"
tail -n +2 "$HERE/MANIFEST.tsv" | cut -f1 | $RUN xargs -P 8 -I{} bash -c 'one {}' | sort >> "$OUT"
n=$(($(wc -l < "$OUT") - 1))
bad=$(tail -n +2 "$OUT" | awk -F'\t' '{ ok = 1; for (i = 2; i <= 5; i++) if ($i != "-" && $i !~ /^OK/) ok = 0; if (!ok) print }')
echo "$n vectors verified"
if [ -n "$bad" ]; then echo "DISAGREEMENTS:"; echo "$bad"; exit 1; fi
echo "all decoders agree"
