#!/usr/bin/env bash
# seqgate.sh - standing safety gate for the seq decoder (lib/entropy/lz6seq.c, lz6huf.c).
# Usage:  bakeoff/seqgate.sh [REFTREE]
#
# Builds from the repo this script lives in and decodes with an exact-size
# SOURCE and an exact-size DESTINATION, so ASAN sees any read past the end
# of the stream and any write past the end of the output (the CLI and a
# larger malloc hide both). Every check runs on both decoder clones: the
# default build (the BMI2 body on a CPU that has BMI2) and the plain body
# (-DLZ6SEQ_NO_DYNAMIC_BMI2). Per clone, under ASAN+UBSan:
#   pocs     hufpoc + scpoc, the two memory-safety bugs fixed by S0; both must run clean
#   edge     synthetic round trips (sizes 0..300 and 1000..1000000, runs, repeats, random) at 7 levels
#   tinyrt   round trips of tiny blocks (8..707 bytes) at levels 1 2 3 5 8 12 15
#   xfuzz2   mutation fuzz (flips, truncation, random runs, header bytes) on real slices and on small
#            synthetic streams, decoding from an exact-size copy of the mutant
#   xdiff    only with REFTREE (a checkout of the reference decoder): the return value and an output hash of
#            every mutant must equal the reference's. For decoder-only changes: a format change cannot use it.
# Then once: asan (round trips of 6 files x 7 levels + 25000 fuzz iterations, default build) and m32
# (32-bit build with sanitizers, falling back to none when the toolchain has no 32-bit sanitizer runtime).
# Prints one PASS/FAIL line per check and a final "GATE PASS" or "GATE FAIL"; the exit status is 0 on a pass.
#
# Environment:
#   GATE_N        mutation-fuzz and xdiff iterations per clone (default 60000, about 10 minutes in all;
#                 the gate for a finished change uses GATE_N=300000 TINY_N=5000)
#   TINY_N        tinyrt iterations (default 1000)
#   SILESIA       directory with the Silesia files xml mr ooffice samba x-ray dickens
#   SEQGATE_CPU   CPU number to pin every run to with taskset (default: not pinned)
#   SEQGATE_WORK  scratch directory (default: a mktemp directory, removed after a pass)
#   CC            C compiler (default gcc)
# Sanity: a tree without the S0 fixes FAILs pocs on purpose.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GATE="$ROOT/bakeoff/gate"
REF="${1:-}"
N="${GATE_N:-60000}"
TN="${TINY_N:-1000}"
SIL="${SILESIA:-/mnt/IA_LAB/agentes/lz6/silesia}"
CC="${CC:-gcc}"

for f in xml mr ooffice samba x-ray dickens; do
  [ -f "$SIL/$f" ] || { echo "GATE FAIL (missing $SIL/$f; set SILESIA)"; exit 2; }
done
[ -z "$REF" ] || [ -f "$REF/lib/entropy/lz6seq.c" ] || { echo "GATE FAIL (REFTREE $REF is not an lz6 checkout)"; exit 2; }
KEEP="${SEQGATE_WORK:-}"
W="${KEEP:-$(mktemp -d "${TMPDIR:-/tmp}/seqgate.XXXXXX")}"
mkdir -p "$W/set" "$W/asan" "$W/m32" || exit 2

run() { if [ -n "${SEQGATE_CPU:-}" ]; then taskset -c "$SEQGATE_CPU" "$@"; else "$@"; fi; }
srcs() { echo "$1/lib/lz6.c $1/lib/lz6hc.c $1/lib/entropy/lz6seq.c $1/lib/entropy/lz6fse.c $1/lib/entropy/lz6rc.c $1/lib/entropy/lz6huf.c"; }
fail=0
note() { echo "$1"; case "$1" in FAIL*) fail=1 ;; esac; }
flat() { tr '\n' ' ' | head -c "$1"; }

SAN=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -std=gnu11)
INC=(-I"$ROOT" -I"$ROOT/lib" -I"$ROOT/lib/entropy")
S=($(srcs "$ROOT"))

