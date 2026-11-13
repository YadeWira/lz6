#!/usr/bin/env python3
# pyref.py - runs zpaq-std's independent Python reference decoder (lz6seq_ref.py) on vectors.
#   pyref.py REFDIR [--lenient] NAME.seq NAME.orig [NAME.seq NAME.orig ...]
# REFDIR is a directory that holds a copy of lz6seq_ref.py. Prints one line per vector:
#   OK | FAIL output differs | REJECT <the decoder's message> | CRASH <exception>
# --lenient sets the module's STRICT switch to False (it then accepts what the C decoder tolerates).
import sys, time
sys.setrecursionlimit(10000)
args = sys.argv[1:]
sys.path.insert(0, args.pop(0))
import lz6seq_ref as R
if args and args[0] == "--lenient":
    R.STRICT = False
    args.pop(0)
bad = 0
for i in range(0, len(args), 2):
    blk = open(args[i], "rb").read()
    orig = open(args[i + 1], "rb").read()
    t = time.time()
    try:
        got = R.decode_block(blk)
        res = "OK" if bytes(got) == orig else "FAIL output differs (%d bytes, original %d)" % (len(got), len(orig))
    except R.FormatError as ex:
        res = "REJECT %s" % ex
    except Exception as ex:
        res = "CRASH %s: %s" % (type(ex).__name__, ex)
    if res != "OK":
        bad += 1
    print("%s\t%s\t%.1fs" % (args[i].split("/")[-1], res, time.time() - t), flush=True)
sys.exit(1 if bad else 0)
