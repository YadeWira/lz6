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

# the library compresses a block that begins with incompressible bytes (until v2.0.1 its first 32 KB decided for the whole block when
# the caller gave room for a stored block; the command line gives less room and was not affected, so this goes through -b)
head -c 100000 "$W/in/noise" > "$W/noisetext"; cat "$W/in/text" >> "$W/noisetext"
ts=$(wc -c < "$W/in/text")
for o in -1 -8 --fast=2; do
  bs=$("$CLI" -b -i1 $o "$W/noisetext" 2>&1 | tr '\r' '\n' | grep -a -E ' -> +[0-9]+ \(' | tail -1 | sed -E 's/.*-> *([0-9]+) .*/\1/')
  [ -n "$bs" ] && [ "$bs" -lt $((100000 + ts / 2)) ] || bad "noise then text -b $o: $bs bytes of $((100000 + ts))"
  "$CLI" $o -c "$W/noisetext" | "$CLI" -d -c | cmp -s - "$W/noisetext" || bad "round trip noise then text $o"
done
# encoder choices that used to depend on where the data sat in the block (v2.0.2); sizes through -b (one block, the library's room)
bsize() { "$CLI" -b -i1 $1 "$2" 2>&1 | tr '\r' '\n' | grep -a -E ' -> +[0-9]+ \(' | tail -1 | sed -E 's/.*-> *([0-9]+) .*/\1/'; }
python3 - "$W" <<'PY'
import sys, random
w = sys.argv[1]; r = random.Random(7)
t = open(w + "/in/text", "rb").read(); n = open(w + "/in/noise", "rb").read(); b = open(w + "/in/binary", "rb").read()
nib = lambda k: bytes(r.getrandbits(4) for _ in range(k))
big = bytes(r.getrandbits(8) for _ in range(3000000)) + n[::-1] + n[1::2] + n[::3]   # 5.75 MB without repeats
open(w + "/u16", "wb").write(t[:len(t) & ~1].decode("latin-1").encode("utf-16-le"))   # UTF-16 text (one lane is all zeros), length % 4 == 0
open(w + "/nibnoisenib", "wb").write(nib(65536) + n + nib(500000))       # mostly literals, of two kinds
open(w + "/textnoise2", "wb").write(t[:65536] + n + n)                   # high entropy everywhere but the start, and a repeat
open(w + "/tn", "wb").write(t[:65536] + big)                             # a long incompressible run...
open(w + "/tnb", "wb").write(t[:65536] + big + b)                        # ...and the same followed by a binary
PY
a=$(bsize -8 "$W/u16"); b=$(bsize -8 "$W/in/text")
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -le $((b + b / 5)) ] || bad "UTF-16 text at -8: $a bytes against $b for the same text in 8 bits"
a=$(bsize -2 "$W/nibnoisenib"); b=$(bsize -1 "$W/nibnoisenib")
[ -n "$a" ] && [ -n "$b" ] && [ "$a" -le $((b + b / 50)) ] || bad "noise between 4-bit symbols: $a bytes at -2, $b at -1 (literal epochs dropped)"
a=$(bsize -1 "$W/textnoise2"); b=$(wc -c < "$W/textnoise2")
[ -n "$a" ] && [ "$a" -le $((b * 3 / 4)) ] || bad "text, then noise twice: $a bytes of $b (the repeat was not found: block stored?)"
a=$(bsize -3 "$W/tnb"); b=$(bsize -3 "$W/tn"); c=$(bsize -3 "$W/in/binary")
[ -n "$a" ] && [ -n "$b" ] && [ -n "$c" ] && [ $((a - b)) -le $((c + c / 25)) ] || bad "a binary after 5.75 MB of noise at -3: $((a - b)) bytes against $c alone"
for f in u16 nibnoisenib textnoise2 tnb; do for o in -1 -5 -8; do
  "$CLI" $o -c "$W/$f" | "$CLI" -d -c | cmp -s - "$W/$f" || bad "round trip $f $o"
