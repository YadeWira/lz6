# Seq Stream Format

This is the byte layout of one block produced by `LZ6_compress_seq`, as read by `LZ6_decompress_seq` (`lib/entropy/lz6seq.c`). All multi-byte integers are little-endian unless marked otherwise. The frame layer that carries these blocks (block codec flag, per-block dispatch, content segmentation) is in [lz6_Frame_format.md](lz6_Frame_format.md); the plain byte-oriented block format is in [lz6_Block_format.md](lz6_Block_format.md).

## Stability

As of **v1.7.0 this format is frozen**: every block that v1.6.11-pre or any later version writes decodes with every later version, and the decoder keeps accepting every layout described below (the x86 filter blocks, the plane blocks with and without delta lanes, the stored and PRNG blocks, all literal modes). Blocks written before v1.6.11-pre (which used wider length-code widths) do not decode; that change, in v1.6.11-pre, was the last one without a compatibility shim. History of the `-pre` series: seq v2 in v1.6.4-pre; offset codes and table header revised in v1.6.6-pre; 4-stream Huffman in v1.6.8-pre; the x86 filter block in v1.6.9-pre; the length-code widths of the present table in v1.6.11-pre; delta lanes in v1.6.12-pre (an extension: every earlier stream is unchanged).

What the encoder writes for the same input is **not** part of the promise: levels are retuned between versions and builds of different pointer widths (32-bit and 64-bit) choose differently; any stream the format allows decodes.

## Limits

A decoder rejects a block that exceeds any of these, and an encoder must stay inside them (`LZ6_compress_seq` rewrites a parse that would not, see NEWS v1.7.0):

| quantity | limit |
|---|---|
| literal run of one sequence | `LL_base[35] + 2^24 - 1` = 16,777,471 (the last code is open-ended with 24 extra bits) |
| match length of one sequence | `ML_base[35] + 2^24 - 1` = 16,777,474 (same) |
| sequences in a block (`sc`) | 2^24 |
| match offset | below 2^25 (bucket 24); and not beyond the bytes produced so far |
| declared output size `isize` | at most the capacity the caller gives, and 2^31 - 1 |
| minimum match | 3 |

A block of any size up to those limits is valid; the frame layer in practice carries blocks of at most 256 MB (`-B7`).

Notation:
- `vlq` is a 255-continuation integer: add bytes while the byte equals 255.
- An **rANS stream** is `[table][slen:4][x:4, big-endian][slen-4 bytes]`, where `x` is the initial decoder state.
- A **table** is `[L_bits:1][maxSym:1][3 bytes per symbol 0..maxSym: normalised frequency]`. Frequencies sum to `M = 2^L_bits`.

## Block header

```
[flags:1][isize:4]
```

| flags | meaning | payload |
|---|---|---|
| bit 4 set | x86-filtered block (since v1.6.9-pre) | see *x86 filter blocks* below |
| bit 3 set | byte-plane block | see *Plane blocks* below |
| bit 2 set | PRNG regeneration (tested before bit 0: here bits 0-1 are the byte shift, not the stored flag) | `[seed:2]`. After `srandom(seed)`, byte `i` of the output = `random() >> (8 * (flags & 3))` truncated to a byte, with `random()` glibc's TYPE_3 additive generator (the 310 outputs after seeding discarded). Written by the encoder for a block that matches such an output for a seed 0..65535 at a shift of 0, 8 or 16 bits, and decoded for any shift. |
| bit 0 set | stored | `isize` raw bytes |
| otherwise | normal seq block | the sections below, in order |

## 1. Literals

```
[lit_mode:1][lit_count:vlq] ...
```

