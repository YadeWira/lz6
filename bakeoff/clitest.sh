#!/bin/bash
# clitest.sh CLI : behaviour of the lz6 command line on bad input, odd names, closed pipes, full disks, bad options, read errors and flipped bits.
# A check passes when the exit status and the effects are the stated ones; a crash (signal) or a hang (timeout) always fails.
#   CLI_DYN   a dynamically linked build of the CLI: enables the read-error injection (an LD_PRELOAD shim cannot touch a static binary)
#   SILESIA   directory with the Silesia files xml, mr and x-ray (default /mnt/IA_LAB/agentes/lz6/silesia)
#   CLITEST_WORK  scratch directory (default: a mktemp directory, kept)
# Prints a line per failure and a summary; the exit status is 0 when everything passed.
CLI=$(readlink -f "$1"); HERE="$(cd "$(dirname "$0")" && pwd)"; SIL="${SILESIA:-/mnt/IA_LAB/agentes/lz6/silesia}"
W="${CLITEST_WORK:-$(mktemp -d "${TMPDIR:-/tmp}/clitest.XXXXXX")}"; mkdir -p "$W" && cd "$W" || exit 2
pass=0; fail=0
ok()  { pass=$((pass+1)); }
bad() { fail=$((fail+1)); echo "FAIL: $1"; }
run() { timeout 120 "$@" > out.txt 2> err.txt; RC=$?; }
sane() { [ $RC -lt 124 ] || { bad "$1 (rc=$RC: $( [ $RC = 124 ] && echo hang || echo crash/signal ))"; return 1; }; }
expect_fail() { d=$1; shift; run "$@"; sane "$d" || return; [ $RC -ne 0 ] && ok || bad "$d: expected a failure, got rc=0"; }
expect_ok()   { d=$1; shift; run "$@"; sane "$d" || return; [ $RC -eq 0 ] && ok || bad "$d: rc=$RC $(head -c 120 err.txt)"; }
unchanged()   { cmp -s "$1" "$2" && ok || bad "$3"; }
head -c 300000 $SIL/xml > a.bin; head -c 200000 $SIL/mr > b.bin; : > empty.bin; head -c 5000000 $SIL/x-ray > big.bin

# --- input errors
expect_fail "missing input file" $CLI -1 nofile.bin
mkdir -p adir
expect_fail "directory as input (compress)" $CLI -1 adir adir.lz6
expect_fail "directory as input (to stdout)" $CLI -1 -c adir
expect_fail "directory as input (decompress)" $CLI -d adir
expect_fail "decompress garbage from stdin" bash -c "echo 'this is not an lz6 stream at all' | $CLI -d -c"
echo 'this is not an lz6 stream at all' > g.lz6
expect_fail "decompress garbage from a file" $CLI -d -c g.lz6
expect_fail "-t on a file that is not an lz6 stream" $CLI -t g.lz6
expect_fail "-t -f on a file that is not an lz6 stream" $CLI -tf g.lz6
expect_fail "-t on garbage from stdin" bash -c "echo garbage | $CLI -t"
expect_ok "-d -f passes an unrecognized file through" bash -c "echo 'hello text' | $CLI -dfc | grep -q 'hello text'"
# --- the degenerate inputs, file and pipe
for o in "-0" "--fast=2" "-1" "-5" "--hc -3"; do
  expect_ok "empty file $o" bash -c "$CLI $o -c empty.bin > e.lz6 && $CLI -d -c e.lz6 > e.out && cmp -s empty.bin e.out && $CLI -t e.lz6"
  expect_ok "empty stdin $o" bash -c "$CLI $o -c < empty.bin > e2.lz6 && $CLI -d -c < e2.lz6 > e2.out && cmp -s empty.bin e2.out"