done; done
# the seq test vectors (bakeoff/seqvec) decode with this tree, and LZ6_compress_seq_opt keeps to its masks
LIBSRC="$ROOT/lib/lz6.c $ROOT/lib/lz6hc.c $ROOT/lib/lz6frame.c $ROOT/lib/xxhash.c $ROOT/lib/entropy/lz6fse.c $ROOT/lib/entropy/lz6huf.c $ROOT/lib/entropy/lz6seq.c"
SV="$ROOT/bakeoff/seqvec"
if ${CC:-cc} -O1 -I"$ROOT/lib" -I"$ROOT/lib/entropy" -o "$W/decchk" "$SV/decchk.c" $LIBSRC -lm 2> "$W/err" &&
   ${CC:-cc} -O1 -I"$ROOT/lib" -I"$ROOT/lib/entropy" -o "$W/encvec" "$SV/encvec.c" $LIBSRC -lm 2>> "$W/err" &&
   ${CC:-cc} -O1 -o "$W/inspect" "$SV/inspect.c" 2>> "$W/err"; then
  nv=0
  for s in "$SV"/vectors/*.seq; do
    o="${s%.seq}.orig"; [ -f "$o" ] || continue
    "$W/decchk" "$s" "$o" > /dev/null 2>&1 || bad "vector $(basename "$s") does not decode to its original"
    "$W/inspect" -s "$s" > /dev/null 2>&1 || bad "vector $(basename "$s"): the inspector cannot walk it"
    nv=$((nv+1))
  done
  [ "$nv" -ge 79 ] || bad "only $nv seq vectors found"
  echo "seq vectors: $nv"
  for f in text binary samples noise; do for lv in 1 5 9; do
    "$W/encvec" "$W/in/$f" 0 0 $lv 0x0 0x0 "$W/m.seq" "$W/m.orig" > /dev/null 2>&1 || { bad "encvec $f -$lv, no masks"; continue; }
    "$W/inspect" -s "$W/m.seq" | grep -q -E 'x86|plane|prng|m[135678]' && bad "$f -$lv with empty masks: $("$W/inspect" -s "$W/m.seq" | cut -c1-120)"
    "$W/encvec" "$W/in/$f" 0 0 $lv 0x0F 0x20 "$W/m.seq" "$W/m.orig" > /dev/null 2>&1 || { bad "encvec $f -$lv, mode 5 only"; continue; }
    "$W/inspect" -s "$W/m.seq" | grep -q -E 'm[13678]' && bad "$f -$lv with literal mode 5 only: $("$W/inspect" -s "$W/m.seq" | cut -c1-120)"
  done; done
else
  bad "the seq vector tools do not compile: $(head -c 300 "$W/err")"
fi
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

# a write that fails at the very end (the last bytes sit in the stdio buffer until the close) is an error too
if [ -w /dev/full ]; then
  "$CLI" -1 -q -c "$W/in/one" > /dev/full 2>/dev/null && bad "compress to a full disk reported success"
  "$CLI" -1 -q "$W/in/one" "$W/full.lz6"; "$CLI" -d -q -c "$W/full.lz6" > /dev/full 2>/dev/null && bad "decode to a full disk reported success"
  "$CLI" -d -f -q "$W/full.lz6" /dev/full 2>/dev/null && bad "decode into /dev/full reported success"
fi

# -b measures what the levels compress with: its sizes match the command line's (they were the classic codec's, 52.8% at -1)
head -c 400000 "$W/in/text" > "$W/bm.in"
for lv in -1 -5 --fast=2; do
  bsz=$("$CLI" -b -i1 $lv "$W/bm.in" 2>&1 | tr '\r' '\n' | grep -a -E ' -> +[0-9]+ \(' | tail -1 | sed -E 's/.*-> *([0-9]+) .*/\1/')
  csz=$("$CLI" $lv -c "$W/bm.in" | wc -c)
  [ -n "$bsz" ] && [ "$(( csz - bsz ))" -ge 0 ] && [ "$(( csz - bsz ))" -le 64 ] || bad "-b $lv size $bsz against the command line's $csz"
done
"$CLI" -b -i1 2>/dev/null && bad "-b without a file name reported success"

# skippable frames in the stream: from a file, and from a pipe (which cannot seek: the bytes are read and dropped)
python3 - "$W/skip.lz6" "$W/in/text" "$CLI" <<'P'
import sys,struct,subprocess
out,src,cli=sys.argv[1:4]
fr=subprocess.run([cli,'-1','-q','-c',src],capture_output=True,check=True).stdout
sk=lambda n,sz: struct.pack('<II',0x184D2A50+n,sz)+bytes(range(256))*(sz//256)+bytes(sz%256)
open(out,'wb').write(sk(0,100000)+fr+sk(7,0)+fr+sk(3,10)+fr+sk(15,70000))
P
cat "$W/in/text" "$W/in/text" "$W/in/text" > "$W/skip.exp"
for T in 1 4; do
  "$CLI" -d -q -T$T -c "$W/skip.lz6" | cmp -s - "$W/skip.exp" || bad "skippable frames from a file -T$T"
  cat "$W/skip.lz6" | "$CLI" -d -q -T$T | cmp -s - "$W/skip.exp" || bad "skippable frames from a pipe -T$T"
done

# the command-line checks: stand-ins for the Silesia files xml, mr and x-ray
mkdir -p "$W/sil"; cp "$W/in/text" "$W/sil/xml"; cp "$W/in/binary" "$W/sil/mr"
cat "$W/in/samples" "$W/in/samples" "$W/in/samples" "$W/in/samples" > "$W/sil/x-ray"
[ "$(uname)" = Linux ] && ldd "$CLI" > /dev/null 2>&1 && export CLI_DYN="$CLI"   # read-error injection needs a dynamic build
SILESIA="$W/sil" CLITEST_WORK="$W/clitest" bash "$ROOT/bakeoff/clitest.sh" "$CLI" | tail -40 ; [ "${PIPESTATUS[0]}" = 0 ] || bad "clitest"

[ $fail -eq 0 ] && echo "PASS" || { echo "FAILED"; exit 1; }