| lit_mode | name | layout after `lit_count` |
|---:|---|---|
| 0 | raw | `lit_count` bytes |
| 1 | FSE order-0 | `[csize:vlq]` + rANS stream |
| 5 | Huffman | `[csize:vlq][k:1][256 code lengths][total_bits:4][MSB-first bitstream]`. Canonical codes, lengths ≤ 16, lookup table of `k ≤ 12` bits |
| 7 | Huffman, 4 streams (since v1.6.8-pre) | `[csize:vlq][k:1][256 code lengths]` then 4 × `[total_bits:4][MSB-first bitstream]`, one code table. Stream `j` holds literals `j·s .. min((j+1)·s, lit_count)`, with `s = ceil(lit_count/4)`. The encoder uses it from 4096 literals up |
| 6 | FSE order-1, 16 contexts | `[csize:vlq][L_bits:1][16 × (L_bits:1 + 256×freq:2)][slen:4][x:4 BE][stream]`. Context = previous literal `>> 4` |
| 3 | FSE order-1, 256 contexts | `[csize:vlq][active bitmap:32][L_bits:1][fallback table][table per active context][slen:4][x:4 BE][stream]`. Context = previous literal; inactive contexts use the fallback table |

## 2. Sequences (v2, since v1.6.4-pre)

```
[sc:4]
[ll table][ml table][of table]
[bits len:4][8 zero pad bytes][backward bitstream]      (present when sc > 0)
```