done
printf 'x' > one.bin; expect_ok "one byte" bash -c "$CLI -3 -c one.bin | $CLI -d -c | cmp -s - one.bin"
# --- overwrite rules (an explicit output name: without one the output goes to stdout when stdout is not a console)
echo 'PRECIOUS' > keep.lz6; cp keep.lz6 keep.ref
expect_fail "output exists, stdin at its end: must not overwrite" bash -c "$CLI -1 a.bin keep.lz6 < /dev/null"
unchanged keep.lz6 keep.ref "an existing output was overwritten when stdin ended at the prompt"
expect_fail "output exists, answer n" bash -c "echo n | $CLI -1 a.bin keep.lz6"
unchanged keep.lz6 keep.ref "an existing output was overwritten after the answer n"
expect_fail "output exists, -q (no interaction possible)" bash -c "$CLI -1 -q a.bin keep.lz6 < /dev/null"
unchanged keep.lz6 keep.ref "an existing output was overwritten with -q"
expect_ok "output exists, answer y" bash -c "echo y | $CLI -1 a.bin keep.lz6"
echo 'PRECIOUS' > keep.lz6; expect_ok "overwrite with -f" $CLI -1 -f a.bin keep.lz6
# --- truncation and trailing data
$CLI -3 -B1 -c a.bin > c.lz6; sz=$(stat -c %s c.lz6); tf=0
for cut in 1 3 4 7 10 20 50 100 $((sz/2)) $((sz-10)) $((sz-5)) $((sz-4)) $((sz-1)); do
  head -c $cut c.lz6 > cut.lz6; run $CLI -d -c cut.lz6; [ $RC -ge 124 ] && { bad "truncated at $cut: rc=$RC"; tf=1; }
  [ $RC -eq 0 ] && cmp -s a.bin out.txt && { bad "truncated at $cut decoded to the original?"; tf=1; }
  run $CLI -t cut.lz6; [ $RC -eq 0 ] && { bad "-t accepted a stream cut at $cut"; tf=1; }
