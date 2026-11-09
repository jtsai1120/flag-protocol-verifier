#!/usr/bin/env python3
"""Bundle every protocol under protocols/ into one JSON file for the playground.

    web/bundle_protocols.py protocols tests/expected.txt site/playground/protocols.json

Each entry carries the protocol's files (the .fpdl and every .qasm beside it),
its code parameters, the expected verdict when tests/expected.txt has one, and
roughly how long a native run takes, so the page can warn before a long run.
"""
import json
import re
import sys
from pathlib import Path

# Native wall time in seconds (ftec-verify --fqcv=image, 2 vCPU), measured on
# the protocols as shipped. None: did not finish within 2000 s.
NATIVE_SECONDS = {
    "CB18_[[17,1,5]]_plain": 169,
    "CB_18[[25,1,5]]_plain": 212,
    "CB18_[[25,1,5]]_gen": 212,
    "CB18_[[35,1,5]]_gen_5x7": 750,
    "CB18_[[45,1,5]]_gen_5x9": 2240,
    "CB18_[[49,1,7]]_gen": None,
    "Du24_[[17,1,5]]_plain": 340,
    "Du24_[[17,1,5]]": 238,
    "Du24[[19,1,5]]": 507,
    "LL25_[[17,1,5]]_[1,1,1,1,1,...]^T": 251,
    "LL25_[[17,1,5]]_[2,2,2,1,1]^T": 304,
    "LL25_[[19,1,5]]_[1,1,1,...]^T": 456,
    "LL25_[[19,1,5]]_[2,2,2,1,1,1]^T": 543,
}


# Verdicts of the larger protocols, from the same native runs (tests/expected.txt
# covers only distance 3).
KNOWN_VERDICTS = {
    "Du24_[[17,1,5]]_plain": "min_t=2 failures=2314",
    "Du24_[[17,1,5]]": "min_t=2 failures=2314",
    **{name: "min_t=-1 failures=0" for name in (
        "CB18_[[17,1,5]]_plain", "CB_18[[25,1,5]]_plain", "CB18_[[25,1,5]]_gen",
        "CB18_[[35,1,5]]_gen_5x7", "CB18_[[45,1,5]]_gen_5x9", "Du24[[19,1,5]]",
        "LL25_[[17,1,5]]_[1,1,1,1,1,...]^T", "LL25_[[17,1,5]]_[2,2,2,1,1]^T",
        "LL25_[[19,1,5]]_[1,1,1,...]^T", "LL25_[[19,1,5]]_[2,2,2,1,1,1]^T")},
}


def main():
    root, expected_file, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    expected = {}
    for line in expected_file.read_text().splitlines():
        if line and not line.startswith("#") and "\t" in line:
            name, summary = line.split("\t", 1)
            expected[name] = summary

    protocols = []
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        fpdls = sorted(d.glob("*.fpdl"))
        if not fpdls:
            continue
        files = {f.name: f.read_text() for f in sorted(d.iterdir())
                 if f.is_file() and f.suffix in (".fpdl", ".qasm")}
        entry = fpdls[0].name
        m = re.search(r"\[\[(\d+),(\d+),(\d+)\]\]", files[entry])
        n, k, dist = (int(x) for x in m.groups()) if m else (0, 0, 0)
        protocols.append({
            "name": d.name,
            "entry": entry,
            "n": n, "k": k, "d": dist,
            "expected": expected.get(d.name) or KNOWN_VERDICTS.get(d.name),
            "native_seconds": NATIVE_SECONDS.get(d.name, 0.1 if dist <= 3 else None),
            "files": files,
        })
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(protocols, ensure_ascii=False))
    print(f"{out}: {len(protocols)} protocols")


if __name__ == "__main__":
    main()
