#!/usr/bin/env python3
"""Publish web/results.json (from tools/site_bench.py) for the results page.

Adds a note for every bundled protocol without runs, and the verifier commit
the data were measured with (results.json's "verifier", else the last commit
that changed the verifier's sources).

usage: results_json.py web/results.json protocols site/results/results.json
"""
import json
import subprocess
import sys
from pathlib import Path

src, protocols, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
data = json.loads(src.read_text()) if src.exists() else {"runs": []}

# Protocols kept in protocols/ but not published on the website (results page
# and playground): their runs with input errors do not finish in reasonable time.
HIDDEN = {"CB18_[[35,1,5]]_plain", "CB18_[[45,1,5]]_plain", "CB18_[[49,1,7]]_plain"}
data["runs"] = [r for r in data["runs"] if r["protocol"] not in HIDDEN]
ran = {r["protocol"] for r in data["runs"]}
data["protocols"] = {}
for d in sorted(p.parent for p in protocols.glob("*/*.fpdl")):
    if d.name not in ran and d.name not in HIDDEN:
        data["protocols"][d.name] = "not run yet"
if "verifier" not in data:
    try:
        data["verifier"] = subprocess.run(["git", "log", "-1", "--format=%h", "--", "src", "include", "third_party"], capture_output=True,
                                          text=True, check=True).stdout.strip()
    except Exception:
        data["verifier"] = ""
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(data, separators=(",", ":")))
print(f"{out}: {len(data['runs'])} runs, {len(data['protocols'])} protocols without runs")
