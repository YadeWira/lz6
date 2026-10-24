# libFuzzer harnesses

Coverage-guided fuzzing of the decoders and the encoders, ASAN + UBSan, clang.

| harness | what it feeds | invariants |
|---|---|---|
| `fz_seq_dec` | arbitrary bytes to `LZ6_decompress_seq` (exact-size input and output buffers) | no overrun; the result never exceeds the capacity; more spare room gives the same output |
| `fz_block_dec` | arbitrary bytes to `LZ6_decompress_safe`, the plain block format | no overrun, not even of the input by one byte |
| `fz_frame_dec` | arbitrary bytes to the streaming `LZ6F_decompress`, chunked as the input says | no overrun, no unbounded output |
| `fz_seq_rt` | `LZ6_compress_seq` then `LZ6_decompress_seq` at a level chosen by the first byte | round trip; the documented capacity always suffices; a too-small capacity never overruns |
| `fz_frame_rt` | `LZ6F_compressFrame` / streaming (with and without flushes) with random preferences, then chunked `LZ6F_decompress` | round trip; the frame ends exactly where the data does |

`build.sh` builds them, `seeds.py` makes seed corpora from real files, `regress/` holds the inputs that crashed a build:
`block_oob_read_1byte.bin`, `block_token_only.bin` and `block_token_only_run2.bin` (a block cut right after its token read one byte past
its end: the first extension byte of a long literal run was not bounds-checked), `block_empty_input.bin` (no input at all).
The gate runs the same cases without libFuzzer as `bakeoff/gate/blockdec.c`.
