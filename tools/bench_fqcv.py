#!/usr/bin/env python3
"""Time the final E1E2 query (fqcv) of the spbdd backend, method by method.

Every protocol is run once per fqcv method (bruteforce, table, image, square),
each in its own process so that peak memory is that method's alone. For each
run the verifier's own statistics are parsed, and the time is split the way the
question is usually asked:

    pre-fqcv time = traversal - fqcv time - diagnostics

i.e. everything before the final query: gate propagation, fault injection,
measurement and splitting. "fqcv time" is only the call into the library
(spbdd::code::find_qec_cond_violation); the extra numbers "square" reports
(building N(S)-S, squaring the error set) are measured outside it, as
"diagnostics", so asking for them does not inflate either figure.

    tools/bench_fqcv.py --build-dir build --xlsx fqcv.xlsx
    tools/bench_fqcv.py --protocol 5,1,3 --methods image,square --timeout 60

Runs that time out or die (out of memory, say) still get a row, with the reason
in the "status" column, so a method that cannot finish shows up as exactly that.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import resource
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
METHODS = ["bruteforce", "table", "image", "square"]
UNIT_BYTES = {"B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3}

NUM = r"([\d.]+)"
SIZE = r"([\d.]+) (B|KiB|MiB|GiB)"

# column -> (regex, converter). Every pattern is optional: a method that does
# not print a line simply leaves the column empty.
FIELDS: dict[str, tuple[re.Pattern, callable]] = {}


def field(name: str, pattern: str, convert):
    FIELDS[name] = (re.compile(pattern, re.M), convert)


def to_bytes(m: re.Match) -> int:
    return int(float(m.group(1)) * UNIT_BYTES[m.group(2)])


field("code", r"^code\s*:\s*(\[\[.*?\]\])", lambda m: m.group(1))
field("tau", r"^code\s*:.*tau = (\d+)", lambda m: int(m.group(1)))
field("paths", r"^paths reached\s*:\s*(\d+) of", lambda m: int(m.group(1)))
field("records", r"^records reached\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("circuits", r"^circuits run\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("expansion_s", rf"^expansion\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("traversal_s", rf"^traversal\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("total_s", rf"^total runtime\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("peak_memory_bytes", rf"^peak memory\s*:\s*{SIZE}", to_bytes)
field("fqcv_calls", r"^fqcv calls\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("fqcv_s", rf"^fqcv time\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("fqcv_max_call_s", rf"^fqcv max call\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("fqcv_sets", r"^fqcv sets\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("fqcv_nodes_max", r"^fqcv set nodes\s*:\s*max (\d+)", lambda m: int(m.group(1)))
field("fqcv_nodes_sum", r"^fqcv set nodes\s*:.*sum (\d+)", lambda m: int(m.group(1)))
field("fqcv_setsize_max", r"^fqcv set size\s*:\s*max ([\d.e+]+)", lambda m: float(m.group(1)))
field("fqcv_rss_growth_bytes", rf"^fqcv rss growth\s*:\s*{SIZE}", to_bytes)
field("nms_build_s", rf"^nms build time\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("nms_nodes", r"^nms nodes\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("nms_size", r"^nms size\s*:\s*([\d.e+]+)", lambda m: float(m.group(1)))
field("sq_sets", r"^sq sets\s*:\s*(\d+)", lambda m: int(m.group(1)))
field("sq_nodes_max", r"^sq nodes\s*:\s*max (\d+)", lambda m: int(m.group(1)))
field("sq_nodes_sum", r"^sq nodes\s*:.*sum (\d+)", lambda m: int(m.group(1)))
field("sq_build_total_s", rf"^sq build time\s*:\s*total {NUM} s", lambda m: float(m.group(1)))
field("sq_build_max_s", rf"^sq build time\s*:.*max {NUM} s", lambda m: float(m.group(1)))
field("diagnostics_s", rf"^diagnostics\s*:\s*{NUM} s", lambda m: float(m.group(1)))
field("diagram_live", r"^diagram\s*:\s*(\d+) live", lambda m: int(m.group(1)))
field("diagram_peak", r"^diagram\s*:.*peak (\d+)", lambda m: int(m.group(1)))
field("diagram_table", r"^diagram\s*:.*table (\d+)", lambda m: int(m.group(1)))
field("qubits", r"^diagram\s*:.*, (\d+) qubits", lambda m: int(m.group(1)))
field("failures", r"^(\d+) unprotected path\(s\)", lambda m: int(m.group(1)))
field("min_t", r"first failure at t = (\d+)", lambda m: int(m.group(1)))


def parse(text: str) -> dict:
    row: dict = {}
    for name, (pattern, convert) in FIELDS.items():
        if match := pattern.search(text):
            row[name] = convert(match)
    if "failures" not in row and "no unprotected path found" in text:
        row["failures"] = 0
    row["verdict"] = ("clean" if row.get("failures") == 0 else
                      f"{row['failures']} bad @t={row.get('min_t')}"
                      if "failures" in row else None)
    if "traversal_s" in row and "fqcv_s" in row:
        row["pre_fqcv_s"] = max(
            0.0, row["traversal_s"] - row["fqcv_s"] - row.get("diagnostics_s", 0.0))
    return row


def limit_memory(gib: float):
    def apply():
        if gib > 0:
            cap = int(gib * 1024**3)
            resource.setrlimit(resource.RLIMIT_AS, (cap, cap))
    return apply


def run_one(binary: Path, protocol: Path, method: str, timeout: float, mem_gib: float) -> dict:
    row = {"protocol": protocol.stem, "method": method, "status": "ok"}
    command = [str(binary), str(protocol), f"--fqcv={method}"]
    started = time.perf_counter()
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                              preexec_fn=limit_memory(mem_gib))
    except subprocess.TimeoutExpired as exc:
        row["status"] = f"timeout (>{timeout:g}s)"
        row["wall_s"] = time.perf_counter() - started
        # Whatever the verifier had printed before the kill is still useful.
        partial = exc.stdout.decode() if isinstance(exc.stdout, bytes) else (exc.stdout or "")
        row.update({k: v for k, v in parse(partial).items() if k in ("code", "tau")})
        return row
    row["wall_s"] = time.perf_counter() - started
    if done.returncode != 0:
        last = (done.stderr.strip().splitlines() or [f"exit {done.returncode}"])[-1]
        row["status"] = "out of memory" if ("bad_alloc" in last or "memory" in last.lower()
                                            or done.returncode in (-9, -6)) else f"failed: {last[:80]}"
        row.update({k: v for k, v in parse(done.stdout).items() if k in ("code", "tau")})
        return row
    row.update(parse(done.stdout))
    return row


# Column order and header text for the CSV / Excel output.
COLUMNS = [
    ("protocol", "protocol"), ("code", "code"), ("tau", "tau"), ("method", "fqcv method"),
    ("status", "status"), ("verdict", "verdict"),
    ("pre_fqcv_s", "pre-fqcv time (s)"), ("fqcv_s", "final E1E2 time (s)"),
    ("fqcv_max_call_s", "max single call (s)"), ("fqcv_calls", "fqcv calls"),
    ("expansion_s", "expansion (s)"), ("traversal_s", "traversal (s)"),
    ("total_s", "total (s)"), ("wall_s", "wall (s)"),
    ("peak_memory_bytes", "peak memory (MiB)"), ("fqcv_rss_growth_bytes", "fqcv RSS growth (MiB)"),
    ("fqcv_sets", "E sets queried"), ("fqcv_nodes_max", "E BDD nodes (max)"),
    ("fqcv_nodes_sum", "E BDD nodes (sum)"), ("fqcv_setsize_max", "|E| (max)"),
    ("nms_build_s", "N(S)-S build (s)"), ("nms_nodes", "N(S)-S BDD nodes"),
    ("nms_size", "|N(S)-S|"), ("sq_nodes_max", "E^2 BDD nodes (max)"),
    ("sq_nodes_sum", "E^2 BDD nodes (sum)"), ("sq_build_total_s", "E^2 build total (s)"),
    ("sq_build_max_s", "E^2 build max (s)"), ("diagnostics_s", "diagnostics (s)"),
    ("diagram_live", "diagram live nodes"), ("diagram_peak", "diagram peak nodes"),
    ("diagram_table", "node table size"), ("qubits", "qubits"),
    ("paths", "paths"), ("records", "records"), ("circuits", "circuits"),
]
MIB_COLUMNS = {"peak_memory_bytes", "fqcv_rss_growth_bytes"}


def cell(row: dict, key: str):
    value = row.get(key)
    if value is None:
        return None
    return value / 1024**2 if key in MIB_COLUMNS else value


def write_csv(rows: list[dict], path: Path) -> None:
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(header for _, header in COLUMNS)
        for row in rows:
            writer.writerow(["" if (v := cell(row, key)) is None else v for key, _ in COLUMNS])


def write_xlsx(rows: list[dict], path: Path, notes: list[str]) -> None:
    from openpyxl import Workbook
    from openpyxl.styles import Alignment, Font, PatternFill
    from openpyxl.utils import get_column_letter

    wb = Workbook()
    ws = wb.active
    ws.title = "all runs"
    ws.append([header for _, header in COLUMNS])
    for row in rows:
        ws.append([cell(row, key) for key, _ in COLUMNS])

    head_fill = PatternFill("solid", fgColor="DDE6F2")
    for c in ws[1]:
        c.font = Font(bold=True)
        c.fill = head_fill
        c.alignment = Alignment(wrap_text=True, vertical="center")
    ws.freeze_panes = "E2"
    for index, (key, header) in enumerate(COLUMNS, start=1):
        letter = get_column_letter(index)
        ws.column_dimensions[letter].width = max(12, min(34, len(header) + 2))
        for c in ws[letter][1:]:
            if key.endswith("_s") and key != "tau":
                c.number_format = "0.0000"
            elif key in MIB_COLUMNS:
                c.number_format = "0.0"
            elif isinstance(c.value, int):
                c.number_format = "#,##0"
            elif isinstance(c.value, float) and key.startswith(("nms_size", "fqcv_setsize")):
                c.number_format = "0.000E+00"
    ws.column_dimensions["A"].width = 38
    ws.auto_filter.ref = ws.dimensions

    # A compact view: one line per protocol, the two headline numbers per method.
    by_protocol: dict[str, dict[str, dict]] = {}
    for row in rows:
        by_protocol.setdefault(row["protocol"], {})[row["method"]] = row
    methods = [m for m in METHODS if any(m in per for per in by_protocol.values())]
    summary = wb.create_sheet("summary")
    summary.append(["protocol", "code", "verdict", "pre-fqcv time (s)"] +
                   [f"{m}: final E1E2 (s)" for m in methods] +
                   [f"{m}: peak mem (MiB)" for m in methods])
    for name, per in by_protocol.items():
        sample = next((r for r in per.values() if "code" in r), next(iter(per.values())))
        ok = [r for r in per.values() if r["status"] == "ok"]
        summary.append(
            [name, sample.get("code"), ok[0].get("verdict") if ok else None,
             min((r["pre_fqcv_s"] for r in ok if "pre_fqcv_s" in r), default=None)] +
            [(per[m].get("fqcv_s") if m in per and per[m]["status"] == "ok"
              else (per[m]["status"] if m in per else None)) for m in methods] +
            [(cell(per[m], "peak_memory_bytes") if m in per and per[m]["status"] == "ok" else None)
             for m in methods])
    for c in summary[1]:
        c.font = Font(bold=True)
        c.fill = head_fill
        c.alignment = Alignment(wrap_text=True, vertical="center")
    summary.freeze_panes = "B2"
    summary.column_dimensions["A"].width = 38
    for index in range(2, summary.max_column + 1):
        summary.column_dimensions[get_column_letter(index)].width = 18
    for r in summary.iter_rows(min_row=2):
        for c in r[3:3 + 1 + len(methods)]:
            if isinstance(c.value, float):
                c.number_format = "0.0000"
        for c in r[4 + len(methods):]:
            if isinstance(c.value, float):
                c.number_format = "0.0"

    legend = wb.create_sheet("notes")
    for line in notes:
        legend.append([line])
    legend.column_dimensions["A"].width = 120
    wb.save(path)


NOTES = [
    "pre-fqcv time = traversal - final E1E2 time - diagnostics: gate propagation, fault injection, "
    "measurement and splitting, i.e. everything before the final fqcv.",
    "final E1E2 time = total time spent inside spbdd::code::find_qec_cond_violation over all checks "
    "(per distinct state; repeated states are served from the verifier's cache).",
    "expansion = building the symbolic path DAG (BMC bound search); not part of either figure above.",
    "peak memory = peak RSS of the whole process. fqcv RSS growth = largest growth of the process "
    "RSS during one fqcv call (VmHWM reset before each call).",
    "E BDD nodes / |E|: the error set handed to fqcv (nodes: shared BDD node count; |E|: number of "
    "Paulis in it, max over calls).",
    "square only: N(S)-S = normalizer minus stabilizer, built once; E^2 = E*E, built per fqcv call. "
    "Both are measured outside the timed region, so they are not included in the final E1E2 time.",
    "A row whose status is not 'ok' timed out or ran out of memory; its numbers are left empty.",
    "Each run is a separate process, with BDD reordering disabled.",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build")
    parser.add_argument("--binary", type=Path, default=None)
    parser.add_argument("--protocols", type=Path, default=REPO_ROOT / "protocols")
    parser.add_argument("--protocol", action="append", default=[],
                        help="only protocols whose path contains this; repeatable")
    parser.add_argument("--methods", default=",".join(METHODS))
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds per run")
    parser.add_argument("--mem-limit-gib", type=float, default=0.0,
                        help="address-space limit per run (0 = none)")
    parser.add_argument("--json", type=Path, default=None)
    parser.add_argument("--csv", type=Path, default=None)
    parser.add_argument("--xlsx", type=Path, default=None)
    parser.add_argument("--collect-only", action="store_true",
                        help="run nothing: only turn the --resume JSON into CSV / Excel")
    parser.add_argument("--resume", type=Path, default=None,
                        help="JSON of an earlier sweep; finished (protocol, method) pairs are kept")
    args = parser.parse_args()

    binary = args.binary or (args.build_dir / "ftec-verify")
    if not binary.exists():
        print(f"error: {binary} not found", file=sys.stderr)
        return 2
    methods = [m.strip() for m in args.methods.split(",") if m.strip()]
    for m in methods:
        if m not in METHODS:
            print(f"error: unknown method {m}", file=sys.stderr)
            return 2
    protocols = sorted(args.protocols.glob("*/*.fpdl"))
    if args.protocol:
        protocols = [p for p in protocols if any(n in str(p) for n in args.protocol)]
    if not protocols:
        print("error: no protocols matched", file=sys.stderr)
        return 2

    rows: list[dict] = []
    done: set[tuple[str, str]] = set()
    if args.resume and args.resume.exists():
        rows = json.loads(args.resume.read_text())
        done = {(r["protocol"], r["method"]) for r in rows}

    for protocol in protocols:
        for method in methods:
            if (protocol.stem, method) in done or args.collect_only:
                continue
            print(f"  {protocol.stem} / {method} ...", file=sys.stderr, flush=True)
            row = run_one(binary, protocol, method, args.timeout, args.mem_limit_gib)
            print(f"      {row['status']}"
                  + (f", fqcv {row['fqcv_s']:.3f}s" if "fqcv_s" in row else ""),
                  file=sys.stderr, flush=True)
            rows.append(row)
            if args.json:
                args.json.write_text(json.dumps(rows, indent=1))

    if args.collect_only:
        have = {(r["protocol"], r["method"]) for r in rows}
        for protocol in protocols:
            for method in methods:
                if (protocol.stem, method) not in have:
                    rows.append({"protocol": protocol.stem, "method": method,
                                 "status": "not run"})
    order = {p.stem: i for i, p in enumerate(protocols)}
    rows.sort(key=lambda r: (order.get(r["protocol"], 1 << 30), METHODS.index(r["method"])))

    # The four methods decide the same question; a protocol on which they
    # disagree is a bug, and worth more than any timing.
    bad = []
    for name in {r["protocol"] for r in rows}:
        verdicts = {r["verdict"] for r in rows if r["protocol"] == name and r["status"] == "ok"}
        if len(verdicts) > 1:
            bad.append(f"{name}: {sorted(map(str, verdicts))}")
    if bad:
        print("VERDICT DISAGREEMENT between fqcv methods:\n  " + "\n  ".join(bad), file=sys.stderr)

    if args.json:
        args.json.write_text(json.dumps(rows, indent=1))
    if args.csv:
        write_csv(rows, args.csv)
    if args.xlsx:
        write_xlsx(rows, args.xlsx, NOTES)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
