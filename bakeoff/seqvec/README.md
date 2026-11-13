# seqvec: test vectors for decoders of the seq stream format

80 seq blocks with their originals, for anyone who writes a decoder of [lz6_Seq_format.md](../../lz6_Seq_format.md)
without the library. Each vector is one block as `LZ6_decompress_seq` takes it (no frame around it):

- `vectors/NAME.seq` is the block, `vectors/NAME.orig` the bytes it must decode to.
- `MANIFEST.tsv` has one line per vector: name, sizes, SHA-256 of both files, how it was made, and the
  inspector's one-line summary of what the block contains.
- `VERIFY.tsv` is the result of the last verification (see below).

The whole directory is about 5.5 MB. Most originals are 100 KB or less; the largest is 196,608 bytes.

## The three kinds of vectors

| prefix | count | made by |
|---|---:|---|
| `a_` | 37 | `mkvec.c`, a hand assembler that shares no code with the library's encoder. These are the cases the encoder never writes |
| `e_` | 32 | the real encoder through `LZ6_compress_seq_opt` (`encvec.c`), with the masks and level named in the manifest |
| `f1_` | 11 | the encoder of v1.7.1 (`git archive v1.7.1 lib`), which writes format 1 |

Two vectors have an empty original (`a_sc0_empty`, `a_empty_stored`). `LZ6_decompress_seq` returns 0 for them,
which is also its error value, so the library's verdict on those two cannot be observed through the API.

`a_big_offsets` is the only block over 16 MiB (33,558,752 bytes of output from a 1,028-byte block). Its
original is not stored. Byte `i` of it is `((r * 37) ^ (r >> 3)) & 255` with `r = i % 601`:

```sh
python3 -c "import sys; d=bytes((((r*37)^(r>>3))&255) for r in range(601)); n=33558752; sys.stdout.buffer.write((d*(n//601+1))[:n])" > a_big_offsets.orig
```

or `mkvec -big a_big_offsets.orig`. Its SHA-256 is in the manifest.

## What is covered

Block kinds:

- **Normal, format 2**: all `a_` and most `e_` vectors.
- **Normal, format 1**: `f1_lit5`, `f1_lit7`, `f1_lit7_fast`, `f1_lit6`, and inside the `f1_` x86 and plane blocks.
- **Stored**: `e_stored`, `e_prng_masked`, `f1_stored`, the empty block `a_empty_stored`, and as an inner block
  in `a_x86_stored` and `a_plane_mixed`.
- **PRNG**: shifts 0, 8, 16 from the encoder (`e_prng_s0`, `_s8`, `_s16`, `f1_prng`); shift 24 and seed 0 by
  hand (`a_prng_shift24`, `a_prng_seed0`); as a lane's block in `a_plane2_flags*`.
- **x86**: real filtered code in `e_x86`, `e_x86_lit8`, `f1_x86` (one range each). By hand, with data the filter
  leaves alone: two touching ranges around a stored block (`a_x86_stored`), and around a plane block (`a_x86_plane`).
- **Plane, stride 2**: `e_plane2` (no delta), `e_plane2_delta`, `e_plane2_lit8`, `e_plane2_G`, `f1_plane2_w16`,
  and `a_plane2_flags0/1/3` for the three flag values that all mean stride 2.
- **Plane, stride 4**: `e_plane4` (no delta), `e_plane4_delta`, `e_plane4_mr`, `e_plane4_E`, `e_plane4_huf`
  (a Huffman lane, mode 2), `f1_plane4_*`, and `a_plane_mixed`.