**Table** (one per symbol stream, table ANS as in zstd's FSE): `[log:1][maxSym:1]` then the normalised counts of symbols `0..maxSym`. `log` is 5..12 and the counts sum to `2^log`. Each count is a varint: a byte `v < 128` is the count; otherwise the count is `(v & 127) | next_byte << 7`. A count of 0 is followed by one byte `r`: `r` more symbols also have count 0.
- Spread: position `pos` walks `(pos + (size/2 + size/8 + 3)) & (size-1)`, and symbol `s` fills `norm[s]` positions in order.
- Decode entry `u`: `x = next[s]++` (`next` starts at `norm[s]`), `nbBits = log - highbit(x)`, `newState = (x << nbBits) - size`.

Alphabets:
- **ll** (≤ 35): literal-length code. Value = `LL_base[c]` + `LL_extra[c]` extra bits. Codes 0..15 are one value each (no extra bits); code `c` in 16..34 covers `[LL_base[c], LL_base[c+1])` with `log2(step)` extra bits, i.e. `1,1,1,1,2,2,2,2,3,3,3,4,4,4,4,5,5,5,5` for codes 16..34; code 35 is the open-ended escape with 24 bits (v1.6.11-pre; before it the widths of codes 20..34 were wider than the step, so most of those raw bits were always zero).
- **ml** (≤ 36): `0` = *no match* (the last sequence carries only literals). Otherwise code `c-1` gives `ML_base[c-1]` + `ML_extra[c-1]` extra bits. Minimum match is 3. The ml extra widths are the same as ll's (`1,1,1,1,2,2,2,2,3,3,3,4,4,4,4,5,5,5,5` for codes 16..34, 24 for the escape).
- **of** (≤ 193): `0..190` = new offset, `191..193` = rep0 / rep1 / rep2.
  - Codes `0..30`: offsets 1..31 exactly (code `c` = offset `c+1`), no extra bits.
  - Codes `31..190`: bucket `b = 5..24` (`b = floor(log2 offset)`) and the offset's low 3 bits `j`: code = `31 + 8*(b-5) + j`. Then `b-3` raw bits `m` follow, and offset = `2^b + j + (m << 3)`.
  - Keeping the low 3 bits in the entropy-coded symbol captures the alignment of structured data (record strides, 4- and 8-byte fields), which raw bits cannot.

The rep stack starts at `{0,0,0}`:
- A rep1/rep2 hit moves that entry to the front.
- A new offset (code ≤ 190) is always pushed onto the stack. The encoder codes an offset equal to rep0 as rep0, so a new-offset code equal to rep0 never occurs in its streams; a decoder still pushes it like any other (the reference decoder before v1.6.11 skipped the push in that case, which cost 2-4% of its instructions for a case no encoder produces; `bakeoff/gate/rpzero.c` pins the rule).

A block is valid only if its sequences produce **exactly the declared size** (the literals-only last sequence closes it; a block whose sequences stop short is rejected, where v1.6.10-pre returned the shorter length) and the bitstream is consumed exactly.

**Bitstream.** It is written forward, LSB-first, and read **backward** from its last byte. The highest set bit of the last byte is an end marker. The 8 zero bytes in front keep every 8-byte load in bounds; a valid stream consumes exactly its bits and none of the padding.

The reader first takes the three initial states: ll (`log_ll` bits), of (`log_of`), ml (`log_ml`). Then, for each sequence `n`:
1. Its symbols are the table entries at the current ll/ml/of states.
2. If ml ≠ 0 and of ≤ 190: read the offset's raw bits (`b-3` for codes ≥ 31, none below).
3. If ml ≠ 0: read the ml extra bits.
4. Read the ll extra bits.
5. If `n` is not the last sequence: update the states in the order ll, ml, of, each as `newState + read(nbBits)`.

The encoder writes all of this in exact reverse: it runs from the last sequence to the first, encoding the of, ml and ll symbols, then writing the ll, ml and of extras.

## Sequence semantics

For each sequence in order:
1. Copy `ll` literals.
2. If `ml == 0`, stop.
3. Otherwise copy `ml` bytes from `offset` bytes back. The copy may overlap, and the offset must be ≤ bytes produced so far.

There is no external dictionary: each block is self-contained.

## x86 filter blocks

```
[flags: 0x10][isize:4][nr:vlq][nr x (start:4, len:4)][inner block]
```

- The **inner block** is a complete seq block of the *filtered* data (normal, plane or stored; never another x86 block) whose `isize` equals the outer one.
- Each `(start, len)` is a byte range of the output (sorted, non-overlapping, `len >= 5`, inside `isize`).
- To decode: decode the inner block, then apply the **inverse E8/E9 filter** in place to each range, with `now_pos = start`. The filter is the classic x86 branch converter of xz / 7-Zip (CALL/JMP rel32 <-> absolute, with its `prev_mask` state machine); it is a bijection on any byte string, and its state resets at the start of every range.
- The encoder picks ranges from 64 KB chunks with a high density of E8/E9 opcodes followed by a 00/FF byte, and keeps one only if a level-1 trial of its first 512 KB is at least 0.5% smaller when filtered.

## Plane blocks

```
[flags: 0x08 | s][isize:4]  then per lane: [mode:1][...]
```

- Stride is 4 if `(flags & 3) == 2`, else 2. `isize` is a multiple of the stride.
- Lane `j` holds bytes `j, j+stride, j+2·stride, ...`

| lane mode | payload |
|---:|---|
| 0 | raw: `isize/stride` bytes |
| 1 | `[csize:4]` + a normal seq block (header included) |
| 2 | `[csize:4]` + a Huffman stream (as literal mode 5, without `lit_count`) |

**Delta lanes (v1.6.12-pre, a format extension).** Bit 7 of the lane mode byte flags a lane stored as **byte deltas**: bits 0-6 are the mode above (0, 1 or 2; any other value fails the block), and after the lane is decoded its bytes are summed back up in place: `b[0] = d[0]`, `b[j] = b[j-1] + d[j]` (mod 256), a prefix sum. Every stream that does not use the flag is unchanged and every v1.6.11-pre stream decodes; v1.6.11-pre's decoder rejects a lane mode with bit 7 set, so a stream with delta lanes needs v1.6.12-pre. The encoder tries deltas from level 2 up (v1.6.12-pre numbering), only when the deltas' order-0 entropy is at least 0.3 bit lower than the lane's, and keeps them when the lane comes out 3% smaller (a lane that was compressed already) or 20% smaller (a raw lane, which would turn a memcpy into an LZ decode). Effect: `mr` -5.7% at every level from 2, `x-ray` -2.2% at level 13, smooth 16-bit and 32-bit data (a random walk) -49% and -63%.
