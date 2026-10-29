#!/bin/bash
# CI test: build the command line, round-trip generated inputs at every level, run the command-line checks.
#   .github/test.sh [path of the lz6 binary]   (default: programs/lz6, built by `make -C programs lz6`)
# The inputs are generated here (no corpus is needed); bakeoff/clitest.sh gets stand-ins for the three Silesia files it reads.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; CLI="${1:-$ROOT/programs/lz6}"; CLI="$(cd "$(dirname "$CLI")" && pwd)/$(basename "$CLI")"
W="$(mktemp -d "${TMPDIR:-/tmp}/lz6ci.XXXXXX")"; trap 'rm -rf "$W"' EXIT
[ -x "$CLI" ] || { echo "no binary at $CLI"; exit 2; }
fail=0; bad() { echo "FAIL $1"; fail=1; }

# inputs: text (the sources), binary (the executable), structured 16-bit samples, zeros, noise, a repeat that crosses the 64 KB window
mkdir -p "$W/in"
cat "$ROOT"/lib/*.c "$ROOT"/lib/entropy/*.c "$ROOT"/programs/*.c > "$W/in/text"
cp "$CLI" "$W/in/binary"
python3 - "$W/in" <<'PY'
import sys, random, struct
d = sys.argv[1]; r = random.Random(1)
open(d + "/samples", "wb").write(b"".join(struct.pack("<h", int(2000 * ((i % 97) - 48) / 48) + r.randint(-9, 9)) for i in range(400000)))
open(d + "/zeros", "wb").write(bytes(3000000))
open(d + "/noise", "wb").write(bytes(r.getrandbits(8) for _ in range(1500000)))
blk = bytes(r.getrandbits(8) for _ in range(70000))
open(d + "/repeat", "wb").write(blk * 20)
open(d + "/one", "wb").write(b"x")
open(d + "/empty", "wb").write(b"")
PY

n=0
for f in "$W"/in/*; do
  for o in -0 --fast=2 --fast=1 -1 -3 -5 -8 -13 "--hc -9" "-1 -B3" "-1 -T4" "-3 -T0 -B4"; do
    "$CLI" $o -c "$f" > "$W/c.lz6" 2> "$W/err" || { bad "compress $(basename "$f") $o: $(head -c 200 "$W/err")"; continue; }
    "$CLI" -d -c "$W/c.lz6" 2> "$W/err" | cmp -s - "$f" || { bad "round trip $(basename "$f") $o"; continue; }
    "$CLI" -d -T3 -c "$W/c.lz6" 2> "$W/err" | cmp -s - "$f" || { bad "round trip -T3 $(basename "$f") $o"; continue; }
    "$CLI" -t "$W/c.lz6" > /dev/null 2>&1 || bad "-t $(basename "$f") $o"
    n=$((n+1))
  done
done
echo "round trips: $n"

# the output does not depend on the thread count
"$CLI" -1 -B3 -c "$W/in/text" > "$W/t1.lz6"; "$CLI" -1 -B3 -T4 -c "$W/in/text" > "$W/t4.lz6"
cmp -s "$W/t1.lz6" "$W/t4.lz6" || bad "-T4 writes other bytes than one thread"

# -c with several files: every frame goes to stdout, one after the other, and they decode as one stream
"$CLI" -1 -c -m "$W/in/text" "$W/in/binary" "$W/in/empty" | "$CLI" -dc | cmp -s - <(cat "$W/in/text" "$W/in/binary" "$W/in/empty") || bad "-c -m to stdout"
"$CLI" -1 -c -m "$W/in/text" "$W/in/binary" > "$W/m.lz6"; "$CLI" -dc -m "$W/m.lz6" "$W/m.lz6" | cmp -s - <(cat "$W/in/text" "$W/in/binary" "$W/in/text" "$W/in/binary") || bad "-dc -m to stdout"

# the output may not be the input (it would be emptied before it is read), nor a link to it
cp "$W/in/text" "$W/same"; cp "$W/in/text" "$W/same.0"; ln "$W/same" "$W/same.hard" 2>/dev/null
"$CLI" -1 -f -q "$W/same" "$W/same" 2>/dev/null && bad "compress over its own input"
"$CLI" -1 -f -q "$W/same" "$W/same.hard" 2>/dev/null && [ -e "$W/same.hard" ] && bad "compress over a link to its input"
cmp -s "$W/same" "$W/same.0" || bad "input changed after the refused overwrite"

# the command-line checks: stand-ins for the Silesia files xml, mr and x-ray
mkdir -p "$W/sil"; cp "$W/in/text" "$W/sil/xml"; cp "$W/in/binary" "$W/sil/mr"
cat "$W/in/samples" "$W/in/samples" "$W/in/samples" "$W/in/samples" > "$W/sil/x-ray"
[ "$(uname)" = Linux ] && ldd "$CLI" > /dev/null 2>&1 && export CLI_DYN="$CLI"   # read-error injection needs a dynamic build
SILESIA="$W/sil" CLITEST_WORK="$W/clitest" bash "$ROOT/bakeoff/clitest.sh" "$CLI" | tail -40 ; [ "${PIPESTATUS[0]}" = 0 ] || bad "clitest"

[ $fail -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
