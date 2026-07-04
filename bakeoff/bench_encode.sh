#!/bin/bash
# bench_encode.sh — measurement harness for the lz6 ENCODE (compression) bake-off.
#
# Encode differs from decode: the output is NOT fixed. A candidate can "go faster"
# by finding fewer/worse matches, which SILENTLY costs ratio. So the gate is:
#
#   1) round-trip MUST pass (decodes back to the original) — hard gate.
#   2) ratio MUST NOT regress — the compressed output is compared byte-for-byte
#      against the recorded baseline. Byte-identical => pure speedup (best).
#      Larger-than-baseline (> RATIO_EPS) => DISQUALIFIED.
#   3) then, and only then, report encode MB/s + delta vs baseline.
#
# Usage:
#   bench_encode.sh baseline        # capture reference sizes/outputs/speeds (run on clean HEAD)
#   bench_encode.sh <label>         # build current tree, gate it, compare vs baseline
#
# Corpora (in /tmp): realistic.dat, corpus8.dat (~4MB each), sil40.dat (39MB).

set -u
cd "$(dirname "$0")/.."          # repo root
BIN=programs/lz6
OUTDIR=bakeoff/encode_baseline   # baseline .lz6 outputs live here
BASEFILE=bakeoff/ENCODE_BASELINE.txt
RATIO_EPS_PCT="0.10"             # allow <=0.10% size growth (equal-price reorderings); above => fail

CORPORA=(/tmp/realistic.dat /tmp/corpus8.dat /tmp/sil40.dat)
SMALL=(/tmp/realistic.dat /tmp/corpus8.dat)   # small corpora used for slow high-level timing
ITERS=5

label="${1:-}"
[ -z "$label" ] && { echo "usage: $0 baseline | <label>"; exit 2; }

build() {
  rm -f lib/*.o programs/*.o
  make -j4 >/tmp/enc_build.log 2>&1 || { echo "BUILD FAIL"; tail -20 /tmp/enc_build.log; exit 1; }
}

# ASAN build — catches OOB reads/writes in the encoder (e.g. a chain-delta U32
# wrap sending a match pointer out of bounds). Optimal parser is slow, so most
# corpora here are small slices — EXCEPT full sil40.dat below, which is load-
# bearing: the CLI streams input through LZ6F_compressUpdate in fixed 16MB
# blocks, and a real heap-buffer-overflow (stale/unbounded best_mlen feeding
# LZ6HC_GetAllMatches's speculative ip[best_mlen] head-check, fixed 2026-07-04)
# only manifested exactly at that 16MB block boundary on an input bigger than
# one block — a 1.2MB slice can never reach it. Do not shrink sil40.dat back
# to a slice here without another way to cross a 16MB block boundary.
# Rebuilds -O3 afterward for timing.
asan_gate() {
  rm -f lib/*.o programs/*.o
  make -j4 CFLAGS="-O1 -g -fsanitize=address -std=gnu99" LDFLAGS="-fsanitize=address" \
       >/tmp/enc_asan_build.log 2>&1 || { echo "  ASAN BUILD FAIL"; tail -20 /tmp/enc_asan_build.log; return 1; }
  local ok=1
  head -c 1200000 /tmp/sil40.dat > /tmp/asan_sil.dat
  for f in /tmp/realistic.dat /tmp/corpus8.dat /tmp/asan_sil.dat; do
    for lvl in 11 15; do
      ASAN_OPTIONS=detect_leaks=0 $BIN -"$lvl" -f "$f" /tmp/asan.lz6 >/tmp/asan_run.log 2>&1 \
        && ASAN_OPTIONS=detect_leaks=0 $BIN -d -f /tmp/asan.lz6 /tmp/asan.out >>/tmp/asan_run.log 2>&1 \
        && cmp -s "$f" /tmp/asan.out \
        || { echo "  ASAN/round-trip FAIL $(basename "$f") L$lvl"; tail -15 /tmp/asan_run.log; ok=0; }
    done
  done
  # full-size, multi-16MB-block corpus — see note above; slow (~10-60s/level) but load-bearing
  for lvl in 11 15; do
    ASAN_OPTIONS=detect_leaks=0 $BIN -"$lvl" -f /tmp/sil40.dat /tmp/asan.lz6 >/tmp/asan_run.log 2>&1 \
      && ASAN_OPTIONS=detect_leaks=0 $BIN -d -f /tmp/asan.lz6 /tmp/asan.out >>/tmp/asan_run.log 2>&1 \
      && cmp -s /tmp/sil40.dat /tmp/asan.out \
      || { echo "  ASAN/round-trip FAIL sil40.dat (full, multi-block) L$lvl"; tail -15 /tmp/asan_run.log; ok=0; }
  done
  rm -f lib/*.o programs/*.o   # force -O3 rebuild after
  return $((1-ok))
}

# encode MB/s for a file at a level: BEST (max) of all iterations' first MB/s
# figure. Best-of is the standard for benchmarking (least external interference)
# and removes the per-iteration noise that tail -1 would pick up. Robust to \r.
enc_mbps() {
  local f="$1" lvl="$2"
  timeout 240 $BIN -b -"$lvl" -i"$ITERS" "$f" 2>&1 | tr '\r' '\n' \
    | grep -E "MB/s ,.*MB/s" \
    | sed -E 's/.*\(([0-9.]+)%\), *([0-9.]+) MB\/s.*/\2/' \
    | sort -g | tail -1
}

