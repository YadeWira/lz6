#!/bin/bash
# mk.sh - rebuilds the whole vector set (vectors/, MANIFEST.tsv) from the sources and the test data.
#   bash mk.sh [WORK]
# WORK (default ${TMPDIR:-/tmp}/lz6-seqvec) receives the binaries and the libraries of v1.7.1 and v2.0.0
# (git archive). Nothing is built inside the repository. The result is deterministic: the same sources,
# compiler target (64-bit) and test data give the same bytes; check with `git status` / sha256 in MANIFEST.tsv.
# Then run verify.sh.
# Test data (override with the environment):
#   SIL  Silesia corpus (dickens, xml, sao, ooffice, mr)
#   AIT  ait_train (E, G)
#   AU   mixed inputs (dk4_xr4, rnd64k_aitA, T1_oo)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WORK=${1:-${TMPDIR:-/tmp}/lz6-seqvec}
SIL=${SIL:-/mnt/IA_LAB/agentes/lz6/silesia}
AIT=${AIT:-/mnt/IA_LAB/agentes/lz6/tmp_backup/ait_train}
AU=${AU:-/mnt/IA_LAB/agentes/lz6/v21/wf/audit2}
RUN=${RUN:-taskset -c 0-7}
V=$HERE/vectors
mkdir -p "$WORK" "$V"

# ---- build ----
for v in v1.7.1 v2.0.0; do
    if [ ! -d "$WORK/lib_$v" ]; then mkdir "$WORK/lib_$v"; git -C "$ROOT" archive $v lib | tar -x -C "$WORK/lib_$v"; fi
done
libs() { echo "$1/lz6.c $1/lz6hc.c $1/lz6frame.c $1/xxhash.c $1/entropy/lz6fse.c $1/entropy/lz6huf.c $1/entropy/lz6seq.c"; }
cc() { local out=$1 lib=$2; shift 2; gcc -O2 -w -I"$lib" -I"$lib/entropy" "$@" $(libs "$lib") -lm -lpthread -o "$WORK/$out"; }
cc encvec    "$ROOT/lib" "$HERE/encvec.c" &
cc mkvec     "$ROOT/lib" "$HERE/mkvec.c" &
cc decchk    "$ROOT/lib" "$HERE/decchk.c" &
cc decchk200 "$WORK/lib_v2.0.0/lib" "$HERE/decchk.c" &
cc decchk171 "$WORK/lib_v1.7.1/lib" "$HERE/decchk.c" &
cc encvec171 "$WORK/lib_v1.7.1/lib" -DSEQVEC_NO_OPT "$HERE/encvec.c" &
gcc -O2 -Wall -Wextra -o "$WORK/inspect" "$HERE/inspect.c" &
wait
for b in encvec mkvec decchk decchk200 decchk171 encvec171 inspect; do [ -x "$WORK/$b" ] || { echo "build of $b failed"; exit 1; }; done