- **Lanes**: raw, seq block and Huffman, each with and without the delta bit (`a_plane_mixed` has raw + delta,
  block + delta and Huffman + delta in one block; the encoder's vectors have block + delta).

Literal modes:

| mode | from the encoder | by hand |
|---|---|---|
| 0 raw, count > 0 | `e_nomask`, `e_epochs_repeat` | most `a_` vectors |
| 1 rANS order 0 | `e_lit1`; nested in `e_lit8_m1`, `e_lit8_m0m1` | none |
| 3 rANS order 1, 256 contexts | `e_lit3` | none |
| 5 Huffman | `e_lit5`, `e_all_small`, `f1_lit5`, plane lanes | one symbol, lengths up to 16, all 256 symbols, large length jumps, split runs (`a_huf_*`) |
| 6 rANS order 1, 16 contexts | `e_lit6`, `f1_lit6` | none |
| 7 Huffman, 4 streams | `e_lit7`, `e_all_*`, `f1_lit7` | 5 literals with an empty fourth stream, 1001 literals (`a_huf4_*`) |
| 8 literal epochs | sections of mode 7 (`e_lit8_m7`), 5 (`e_lit8_m5`), 1 (`e_lit8_m1`), 0 and 1 (`e_lit8_m0m1`) | modes 0, 5, 7 mixed; one section; 256 sections; empty sections (`a_lit8_*`) |

Sequences (all by hand unless noted):

- `sc = 0` with `isize = 0` (`a_sc0_empty`); `sc = 1` with only literals (`a_lits_only`).
- The last sequence is a match (`a_last_match`, `a_offset1_run`, `a_one_symbol`, `a_1024_epochs` and others);
  an empty literals-only sequence after a match that completes the output (`a_empty_last_seq`).
- A new-offset code equal to rep0, where the push changes the output (`a_newoff_rep0`).
- rep0, rep1 and rep2 hits (`a_reps`, `a_zero_runs`, and every encoder vector).
- Every ll code 0..35 and every ml symbol 0..36 (`a_all_codes`).
- Every new-offset code 0..190: codes 0..126 in `a_all_codes`, codes 127..190 in `a_big_offsets`.
- Escape codes with 24 extra bits: ll 70,000 (`a_huf_one_symbol`), ml 99,999 (`a_offset1_run`), and the longest
  match the format allows, 16,777,474, twice (`a_big_offsets`).
- The largest offset, 2^25 - 1, and offsets just above 2^24 (`a_big_offsets`).
- A match of nearly the whole block at offset 1 (`a_offset1_run`).
- Table logs 5 to 12 in each of ll, ml, of (`a_logs`, `a_1024_epochs`); the encoder's vectors use 7 to 11.
- Tables of one symbol (maxSym 0 and maxSym > 0) and of two symbols (`a_lits_only`, `a_one_symbol`,
  `a_two_symbols`, `a_1024_epochs`).
- Long zero runs in a table header: gamma codes 34, 35 and 188 (`a_zero_runs`). 193 is the longest a table
  header can need, because the of alphabet ends at symbol 193.
- Rice parameters 0, 5 and 11 forced in table headers (`a_rice_k`).
- "Same table as the previous epoch" for each table alone, all three, and chained (`a_table_repeat`,
  `a_1024_epochs`; from the encoder in `e_epochs_repeat`, `e_x86_lit8`).
- 1024 epochs, the maximum, with `epochs = sc` (`a_1024_epochs`).
- Several epochs from the encoder: `e_epochs_repeat` (4), `e_x86_lit8` (3), `e_lit8_m1` (2).

## What is NOT covered

- **No negative vectors.** Every block here is valid. Nothing tests that a decoder rejects a bad block, and
  none of the entries of the specification's *Decoder checks and tolerances* table is exercised (spare bytes in
  a `csize`, trailing bytes, unused literals, non-shortest LEB128, incomplete Huffman codes).
- **rANS modes by hand.** `mkvec.c` has no rANS coder, so modes 1, 3 and 6 come only from the encoder, with
  `L_bits` = 12. Other `L_bits` values (1..16) are not covered. Mode 3 in format 1 is not covered.
- **Format 1 edge cases.** The assembler writes format 2 only. Format 1 is covered as far as v1.7.1 writes it:
  no `sc = 0`, no block that ends on a match, no rep0 push case, no raw or mode 1 literals in format 1.
- **x86 filter.** Only one real range per block, on pieces of `ooffice`. Two real ranges, `nr >= 255` (a
  two-byte `vlq`, which needs a block over 16.6 MB), and an inner PRNG block are not covered. The two hand
  vectors test the wrapper with data the filter does not change.
- **Sizes near the limits.** A literal run near 2^24, `sc` near 2^24, `isize` near 2^31, LEB128 counts of 4 or
  5 bytes: not covered. The longest literal run is 70,000 and the largest `lit_count` is about 200,000.
- **Gamma codes of 256 or more.** The specification allows up to 511, but no valid header can need more than
  255 (256 only in a Huffman header where no symbol has a code).
- **Plane blocks**: a Huffman lane in a stride 2 block; a lane block with spare bytes behind it.
- **Mode 8 with nested modes 3 or 6.**
- **Huffman codes longer than 12 bits from the encoder**: only by hand (`a_huf_deep`, `a_huf_jumps`).
- **32-bit builds of the encoder** (they choose differently; the decoder side is unaffected).

## Tools