# compressed size (bytes) for a file at a level
csize() {
  local f="$1" lvl="$2" out="$3"
  $BIN -"$lvl" -f "$f" "$out" 2>/dev/null
  stat -c %s "$out"
}

roundtrip() {
  local f="$1" lvl="$2"
  local c=/tmp/rt.lz6 d=/tmp/rt.out
  $BIN -"$lvl" -f "$f" "$c" 2>/dev/null || return 1
  $BIN -d -f "$c" "$d" 2>/dev/null || return 1
  cmp -s "$f" "$d"
}

# ---------------- baseline capture ----------------
if [ "$label" = "baseline" ]; then
  build
  mkdir -p "$OUTDIR"
  : > "$BASEFILE"
  echo "# lz6 encode baseline — commit $(git rev-parse --short HEAD)" >> "$BASEFILE"
  echo "# fmt: SIZE <corpus> <lvl> <bytes> | MBPS <corpus> <lvl> <encMBps>" >> "$BASEFILE"
  # ratio baseline: sizes + saved outputs at the optimal levels
  for f in "${CORPORA[@]}"; do
    b=$(basename "$f")
    for lvl in 11 15; do
      out="$OUTDIR/${b}.L${lvl}.lz6"
      sz=$(csize "$f" "$lvl" "$out")
      echo "SIZE $b $lvl $sz" >> "$BASEFILE"
    done
  done
  # speed baseline: small corpora at 11/14/15, sil40 at 11 only (large-file, slow)
  for f in "${SMALL[@]}"; do
    b=$(basename "$f")
    for lvl in 11 14 15; do
      echo "MBPS $b $lvl $(enc_mbps "$f" "$lvl")" >> "$BASEFILE"
    done
  done
  echo "MBPS $(basename /tmp/sil40.dat) 11 $(enc_mbps /tmp/sil40.dat 11)" >> "$BASEFILE"
  echo "baseline captured -> $BASEFILE  (+outputs in $OUTDIR)"
  cat "$BASEFILE"
  exit 0
fi

# ---------------- candidate evaluation ----------------
[ -f "$BASEFILE" ] || { echo "no baseline ($BASEFILE). run: $0 baseline"; exit 2; }

echo "== bench_encode.sh  label=$label =="

