#!/usr/bin/env bash
# bench_decode.sh — measure lz6 decode speed + verify round-trip.
# Usage:  bakeoff/bench_decode.sh [label]
# Run from the repo root. Rebuilds lz6, checks round-trip on 3 corpora,
# then reports decode MB/s. Compare a candidate's number against baseline.
#
# Corpora expected in /tmp (recreate if missing — see repo notes):
#   /tmp/realistic.dat  (~4 MB binary)
#   /tmp/corpus8.dat    (~4 MB text/dict)
#   /tmp/sil40.dat      (40 MB, silesia slice)
set -u
LABEL="${1:-current}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LZ6="$ROOT/programs/lz6"
CORPORA=(/tmp/realistic.dat /tmp/corpus8.dat /tmp/sil40.dat)

echo "== bench_decode.sh  label=$LABEL =="

# 1. build
rm -f "$ROOT"/lib/*.o "$ROOT"/programs/*.o "$LZ6" 2>/dev/null
if ! make -C "$ROOT/programs" >/dev/null 2>&1; then
  echo "BUILD FAIL — disqualified"; exit 2
fi

# 2. round-trip gate (HARD): must pass every corpus at several levels
rt_fail=0
for f in "${CORPORA[@]}"; do
  [ -f "$f" ] || { echo "skip (missing) $f"; continue; }
  for L in 1 6 11 15; do
    "$LZ6" -$L -f "$f" /tmp/_bo.lz6 >/dev/null 2>&1
    "$LZ6" -d -f /tmp/_bo.lz6 /tmp/_bo.out >/dev/null 2>&1
    if ! cmp -s "$f" /tmp/_bo.out; then
      echo "ROUND-TRIP FAIL: $(basename "$f") L$L"; rt_fail=1
    fi
  done
done
if [ $rt_fail -ne 0 ]; then
  echo "RESULT[$LABEL]: round-trip FAILED — DISQUALIFIED"; exit 1
fi
echo "round-trip: OK (all corpora, L1/6/11/15)"

# 2b. SAFETY GATE (HARD): ASAN + malformed-input fuzz on the block decoder.
#     The CLI over-allocates output, hiding out-of-bounds writes; this driver
#     decodes corrupted blocks into an EXACTLY sized buffer so any OOB hits an
#     ASAN redzone. A decoder that is faster but unsafe on malformed input is
#     disqualified — decompressors process untrusted data.
FUZZ_BIN=/tmp/lz6_fuzz_decode
if cc -O2 -g -fsanitize=address -std=gnu99 -I"$ROOT/lib" -DXXH_NAMESPACE=LZ6_ \
      "$ROOT/bakeoff/fuzz_decode.c" "$ROOT/lib/lz6.c" "$ROOT/lib/lz6hc.c" "$ROOT/lib/xxhash.c" \
      -o "$FUZZ_BIN" 2>/tmp/lz6_fuzz_build.log; then
  export ASAN_OPTIONS=abort_on_error=0:exitcode=99:detect_leaks=0
  safe_fail=0
  for f in "${CORPORA[@]}"; do
    [ -f "$f" ] || continue
    if ! "$FUZZ_BIN" "$f" 8000 >/tmp/lz6_fuzz.out 2>&1; then
      echo "SAFETY FAIL — ASAN caught an out-of-bounds access (seed=$(basename "$f")):"
      grep -m3 -E "AddressSanitizer|SUMMARY|overflow|lz6.c:" /tmp/lz6_fuzz.out | sed 's/^/    /'
      safe_fail=1; break
    fi
  done
  if [ $safe_fail -ne 0 ]; then
    echo "RESULT[$LABEL]: unsafe decoder — DISQUALIFIED"; exit 1
  fi
  echo "safety (ASAN fuzz): OK (no OOB on malformed input, all corpora)"
else
  echo "WARN: could not build ASAN fuzz driver (skipping safety gate):"; tail -3 /tmp/lz6_fuzz_build.log
fi

# 3. decode MB/s — the 2nd MB/s figure from -b (-i5 = best of 5)
#    report per corpus at level 6 (representative HC output)
decode_mbs() { # file
  "$LZ6" -b6 -i5 "$1" 2>&1 | tr '\r' '\n' \
    | grep -oE '[0-9.]+ MB/s +, +[0-9.]+ MB/s' | tail -1 \
    | grep -oE '[0-9.]+ MB/s' | tail -1 | grep -oE '[0-9.]+'
}
echo "decode MB/s (level 6 output, best of 5):"
for f in "${CORPORA[@]}"; do
  [ -f "$f" ] || continue
  v="$(decode_mbs "$f")"
  printf "  %-16s %s MB/s\n" "$(basename "$f")" "${v:-?}"
done
echo "RESULT[$LABEL]: round-trip OK — compare decode MB/s vs baseline above."