done; [ $tf = 0 ] && ok
cp c.lz6 trail.lz6; printf 'JUNKJUNKJUNK' >> trail.lz6; run $CLI -d -c trail.lz6; sane "trailing junk" && ok
# --- concatenated and skippable frames
$CLI -2 -c b.bin > b.lz6; cat c.lz6 b.lz6 > ab.lz6; cat a.bin b.bin > ab.bin
expect_ok "concatenated frames" bash -c "$CLI -d -c ab.lz6 | cmp -s - ab.bin"
expect_ok "concatenated frames from a pipe" bash -c "cat ab.lz6 | $CLI -d -c | cmp -s - ab.bin"
printf '\x50\x2a\x4d\x18\x08\x00\x00\x00ABCDEFGH' > skip.lz6; cat skip.lz6 c.lz6 > sc.lz6
expect_ok "skippable frame first" bash -c "$CLI -d -c sc.lz6 | cmp -s - a.bin"
# --- names
mkdir -p 'dir with space'; cp a.bin 'dir with space/we ird é ü.bin'
expect_ok "name with spaces and accents" bash -c "$CLI -1 -f 'dir with space/we ird é ü.bin' 'dir with space/o ut é.lz6' && $CLI -d -f 'dir with space/o ut é.lz6' 'dir with space/back é.bin' && cmp -s a.bin 'dir with space/back é.bin'"
cp a.bin ./-dash.bin
expect_ok "name starting with a dash after --" bash -c "$CLI -1 -f -- -dash.bin -dash.lz6 && $CLI -d -f -- -dash.lz6 -dash.out && cmp -s a.bin ./-dash.out"
expect_ok "-m multiple files" bash -c "cp a.bin m1.bin; cp b.bin m2.bin; $CLI -1 -f -m m1.bin m2.bin && $CLI -d -f -m m1.bin.lz6 m2.bin.lz6 && cmp -s m1.bin a.bin && cmp -s m2.bin b.bin"
# --- options
for o in "-T0" "-T1" "-T3" "-T64"; do expect_ok "option $o" bash -c "$CLI -1 $o -B1 -c a.bin | $CLI -d $o -c | cmp -s - a.bin"; done
for o in "-T-1" "-Tx" "-B0" "-B8" "-B9" "-B15" "--fast=x" "--fast=0" "--fast=2x" "--nosuchoption"; do expect_fail "bad option $o" $CLI $o -c a.bin; done
for o in "--fast=1" "--fast=2" "--fast=3" "--fast=999" "-14" "-15" "-16" "-99"; do expect_ok "option $o" bash -c "$CLI $o -B1 -c a.bin | $CLI -d -c | cmp -s - a.bin"; done
expect_ok "-14 equals -13" bash -c "$CLI -14 -B1 -c a.bin | cmp -s - <($CLI -13 -B1 -c a.bin)"
expect_ok "-v and -q" bash -c "$CLI -1 -v -c a.bin >/dev/null 2>&1 && $CLI -1 -q -c a.bin >/dev/null"
# --- closed pipe, full disk, unwritable output
run bash -c "$CLI --fast=2 -c big.bin | head -c 10 > /dev/null"; sane "closed pipe" && ok
expect_fail "full disk" bash -c "$CLI -1 -c a.bin > /dev/full"
mkdir -p ro && chmod 555 ro; cp a.bin ro_in.bin; expect_fail "unwritable output dir" $CLI -1 -f ro_in.bin ro/out.lz6; chmod 755 ro
# --- sparse files
truncate -s 20M sparse.bin; printf 'data' | dd of=sparse.bin bs=1 seek=10000000 conv=notrunc 2>/dev/null
expect_ok "sparse file round trip" bash -c "$CLI -1 -c sparse.bin | $CLI -d --sparse -f -c - > sparse.out; cmp -s sparse.bin sparse.out"
expect_ok "sparse decode to a file" bash -c "$CLI -1 -c sparse.bin > sp.lz6 && $CLI -d -f --sparse sp.lz6 sp.out && cmp -s sparse.bin sp.out && [ \$(du -k sp.out | cut -f1) -lt 5000 ]"
# --- a read that fails half way (EIO) must be an error, never a hang and never a shorter file reported as a success
if [ -n "${CLI_DYN:-}" ] && gcc -shared -fPIC -O2 "$HERE/clitest/failread.c" -o failread.so -ldl 2>/dev/null; then
  head -c 6000000 $SIL/x-ray > big6.bin
  for cfg in "--fast=2" "-1" "-5" "-0" "--hc -3"; do
    FAILREAD_AFTER=1500000 LD_PRELOAD=$PWD/failread.so run $(readlink -f "$CLI_DYN") $cfg -B3 -f big6.bin fr.lz6
    sane "read error during compression ($cfg)" && { [ $RC -ne 0 ] && ok || bad "read error during compression ($cfg): reported success (rc=0) with a shorter output"; }
  done
  $CLI -3 -B3 -c big6.bin > big6.lz6
  FAILREAD_AFTER=500000 LD_PRELOAD=$PWD/failread.so run $(readlink -f "$CLI_DYN") -d -c big6.lz6; sane "read error during decompression" && { [ $RC -ne 0 ] && ok || bad "read error during decompression: rc=0"; }
else
  echo "note: read-error injection skipped (set CLI_DYN to a dynamically linked build, and have gcc)"
fi
# --- integrity: with the default checksum a flipped bit must never decode to different data without an error
for cfg in "--fast=2 -B1" "-1 -B1" "-5 -B1" "--hc -3 -B1" "-0 -B1"; do
  $CLI $cfg -c b.bin > f.lz6; fsz=$(stat -c %s f.lz6); silent=0
  for i in $(seq 1 120); do
    python3 -I - "$fsz" "$i" <<'PY'
import sys, random
sz, seed = int(sys.argv[1]), int(sys.argv[2]); random.seed(seed*7919+sz)
b = bytearray(open('f.lz6','rb').read()); pos = random.randrange(sz); b[pos] ^= 1 << random.randrange(8); open('flip.lz6','wb').write(b)
PY
    run $CLI -d -c flip.lz6; [ $RC -ge 124 ] && { bad "bit flip ($cfg #$i): rc=$RC"; silent=1; continue; }
    if [ $RC -eq 0 ] && ! cmp -s b.bin out.txt; then silent=1; echo "SILENT CORRUPTION: $cfg flip #$i decoded without error to different data"; fi
  done
  [ $silent = 0 ] && ok || bad "integrity with $cfg"
done
echo "clitest: $pass passed, $fail failed"
[ $fail = 0 ]