| file | what it is |
|---|---|
| `inspect.c` | Stream inspector written from the specification, standard headers only. Walks one block and prints the tree of what it holds: block kind, x86 ranges, plane stride and each lane's mode and delta bit, and for a normal block the literal mode (per section in mode 8), `lit_count`, `sc`, and per epoch and table either "repeat" or the log and maxSym. `-s` prints the one-line summary used in the manifest. Exit status 1 with `ERROR: at byte N: ...` on anything it cannot walk. It parses headers and checks their limits; it does not decode symbols |
| `mkvec.c` | The hand assembler. `mkvec OUTDIR` writes the built-in `a_` cases; `mkvec -f DESC OUT.seq OUT.orig` assembles one block from a text description (literal bytes and mode, sequences, epoch layout and table logs; the format is in the comment above `parse_desc`). It calls only `LZ6_decompress_seq`, to check each block it writes |
| `encvec.c` | One vector from the real encoder: a piece of a file or a generated input, a level and the two masks |
| `decchk.c` | Decodes a vector with the library it is linked against and compares with the original |
| `pyref.py` | Runs zpaq-std's Python reference decoder (`lz6seq_ref.py`, not part of this repository) on vectors |
| `mk.sh` | Rebuilds everything: tools, the libraries of v1.7.1 and v2.0.0, all vectors, `MANIFEST.tsv` |
| `verify.sh` | Decodes every vector with each decoder and writes `VERIFY.tsv` |

## Rebuilding

```sh
bash mk.sh [WORK]              # WORK defaults to ${TMPDIR:-/tmp}/lz6-seqvec; nothing is built in the repository
bash verify.sh [WORK] [REFDIR] # REFDIR: a directory with a copy of lz6seq_ref.py (optional)
```

`mk.sh` is deterministic: two runs give the same `MANIFEST.tsv`. The `a_` vectors and the generated inputs
(`prng:`, `lcg:`, `markov:`, `markov16:`, `walk16:`, `walk32:`) depend only on the sources here and on glibc's
`random()`. The other `e_` and `f1_` vectors need the test data; set `SIL`, `AIT` and `AU` if it is not at the
default paths:

| variable | default | files used |
|---|---|---|
| `SIL` | `/mnt/IA_LAB/agentes/lz6/silesia` | `dickens`, `xml`, `sao`, `ooffice`, `mr` |
| `AIT` | `/mnt/IA_LAB/agentes/lz6/tmp_backup/ait_train` | `E`, `G` |
| `AU` | `/mnt/IA_LAB/agentes/lz6/v21/wf/audit2` | `dk4_xr4`, `rnd64k_aitA`, `T1_oo` |

The `e_` vectors are what the working tree's encoder writes today. The encoder's choices are not part of the
format promise, so a later version may write different bytes for the same input; the manifest then changes and
the vectors stay valid.

## Verification

`verify.sh` checks every vector against:

1. `LZ6_decompress_seq` of the working tree,
2. the library of v2.0.0 (all 80 vectors: format 2 has no feature newer than 2.0.0, and 2.0.0 reads format 1),
3. the library of v1.7.1 (the 11 `f1_` vectors),
4. zpaq-std's Python reference decoder in its default strict mode, when `REFDIR` is given.

Last run: all 80 vectors decode to their originals with every decoder that applies; no disagreement.
The vectors also decode cleanly under AddressSanitizer and UndefinedBehaviorSanitizer builds of the working tree.

## Where the specification did not decide the assembler's output

Points where `mkvec.c` had to pick something the specification leaves open or says only by implication. The
library and the Python reference accept all of them.

1. **tANS encoding.** The specification describes the decode table only. The encoder here is derived from it
   (search the entries of a symbol for the one whose range holds the next state). The state of the last
   sequence of an epoch may be any entry of its symbol; the lowest is taken. The normalisation of the counts is
   free as well.
2. **The of symbol of a literals-only sequence.** It reads no offset bits, but the sequence still has an of
   state, and the specification does not say which symbol it must show. The encoder uses symbol 0; the
   assembler uses the of symbol of the sequence before it (symbol 0 when there is none).
3. **An empty literals-only sequence after a match that completes the output** (`a_empty_last_seq`). The text
   says a literals-only sequence "must be the one that completes the output"; here the output is already
   complete and the sequence adds nothing. The reference decoder accepts it.
4. **Mode 7 with fewer literals than streams can share** (`a_huf4_tiny`). "Stream `j` holds literals
   `j*s .. min((j+1)*s, lit_count)`" gives a negative range for 5 literals and `j = 3`; the reference code clamps
   the start to `lit_count`, so that stream is empty (`total_bits = 0`, no bytes).
5. **Literal epochs with a section of count 0** (`a_lit8_empty_sec`). Not mentioned; accepted.
6. **Runs of absent symbols in a Huffman header cut shorter than needed** (`a_huf_split_runs`). The text only
   requires a split above 511; any split is accepted.
7. **Plane flags.** Stride 2 is every value of bits 0-1 other than 2. The encoder writes 0;
   `a_plane2_flags1` and `a_plane2_flags3` pin the other two.