# 0) ASAN safety gate (compress+decompress under AddressSanitizer)
if asan_gate; then echo "safety (ASAN encode round-trip): OK"; else
  echo "RESULT[$label]: DISQUALIFIED — ASAN violation in encoder"; exit 1; fi

build

# 1) round-trip gate
rt_ok=1
for f in "${SMALL[@]}"; do
  for lvl in 1 6 11 15; do roundtrip "$f" "$lvl" || { echo "  round-trip FAIL $(basename "$f") L$lvl"; rt_ok=0; }; done
done
for lvl in 1 6 11; do roundtrip /tmp/sil40.dat "$lvl" || { echo "  round-trip FAIL sil40 L$lvl"; rt_ok=0; }; done
if [ "$rt_ok" = 1 ]; then echo "round-trip: OK (all corpora)"; else
  echo "RESULT[$label]: DISQUALIFIED — round-trip failed"; exit 1; fi

# 2) ratio gate: compare compressed output vs baseline, byte + size
ratio_ok=1; identical=1
for f in "${CORPORA[@]}"; do
  b=$(basename "$f")
  for lvl in 11 15; do
    base_out="$OUTDIR/${b}.L${lvl}.lz6"
    base_sz=$(awk -v c="$b" -v l="$lvl" '$1=="SIZE"&&$2==c&&$3==l{print $4}' "$BASEFILE")
    new_out="/tmp/cand.${b}.L${lvl}.lz6"
    new_sz=$(csize "$f" "$lvl" "$new_out")
    if [ -f "$base_out" ] && cmp -s "$base_out" "$new_out"; then
      echo "  ratio $b L$lvl: byte-IDENTICAL ($new_sz)"
    elif [ ! -f "$base_out" ] && [ "$new_sz" = "$base_sz" ]; then
      echo "  ratio $b L$lvl: size-identical ($new_sz; baseline .lz6 not stored)"
    else
      identical=0
      # size delta %
      d=$(awk -v n="$new_sz" -v o="$base_sz" 'BEGIN{printf "%.3f",(n-o)*100.0/o}')
      over=$(awk -v n="$new_sz" -v o="$base_sz" -v e="$RATIO_EPS_PCT" 'BEGIN{print (n>o*(1+e/100))?1:0}')
      if [ "$over" = 1 ]; then
        echo "  ratio $b L$lvl: REGRESSED  base=$base_sz new=$new_sz (${d}%)  <-- exceeds ${RATIO_EPS_PCT}%"
        ratio_ok=0
      else
        echo "  ratio $b L$lvl: ok (not identical) base=$base_sz new=$new_sz (${d}%)"
      fi
    fi
  done
done
if [ "$ratio_ok" != 1 ]; then echo "RESULT[$label]: DISQUALIFIED — ratio regression"; exit 1; fi
[ "$identical" = 1 ] && echo "ratio: OK (byte-identical everywhere — pure speedup)" || echo "ratio: OK (within ${RATIO_EPS_PCT}%)"

# 3) encode MB/s + delta
echo "encode MB/s (best of $ITERS, Δ% vs baseline):"
report() {
  local f="$1" lvl="$2" b; b=$(basename "$f")
  local base new d
  base=$(awk -v c="$b" -v l="$lvl" '$1=="MBPS"&&$2==c&&$3==l{print $4}' "$BASEFILE")
  new=$(enc_mbps "$f" "$lvl")
  d=$(awk -v n="$new" -v o="$base" 'BEGIN{if(o>0)printf "%+.1f",(n-o)*100.0/o; else print "n/a"}')
  printf "  %-14s L%-2s  %7s -> %7s MB/s  (%s%%)\n" "$b" "$lvl" "$base" "$new" "$d"
}
for f in "${SMALL[@]}"; do for lvl in 11 14 15; do report "$f" "$lvl"; done; done
report /tmp/sil40.dat 11
echo "RESULT[$label]: PASS (round-trip OK, ratio OK)"