$CC -O2 -std=gnu11 "${INC[@]}" "$GATE/mkseq.c" "${S[@]}" -o "$W/mkseq" -lm || { echo "GATE FAIL (build mkseq)"; exit 8; }
$CC -O2 -std=gnu11 "${INC[@]}" "$GATE/mkset.c" "${S[@]}" -o "$W/mkset" -lm || { echo "GATE FAIL (build mkset)"; exit 8; }

# fuzz corpus: slices of real files at four levels, plus small synthetic streams
for f in xml mr ooffice samba x-ray; do head -c 300000 "$SIL/$f" > "$W/$f.s"; done
head -c 200000 "$SIL/dickens" > "$W/dickens.s"
for L in 1 3 8 15; do for f in "$W"/*.s; do run "$W/mkseq" $L "$f" "$W/set/$(basename "$f").L$L.seq"; done; done
"$W/mkset" "$W/set" > /dev/null

grep -qw bmi2 /proc/cpuinfo 2>/dev/null || note "FAIL bmi2: this CPU has no BMI2, the BMI2 clone is not exercised"

for clone in bmi2 default; do
  X=(); [ "$clone" = default ] && X=(-DLZ6SEQ_NO_DYNAMIC_BMI2)
  for poc in hufpoc scpoc; do
    $CC "${SAN[@]}" "${X[@]}" "${INC[@]}" "$GATE/$poc.c" "${S[@]}" -o "$W/${poc}_$clone" -lm || { note "FAIL build $poc $clone"; continue; }
    out=$(run "$W/${poc}_$clone" 2>&1); rc=$?
    if [ $rc -eq 0 ]; then note "PASS $poc $clone"; else note "FAIL $poc $clone rc=$rc: $(echo "$out" | flat 300)"; fi
  done
  $CC "${SAN[@]}" "${X[@]}" "${INC[@]}" "$GATE/edge.c" "${S[@]}" -o "$W/edge_$clone" -lm || { note "FAIL build edge $clone"; continue; }
  out=$(run "$W/edge_$clone" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then note "PASS edge $clone: $(echo "$out" | tail -1 | head -c 150)"; else note "FAIL edge $clone rc=$rc: $(echo "$out" | flat 400)"; fi
  $CC "${SAN[@]}" "${X[@]}" "${INC[@]}" "$GATE/tinyrt.c" "${S[@]}" -o "$W/tinyrt_$clone" -lm || { note "FAIL build tinyrt $clone"; continue; }
  out=$(run "$W/tinyrt_$clone" "$TN" "$SIL/xml" "$SIL/dickens" "$SIL/mr" "$SIL/ooffice" "$SIL/samba" 2>&1); rc=$?
  if [ $rc -eq 0 ]; then note "PASS tinyrt $clone ($TN): $(echo "$out" | tail -1)"; else note "FAIL tinyrt $clone rc=$rc: $(echo "$out" | flat 400)"; fi
  $CC "${SAN[@]}" "${X[@]}" "${INC[@]}" "$GATE/xfuzz2.c" "${S[@]}" -o "$W/xfuzz2_$clone" -lm || { note "FAIL build xfuzz2 $clone"; continue; }
  out=$(run "$W/xfuzz2_$clone" "$N" "$W"/set/*.seq 2>&1); rc=$?
  if [ $rc -eq 0 ]; then note "PASS xfuzz2 $clone ($N): $(echo "$out" | tail -1)"; else note "FAIL xfuzz2 $clone rc=$rc: $(echo "$out" | flat 500)"; fi
  if [ -n "$REF" ]; then
    RINC=(-I"$REF" -I"$REF/lib" -I"$REF/lib/entropy"); RS=($(srcs "$REF"))
    if $CC -O2 -std=gnu11 "${X[@]}" "${INC[@]}" "$GATE/xdiff.c" "${S[@]}" -o "$W/xdiff_new_$clone" -lm &&
       $CC -O2 -std=gnu11 "${X[@]}" "${RINC[@]}" "$GATE/xdiff.c" "${RS[@]}" -o "$W/xdiff_ref_$clone" -lm; then
      run "$W/xdiff_new_$clone" "$N" "$W"/set/*.seq > "$W/xd_new_$clone.out" 2> /dev/null
      run "$W/xdiff_ref_$clone" "$N" "$W"/set/*.seq > "$W/xd_ref_$clone.out" 2> /dev/null
      if cmp -s "$W/xd_new_$clone.out" "$W/xd_ref_$clone.out" && [ -s "$W/xd_new_$clone.out" ]; then
        note "PASS xdiff $clone: $(wc -l < "$W/xd_new_$clone.out") mutants identical to REFTREE (rc + output hash)"
      else
        note "FAIL xdiff $clone: differs from REFTREE: $(cmp "$W/xd_new_$clone.out" "$W/xd_ref_$clone.out" 2>&1 | head -1)"
      fi
    else
      note "FAIL build xdiff $clone"
    fi
  fi
done

# round trips at the CLI-free level, ASAN+UBSan, default build; and the 32-bit build on the same slices
for f in xml mr ooffice; do head -c 400000 "$SIL/$f" > "$W/asan/$f.s400k"; done
for f in dickens samba x-ray; do head -c 1500000 "$SIL/$f" > "$W/asan/$f.s"; done
SLICES=("$W"/asan/*.s400k "$W"/asan/dickens.s "$W"/asan/samba.s "$W"/asan/x-ray.s)
if $CC "${SAN[@]}" "${INC[@]}" "$GATE/rt.c" "${S[@]}" -o "$W/rt" -lm &&
   $CC "${SAN[@]}" "${INC[@]}" "$GATE/xfuzz.c" "${S[@]}" -o "$W/xfuzz" -lm; then
  bad=0
  for L in 1 2 3 5 8 12 15; do for f in "${SLICES[@]}"; do
    out=$(run "$W/rt" $L "$f" 2>&1); rc=$?
    if [ $rc -ne 0 ]; then bad=1; note "FAIL asan rt L$L $f rc=$rc: $(echo "$out" | flat 300)"; fi
  done; done
  for L in 1 3 8 15; do for f in "$W"/asan/xml.s400k "$W"/asan/mr.s400k "$W"/asan/ooffice.s400k "$W"/asan/samba.s; do
    run "$W/mkseq" $L "$f" "$W/asan/$(basename "$f").L$L.seq"
  done; done
  out=$(run "$W/xfuzz" 25000 "$W"/asan/*.seq 2>&1); rc=$?
  if [ $rc -ne 0 ]; then bad=1; note "FAIL asan xfuzz rc=$rc: $(echo "$out" | flat 400)"; fi
  [ $bad -eq 0 ] && note "PASS asan: 42 round trips and $(echo "$out" | tail -1)"
else
  note "FAIL build asan"
fi

M32=(-O1 -g -m32 -std=gnu11); M32S=""
if $CC "${M32[@]}" -fsanitize=address,undefined "$GATE/hufpoc.c" "${INC[@]}" "${S[@]}" -o "$W/m32/probe" -lm 2> /dev/null; then
  M32+=(-fsanitize=address,undefined -fno-sanitize-recover=undefined); M32S=" +asan+ubsan"
fi
if $CC "${M32[@]}" "${INC[@]}" "$GATE/rt.c" "${S[@]}" -o "$W/m32/rt" -lm &&
   $CC "${M32[@]}" "${INC[@]}" "$GATE/edge.c" "${S[@]}" -o "$W/m32/edge" -lm; then
  bad=0
  for L in 1 3 8 15; do for f in "${SLICES[@]}"; do
    out=$(run "$W/m32/rt" $L "$f" 2>&1); rc=$?
    if [ $rc -ne 0 ]; then bad=1; note "FAIL m32 rt L$L $f rc=$rc: $(echo "$out" | flat 300)"; fi
  done; done
  out=$(run "$W/m32/edge" 2>&1); rc=$?
  if [ $rc -ne 0 ]; then bad=1; note "FAIL m32 edge rc=$rc: $(echo "$out" | flat 400)"; fi
  [ $bad -eq 0 ] && note "PASS m32$M32S: 24 round trips; $(echo "$out" | tail -1)"
else
  note "FAIL build m32 (no 32-bit toolchain?)"
fi

if [ $fail -eq 0 ]; then
  echo "GATE PASS"
  [ -n "$KEEP" ] || rm -rf "$W"
  exit 0
fi
echo "GATE FAIL (scratch kept in $W)"
exit 1
