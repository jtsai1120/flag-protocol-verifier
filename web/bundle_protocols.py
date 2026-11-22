#!/usr/bin/env python3
"""Bundle every protocol under protocols/ into one JSON file for the playground.

    web/bundle_protocols.py protocols tests/expected.txt site/playground/protocols.json

Each entry carries the protocol's files (the .fpdl and every .qasm beside it),
its code parameters, and, from the results page's `table` run (web/results.json),
the three verdicts the results page shows (circuit validity, protocol validity,
correctability) and how long the native run took, so the page lists the same
verdicts and can warn before a long run. tests/expected.txt fills in a verdict
for a protocol that has no run yet.
"""
import json
import re
import sys
from pathlib import Path

# Native wall time in seconds (ftec-verify --fqcv=image, 2 vCPU), measured on
# the protocols as shipped. None: did not finish within 2000 s.
NATIVE_SECONDS = {
    "CB18_[[17,1,5]]_plain": 176,
    "CB18_[[25,1,5]]_plain": 212,
    "Du24_[[17,1,5]]_plain": 307,
    "Du24_[[17,1,5]]": 153,
    "Du24[[19,1,5]]": 507,
    "LL25_[[17,1,5]]_[1,1,1,1,1,...]^T": 267,
    "LL25_[[17,1,5]]_[2,2,2,1,1]^T": 304,
    "LL25_[[19,1,5]]_[1,1,1,...]^T": 456,
    "LL25_[[19,1,5]]_[2,2,2,1,1,1]^T": 543,
}




# The run fields the playground shows; the same ones the results page's
# verdict columns read.
VERDICT_FIELDS = ("circuit_valid", "circuit_problems", "circuit_warnings", "circuits_skipped",
                  "invalid", "invalid_paths", "flag_bugs", "failures", "min_t", "ftec", "status")


def main():
    root, expected_file, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    expected = {}
    for line in expected_file.read_text().splitlines():
        if line and not line.startswith("#") and "\t" in line:
            name, summary = line.split("\t", 1)
            expected[name] = summary

    # The results page's table runs (tools/site_bench.py), when there are
    # any, are the newest verdicts and timings; the lists above fill the gaps.
    verdicts, seconds = {}, {}
    if len(sys.argv) > 4 and Path(sys.argv[4]).exists():
        for run in json.loads(Path(sys.argv[4]).read_text())["runs"]:
            if run.get("config") != "table":
                continue
            verdicts[run["protocol"]] = {k: run[k] for k in VERDICT_FIELDS if k in run}
            if run.get("status") == "ok" and "total_s" in run:
                seconds[run["protocol"]] = run["total_s"]

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
            "verdicts": verdicts.get(d.name),
            "expected": expected.get(d.name),
            "native_seconds": seconds.get(d.name, NATIVE_SECONDS.get(d.name, 0.1 if dist <= 3 else None)),
            "files": files,
        })
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(protocols, ensure_ascii=False))
    print(f"{out}: {len(protocols)} protocols")


if __name__ == "__main__":
    main()
