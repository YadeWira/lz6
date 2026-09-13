#!/usr/bin/env python3
"""Parse lzbench output (with \r progress redraws) into aggregated tables.

Usage: parse_lzbench.py RAW.txt [--sort ratio|dec|enc] [--csv OUT]

A codec run is identified by its long name + level (e.g. "lz6 1.6.2-pre
seq -3"). Per-file rows are size-weighted into one aggregate row.
"""
import re
import sys
import os

ROW = re.compile(
    r"^(?P<name>.+?)\s+(?P<enc>[0-9.]+)\s*MB/s\s+(?P<dec>[0-9.]+)\s*MB/s\s+"
    r"(?P<csize>[0-9]+)\s+(?P<ratio>[0-9.]+)\s+(?P<file>\S+)\s*$"
)


def parse(path):
    with open(path, "rb") as f:
        raw = f.read().decode("utf-8", "replace")
    raw = raw.replace("\r", "\n")
    rows = {}
    for line in raw.split("\n"):
        m = ROW.match(line.strip())
        if not m:
            continue
        name = m.group("name").strip()
        f = m.group("file")
        csize = int(m.group("csize"))
        ratio = float(m.group("ratio"))
        enc = float(m.group("enc"))
        dec = float(m.group("dec"))
        orig = csize * 100.0 / ratio if ratio > 0 else 0
        rows.setdefault(name, []).append((f, orig, csize, enc, dec))
    agg = {}
    for name, rs in rows.items():
        # dedupe: lzbench may print a file more than once across -i passes
        seen = {}
        for f, o, c, e, d in rs:
            seen[f] = (o, c, e, d)
        tot_o = sum(v[0] for v in seen.values())
        tot_c = sum(v[1] for v in seen.values())
        enc_w = sum(v[0] / v[2] for v in seen.values() if v[2] > 0)
        dec_w = sum(v[0] / v[3] for v in seen.values() if v[3] > 0)
        agg[name] = dict(
            files=len(seen), orig=tot_o, csize=tot_c,
            ratio=100.0 * tot_c / tot_o if tot_o else 0,
            enc=tot_o / enc_w if enc_w else 0,
            dec=tot_o / dec_w if dec_w else 0,
        )
    return agg


def main():
    path = sys.argv[1]
    sort_key = "ratio"
    csv_out = None
    args = sys.argv[2:]
    for i, a in enumerate(args):
        if a == "--sort":
            sort_key = args[i + 1]
        if a == "--csv":
            csv_out = args[i + 1]
    agg = parse(path)
    keyf = {"ratio": lambda k: agg[k]["ratio"],
            "dec": lambda k: agg[k]["dec"],
            "enc": lambda k: agg[k]["enc"]}[sort_key]
    names = sorted(agg, key=keyf)
    print(f"{'codec':<34}{'csize':>12}{'ratio':>8}{'enc MB/s':>10}{'dec MB/s':>10}  n")
    for n in names:
        a = agg[n]
        print(f"{n:<34}{a['csize']:>12,}{a['ratio']:>7.2f}%{a['enc']:>10.1f}{a['dec']:>10.1f}  {a['files']}")
    if csv_out:
        with open(csv_out, "w") as f:
            f.write("codec,orig,csize,ratio,enc_mbs,dec_mbs,files\n")
            for n in names:
                a = agg[n]
                f.write(f'"{n}",{int(a["orig"])},{a["csize"]},{a["ratio"]:.3f},'
                        f'{a["enc"]:.2f},{a["dec"]:.2f},{a["files"]}\n')


if __name__ == "__main__":
    main()
