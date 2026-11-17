#!/usr/bin/env python3
"""Run every bundled protocol for the website's results page.

Each run is one process of build/ftec-verify (so peak memory is that run's
alone); the verifier's own statistics are parsed with tools/bench_fqcv.py's
parser plus the validity lines. A protocol whose circuits fail the circuit
check is run a second time with --skip-circuit-check, so that the rest of its
row is still filled; its circuit-check columns come from the first run.

Results are merged into one JSON file keyed by (protocol, config), written
after every run, so the job can be stopped and resumed (--resume skips runs
already in the file).

    tools/site_bench.py --configs table,table-noval --jobs 2 --out web/results.json
    tools/site_bench.py --configs image,square,bruteforce --resume --out web/results.json

Configs: a method name (bruteforce, table, image, square) runs the full check;
"<method>-noval" adds --no-validity; "<method>-r0" adds --no-input-errors.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("bench_fqcv", REPO_ROOT / "tools" / "bench_fqcv.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)

NUM = r"([\d.]+)"
SIZE = bench.SIZE


def extra_fields(text: str) -> dict:
    row: dict = {}
    if m := re.search(rf"^circuit check\s*:\s*(valid|(\d+) problem\(s\)), (\d+) warning\(s\), {NUM} s, peak {SIZE}",
                      text, re.M):
        row["circuit_valid"] = m.group(1) == "valid"
        row["circuit_problems"] = int(m.group(2) or 0)
        row["circuit_warnings"] = int(m.group(3))
        row["circuit_s"] = float(m.group(4))
        row["circuit_peak_bytes"] = int(float(m.group(5)) * bench.UNIT_BYTES[m.group(6)])
    if m := re.search(r"^terminal records: Cond 1 (\d+), Cond 2 (\d+), Cond 3 (\d+)", text, re.M):
        row["cond1"], row["cond2"], row["cond3"] = (int(m.group(i)) for i in (1, 2, 3))
    if m := re.search(rf"^validity facts\s*:.*, {NUM} s", text, re.M):
        row["validity_facts_s"] = float(m.group(1))
    if m := re.search(rf"^flag condition\s*:.*, {NUM} s", text, re.M):
        row["flag_condition_s"] = float(m.group(1))
    if m := re.search(r"^FTEC\s*:\s*(\w+)", text, re.M):
        row["ftec"] = m.group(1)
    if m := re.search(r"^paths reached\s*:\s*(\d+) of (\d+)", text, re.M):
        row["paths"], row["paths_total"] = int(m.group(1)), int(m.group(2))
    return row


def flags_of(config: str) -> tuple[str, list[str]]:
    method, _, variant = config.partition("-")
    flags = {"": [], "noval": ["--no-validity"], "r0": ["--no-input-errors"]}[variant]
    return method, flags


def run(binary: Path, fpdl: Path, config: str, timeout: float, log_dir: Path,
        extra: list[str] | None = None) -> tuple[dict, str]:
    method, flags = flags_of(config)
    command = [str(binary), str(fpdl), f"--fqcv={method}", *flags, *(extra or [])]
    started = time.perf_counter()
    row: dict = {"status": "ok"}
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
        text, rc, err = done.stdout, done.returncode, done.stderr
    except subprocess.TimeoutExpired as exc:
        text = exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        rc, err = None, ""
        row["status"] = f"timeout (>{timeout:g} s)"
    row["wall_s"] = round(time.perf_counter() - started, 3)
    tag = config + ("-skip" if extra else "")
    (log_dir / f"{fpdl.parent.name}.{tag}.log").write_text(text + "\n--- stderr ---\n" + err)
    if rc not in (0, None) and row["status"] == "ok":
        row["status"] = "circuits not valid" if rc == 2 and "circuit validity" in err else f"exit {rc}"
    row.update(bench.parse(text))
    row.update(extra_fields(text))
    return row, text


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", type=Path, default=REPO_ROOT / "build" / "ftec-verify")
    ap.add_argument("--protocols", type=Path, default=REPO_ROOT / "protocols")
    ap.add_argument("--only", action="append", default=[], help="substring of protocol names")
    ap.add_argument("--skip", action="append", default=[], help="substring of protocol names to skip")
    ap.add_argument("--configs", default="table")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=3000)
    ap.add_argument("--out", type=Path, default=REPO_ROOT / "web" / "results.json")
    ap.add_argument("--logs", type=Path, default=Path("/tmp/site_bench_logs"))
    ap.add_argument("--resume", action="store_true")
    args = ap.parse_args()

    args.logs.mkdir(parents=True, exist_ok=True)
    results = json.loads(args.out.read_text()) if args.out.exists() else {"runs": []}
    done_keys = {(r["protocol"], r["config"]) for r in results["runs"] if r.get("status") == "ok"
                 or r.get("status", "").startswith(("timeout", "circuits"))}
    lock = threading.Lock()

    fpdls = sorted(p for p in args.protocols.glob("*/*.fpdl"))
    fpdls = [p for p in fpdls if (not args.only or any(s in p.parent.name for s in args.only))
             and not any(s in p.parent.name for s in args.skip)]
    # Small protocols first; within a size, the order of the list.
    def size(p: Path) -> int:
        m = re.search(r"\[\[(\d+),", p.parent.name)
        return int(m.group(1)) if m else 0
    jobs = [(p, c) for c in args.configs.split(",") for p in sorted(fpdls, key=size)]
    if args.resume:
        jobs = [(p, c) for p, c in jobs if (p.parent.name, c) not in done_keys]

    def work(job):
        fpdl, config = job
        row, _ = run(args.binary, fpdl, config, args.timeout, args.logs)
        if row["status"] == "circuits not valid":
            circuit = {k: v for k, v in row.items() if k.startswith("circuit_")}
            row2, _ = run(args.binary, fpdl, config, args.timeout, args.logs, ["--skip-circuit-check"])
            row = {**row2, **circuit, "status": "ok", "circuits_skipped": True}
        row.update({"protocol": fpdl.parent.name, "config": config})
        with lock:
            results["runs"] = [r for r in results["runs"]
                               if (r["protocol"], r["config"]) != (row["protocol"], row["config"])]
            results["runs"].append(row)
            results["updated"] = time.strftime("%Y-%m-%d %H:%M %Z")
            args.out.write_text(json.dumps(results, indent=1, sort_keys=True))
        print(f"{row['protocol']:40s} {config:12s} {row['status']:22s} "
              f"wall {row['wall_s']:.1f} s  ftec={row.get('ftec')}", flush=True)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        list(pool.map(work, jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