# ---- vectors ----
HOW=$WORK/how.tsv
: > "$HOW"
# hand-assembled (a_*)
$RUN "$WORK/mkvec" "$V" | sed 's/\t/\tassembler: /' >> "$HOW"
# real encoder (e_*): name input offset length level blockMask litMask
enc() {
    $RUN "$WORK/encvec" "$2" $3 $4 $5 $6 $7 "$V/$1.seq" "$V/$1.orig"
    printf '%s\tLZ6_compress_seq_opt level %s blockMask %s litMask %s; input %s' "$1" $5 $6 $7 "$(basename "$2")" >> "$HOW"
    case "$2" in *:*) printf ', %s bytes\n' $4 >> "$HOW";; *) printf ' bytes %s..+%s\n' $3 $4 >> "$HOW";; esac
}
# v1.7.1 encoder, format 1 (f1_*): name input offset length level
enc171() {
    $RUN "$WORK/encvec171" "$2" $3 $4 $5 0 0 "$V/$1.seq" "$V/$1.orig"
    printf '%s\tv1.7.1 LZ6_compress_seq level %s; input %s' "$1" $5 "$(basename "$2")" >> "$HOW"
    case "$2" in *:*) printf ', %s bytes\n' $4 >> "$HOW";; *) printf ' bytes %s..+%s\n' $3 $4 >> "$HOW";; esac
}
ALL=0x0F
LALL=0x1EA   # LZ6SEQ_LIT_ALL: modes 1, 3, 5, 6, 7, 8
# literal modes, no block kinds
enc e_nomask         $SIL/dickens 100000 65536 5 0 0
enc e_lit5           $SIL/dickens 100000 65536 5 0 0x020
enc e_lit7           $SIL/dickens 100000 65536 5 0 0x080
enc e_lit1           $SIL/dickens 100000 65536 5 0 0x002
enc e_lit6           markov16:5:64:14 0 100000 9 0 0x040
enc e_lit3           markov16:5:32:8  0 100000 9 0 0x008
enc e_lit8_m7        $SIL/sao 0 100000 5 0 0x180
enc e_lit8_m5        $SIL/sao 0 100000 5 0 0x120
enc e_lit8_m1        $AU/dk4_xr4 4144304 100000 5 0 0x102
enc e_lit8_m0m1      $AU/rnd64k_aitA 0 100000 1 0 0x102
# everything allowed (= LZ6_compress_seq), several levels
enc e_all_fast2      $SIL/dickens 100000 65536 -2 $ALL $LALL
enc e_all_fast1      $SIL/dickens 100000 65536 -1 $ALL $LALL
enc e_all_l1         $SIL/dickens 100000 65536 1 $ALL $LALL
enc e_all_l13        $SIL/xml 0 131072 13 $ALL $LALL
enc e_all_small      $SIL/xml 0 20000 5 $ALL $LALL
enc e_epochs_repeat  markov:5:64:4 0 100000 9 0 0x008
# x86
enc e_x86            $SIL/ooffice 1048576 65536 5 0x01 $LALL
enc e_x86_lit8       $AU/T1_oo 950000 196608 5 0x01 $LALL
# plane blocks
enc e_plane2_delta   walk16:1 0 60000 5 $ALL $LALL
enc e_plane2         walk16:1 0 60000 5 0x02 $LALL
enc e_plane2_lit8    $SIL/mr 2000000 131072 5 $ALL $LALL
enc e_plane2_G       $AIT/G 0 100000 5 $ALL $LALL
enc e_plane4_delta   walk32:1 0 100000 5 $ALL $LALL
enc e_plane4         walk32:1 0 100000 5 0x02 $LALL
enc e_plane4_mr      $SIL/mr 2000000 100000 5 $ALL $LALL
enc e_plane4_huf     $AIT/E 0 100000 5 $ALL $LALL
enc e_plane4_E       $AIT/E 0 100000 9 $ALL $LALL
# PRNG and stored
enc e_prng_s0        prng:12345:0 0 20000 5 $ALL $LALL
enc e_prng_s8        prng:777:8 0 10000 5 $ALL $LALL
enc e_prng_s16       prng:65535:16 0 30000 5 $ALL $LALL
enc e_prng_masked    prng:12345:0 0 20000 5 0x07 $LALL
enc e_stored         lcg:1 0 4096 5 $ALL $LALL
# format 1, written by v1.7.1
enc171 f1_lit5         $SIL/xml 0 20000 5
enc171 f1_lit7         $SIL/dickens 100000 65536 5
enc171 f1_lit7_fast    $SIL/dickens 100000 65536 -1
enc171 f1_lit6         markov16:5:64:14 0 100000 9
enc171 f1_x86          $SIL/ooffice 1048576 65536 5
enc171 f1_plane4_w16   walk16:1 0 60000 5
enc171 f1_plane2_w16   walk16:1 0 60002 5
enc171 f1_plane4_w32   walk32:1 0 100000 5
enc171 f1_plane4_huf   $AIT/E 0 100000 5
enc171 f1_prng         prng:777:8 0 10000 5
enc171 f1_stored       lcg:1 0 4096 5

# ---- manifest ----
"$WORK/mkvec" -big "$WORK/a_big_offsets.orig"
M=$HERE/MANIFEST.tsv
printf 'name\tseq_bytes\torig_bytes\tseq_sha256\torig_sha256\tmade_by\tcontents\n' > "$M"
while IFS=$'\t' read -r name how; do
    s=$V/$name.seq; o=$V/$name.orig; note=
    [ -f "$o" ] || { o=$WORK/$name.orig; note=' (original not stored)'; }
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "$(stat -c %s "$s")" "$(stat -c %s "$o")$note" \
        "$(sha256sum < "$s" | cut -d' ' -f1)" "$(sha256sum < "$o" | cut -d' ' -f1)" "$how" "$("$WORK/inspect" -s "$s")" >> "$M"
done < "$HOW"
# files of vectors that are no longer in the list
for f in "$V"/*.seq "$V"/*.orig; do
    n=$(basename "$f"); n=${n%.*}
    grep -q "^$n	" "$HOW" || { echo "removing stale $f"; rm -f -- "$f"; }
done
echo "$(($(wc -l < "$M") - 1)) vectors, $(du -sh "$HERE" | cut -f1) in $HERE"
