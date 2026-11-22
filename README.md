# Flag protocol verifier

Checks whether a flag fault-tolerant error-correction protocol is actually
fault tolerant.

Given a protocol written in FPDL and the OpenQASM circuits it uses, the
verifier enumerates every execution path, injects every fault at every fault
location, and checks the three things that together make a protocol fault
tolerant, following *Formal Verification of Flagged Fault-Tolerant Error
Correction Protocols* (FTEC ⇔ protocol valid ∧ correctable):

1. **Circuit validity.** Every syndrome-extraction circuit is correct (it
   measures the generators it declares, and lets data errors through
   unchanged) and deterministic (no outcome is random, and the encoded state
   is left alone).
2. **Protocol validity.** At every terminal, what can have happened along the
   measurement record is something the decoder can act on (no control-flow
   bugs), and the flag circuits that a repeated final round relies on satisfy
   the flag condition (no flag bugs).
3. **Correctability.** No record leaves two errors with the same syndrome
   whose product lies in `N(S)\S`; correcting one of them would damage the
   encoded state under the other. Executions may start from a data error of
   weight r as well as suffer s faults, with r + s ≤ t.

See [What is checked](#what-is-checked) for the details.

All Pauli-set representation and arithmetic is done by
[SPBDD](https://github.com/fmlab-iis/spbdd), a library that represents sets of
multi-qubit Pauli operators as BuDDy BDDs.

**Website and browser playground:** <https://jtsai1120.github.io/flag-protocol-verifier/>

```
.fpdl ──► fpdl::Parser ──┬─► code: [[n,k,d]] + generators ──► tau = ⌊(d−1)/2⌋
                          ├─► SE declarations (qd/qm/qf, file)
                          └─► SymbolicPaths ──► ftec::build_dag ──► trie
                                                                     │
                                                       DFS + guard routing
                                                                     ▼
                                                           spbdd backend
                                              (state propagation, fault injection,
                                               measurement splitting, final E1E2 query)
```

## Building

You need `g++` (C++20), `make` and `git`.

```bash
make          # fetch SPBDD → build libspbdd.a → build build/ftec-verify
make check    # also build and run the tests
```

What `make` does:

1. Fetches `fmlab-iis/spbdd` into `third_party/spbdd-src`, pinned to
   `SPBDD_REF` in the `Makefile`.
2. Builds `libspbdd.a` with SPBDD's own Makefile. That fetches BuDDy and bundles
   it into the same archive.
3. Keeps only the archive and the public headers:
   ```
   third_party/spbdd/lib/libspbdd.a
   third_party/spbdd/include/spbdd/{spbdd,code,context,pset}.hpp, bdd.h
   ```
4. Links the verifier against that archive alone.

| Command | Purpose |
|---|---|
| `make SPBDD_REF=<branch/tag/commit>` | Use another SPBDD version (run `make distclean` first) |
| `make SPBDD_SRC=/path/to/spbdd` | Use an SPBDD checkout you already have, without fetching |
| `make site` | Build the website's WebAssembly playground (needs Emscripten) |
| `make clean` | Remove `build/` |
| `make distclean` | Also remove `third_party/` |

## Usage

```bash
./build/ftec-verify <protocol.fpdl> [options]

./build/ftec-verify "protocols/CR17_[[5,1,3]]/CR17_[[5,1,3]].fpdl"
./build/ftec-verify "protocols/CB18_[[17,1,5]]_plain/CB18_[[17,1,5]]_plain.fpdl" --fqcv=table
```

| Option | Default | Description |
|---|---|---|
| `--fqcv=METHOD` | `image` | How the final E1E2 query is answered: `bruteforce`, `table`, `image` or `square` (see below) |
| `--bound=N` | searched | Fix the BMC bound; without it the bound doubles from 16 until the expansion is complete (up to 65536) |
| `--max-paths=N` | `5000` | Give up past this many symbolic paths |
| `--first` | off | Stop at the first unprotected path |
| `--dag` | off | Print the merged path structure and exit without verifying |
| `--no-input-errors` | off | Start every execution from an error-free input (r = 0) |
| `--no-validity` | off | Skip the protocol validity check |
| `--skip-circuit-check` | off | Verify the circuits as written even when they are not valid |

The output lists:

- the number of paths, records and circuits reached;
- correctability: `no unprotected path found`, or each unprotected path with
  its fault count and measurement record;
- protocol validity: how many terminal records each control-flow condition
  settled, then the control-flow bugs (paths) and the flag bugs (circuits),
  each with a counterexample;
- the overall verdict, `FTEC : accepted` or `rejected`;
- backend statistics: cache hit rates, and the time and BDD sizes of the final
  query;
- expansion time, traversal time and peak memory.

Dynamic BDD variable reordering is always off. It never paid off on any
protocol large enough to time.

### The four methods of the final E1E2 query

All four are SPBDD's `code::find_qec_cond_violation`. They answer the same
question and always give the same verdict; only the cost differs.

| Method | How |
|---|---|
| `bruteforce` | Enumerate the error set and multiply every pair |
| `table` | Enumerate the error set once, keyed by syndrome |
| `image` | Dynamic programming over the layers of the set's BDD, computing the image of the label set |
| `square` | Build E·E and ask whether it meets N(S)−S (built once per code) |

### Benchmarking

`tools/bench_fqcv.py` runs every protocol once per method, each in its own
process, so each method's peak memory is its own. Results go to JSON, CSV or
Excel. The statistics keep the final query apart from everything before it:
`before E1E2 = traversal − fqcv − diagnostics`.

```bash
tools/bench_fqcv.py --xlsx fqcv.xlsx
tools/bench_fqcv.py --protocol 25,1,5 --methods table,image,square --timeout 2000 --json out.json
```

### Generating larger CB18 protocols

`tools/gen_cb18_surface.py` implements the general t-FTEC algorithm of
Chamberland & Beverland, *Flag fault-tolerant error correction with arbitrary
distance codes* (Quantum 2, 53, 2018), Section 3. It writes the FPDL file and
the `FSE_f` / `FSE_b` circuits for a rotated surface code.

- Weight-4 checks use the single-flag circuit of Fig. 2(b). Weight-2 checks
  are unflagged.
- The lattice can be square (d×d) or rectangular (d×C: distance d, d·C qubits).
- The generated d=5 protocol is identical to the hand-written
  `CB18_[[25,1,5]]_plain`.

```bash
tools/gen_cb18_surface.py 5 --out protocols            # [[25,1,5]]
tools/gen_cb18_surface.py 5 --cols 6 --out protocols   # [[30,1,5]], 5×6
tools/gen_cb18_surface.py 5 --cols 7 --out protocols   # [[35,1,5]], 5×7
tools/gen_cb18_surface.py 5 --cols 9 --out protocols   # [[45,1,5]], 5×9
tools/gen_cb18_surface.py 7 --out protocols            # [[49,1,7]]
```

Full check (input errors, protocol validity, correctability), `table`
method, 2 vCPU with two runs in parallel:

| Protocol | Records | Time | Peak memory | Verdict |
|---|---|---|---|---|
| CB18 [[25,1,5]] 5×5 | 1,938,025 | 332 s | 378 MiB | accepted |
| CB18 [[30,1,5]] 5×6 | 2,997,132 | 719 s | 496 MiB | accepted |
| CB18 [[35,1,5]] 5×7 | 4,286,296 | 1,368 s | 639 MiB | accepted |
| CB18 [[45,1,5]] 5×9 | 7,554,651 | 3,782 s | 1,126 MiB | accepted |
| CB18 [[49,1,7]] 7×7 | — | killed after 6,791 s | over 5.7 GiB | out of memory |

The bottleneck is state propagation and fault injection before the final
query. The query itself is about 0.5% of the total.

Condition 3 compares each residual set with the errors within weight s of
the code space, (weight ≤ s) · S. That diagram grows quickly with the width
of the lattice: for s = 2 it has 43,743 nodes at 5×5, 116,097 at 5×6,
387,079 at 5×7 and 2,790,471 at 5×9. Two things keep it usable:

- The check is `set − (set ∩ bound)` rather than `set − bound`. A plain
  difference rebuilds the complement of the bound's subgraphs and, on
  [[35,1,5]], did not finish in 18 hours.
- BuDDy's operation cache has 2^20 entries instead of spbdd's default 2^16.
  With 2^16, building the bound for [[45,1,5]] did not finish in 25 minutes;
  with 2^20 it takes about 100 s.

## What is checked

### 1. Circuit validity

Before anything else, `ftec-verify` checks every extraction circuit once
(`src/ftec/circuit_check.cpp`, no BDDs). It propagates signed Pauli operators
backwards through the ideal circuit to its start (the Heisenberg picture of a
stabilizer simulation). Ancillas start in |0>, and so does any qubit after a
reset; the data start in the code space.

- **Correct.** Syndrome bit i observes the generator that `g:` lists at index
  i, so an input error flips it exactly when it anticommutes with that
  generator. An input data error E leaves the circuit as E' with E'E in the
  stabilizer group (checked on the 2n errors X_j and Z_j).
- **Deterministic.** Every measurement, of any ancilla, observes a stabilizer:
  its error-free outcome is fixed whatever the encoded state, and does not
  depend on an earlier measurement of a reused qubit. Every logical operator
  comes out as itself times a stabilizer and never touches a qubit while that
  qubit is measured or reset.

Outcomes are recorded as flips relative to the error-free outcome, so a
measurement that deterministically reads 1, or a logical operator that comes
out with a minus sign, is reported as a warning only. A syndrome bit that
reads 1 by design is declared with a minus sign in `g:`, as `-[...]`: the bit
measures minus the code's stabilizer with those letters, and the warning then
goes away (a `-[...]` row whose bit reads 0 is warned about instead). The
protocol's branch guards then see that bit as the circuit reads it, 1 when
nothing happened; validity and correctability still work with the flips.
Bha23 [[7,1,3]] fig6 does this for its three weight-6 flag circuits: with
`Y = iXZ`, IIIXXXX · IZZIIZZ = −IZZXXYY, so a circuit that measures +IZZXXYY
reads 1, and the protocol starts with `s = 1` and stops on `s != 1`. Circuits that fail are
reported with the line and what they measure instead, and the protocol is
rejected without being verified; `--skip-circuit-check` verifies them anyway.

The two LL25 [[5,1,3]] [2,2] protocols fail: their parallel schedules make
some syndrome bits random, and one of them measures the fifth cyclic generator
instead of the declared one. An independent simulation with
[stim](https://github.com/quantumlib/Stim) agrees with the checker on 240
mutated circuits (757 measurements, no disagreement).

### 2. Correctability with input errors

The backend keeps one error set per level (r, s): executions that started from
a data error of weight r and suffered s faults, with r + s ≤ t. At each
terminal the union of all levels is E_m, and no two errors in it may share a
syndrome while differing by a logical operator. `--no-input-errors` keeps only
r = 0.

### 3. Protocol validity

Protocol validity (Def. 3.10) is checked as in the paper's algorithm, and its
failures are reported in two kinds, as in the paper's Table 1.

**Control-flow bugs** (Def. 3.9), counted in paths. Every terminal record
must satisfy the condition its terminal calls for:

| Terminal | Condition | Checked |
|---|---|---|
| `decode(...)` | 1 | Every error in E_m has the same ideal syndrome (no generator splits E_m) |
| `decode(...)` | 2 | The path ends in t − w + 1 identical, unflagged, full rounds, where w counts the faults its records expose (flags, and changes of syndrome between consecutive unflagged full rounds, the first one compared with the trivial syndrome) |
| `end()` | 3 | Every error from s faults is within weight s of the code space |

Condition 3 is the condition of a terminal that applies the trivial
correction, so a decoding terminal is not settled by it, and an ending
terminal is settled by nothing else. Condition 2 is a property of the path:
every record that reaches the path must end in the same repeated suffix (the
suffix form). A record that merely happens to repeat its last round does not
count.

**Flag bugs** (Def. 3.10 (3)), counted in circuits. On every path with the form
of Condition 2, the circuits of the repeated suffix must satisfy the
(t − w)-flag condition, whether or not Condition 1 also holds there. The
k-flag condition of a circuit is checked by running the circuit alone with up
to k faults: whenever no flag is raised, s faults must leave an error within
weight s of the code space, and a fault-free run must never raise a flag.

Each bug is reported with a counterexample: two residual errors the record
cannot tell apart, an error heavier than its faults allow, or the faults that
break the flag condition.

### Results on the bundled protocols

Input errors included; `--fqcv=table`. Control-flow bugs count paths, flag
bugs count circuits, as in the paper's Table 1.

| Protocol | Circuits | Control-flow bugs | Flag bugs | Correctable | FTEC |
|---|---|---|---|---|---|
| CR17 [[5,1,3]] | valid | 0 | 0 | yes | accepted |
| LL25 [[5,1,3]] [1,1,1,1]^T | valid | 0 | 0 | yes | accepted |
| LL25 [[5,1,3]] [2,2] | 6 problems | 0 | 0 | no (t = 1) | rejected |
| LL25 [[5,1,3]] [2,2]^T | 8 problems | 0 | 0 | no (t = 1) | rejected |
| LL25 [[5,1,3]] [2,2]^T fix_2 | valid | 0 | 0 | no (t = 1) | rejected |
| Bha23 [[5,1,3]] | valid | 8 | 0 | yes | rejected |
| Bha23 [[7,1,3]] fig5 | valid | 6 | 0 | yes | rejected |
| Bha23 [[7,1,3]] fig6 | valid | 0 | 0 | yes | accepted |
| CB18 [[17,1,5]], [[25,1,5]], [[30,1,5]], [[35,1,5]], [[45,1,5]] | valid | 0 | 0 | yes | accepted |
| CB18 [[49,1,7]] | valid | — | — | — | out of memory |
| Du24 [[17,1,5]] (parallel flags) | valid | 0 | 2 (`F6X_f`, `F6Z_f`) | no (t = 2) | rejected |
| Du24 [[17,1,5]] plain | valid | 0 | 1 (`FSE_f`) | no (t = 2) | rejected |
| Du24 [[19,1,5]] | valid | 0 | 1 (`FSE_f`) | yes | rejected |
| LL25 [[17,1,5]], [[19,1,5]] [1,1,1,...]^T and [[17,1,5]] [2,2,2,1,1]^T | valid | 1 (`end()`, input errors only) | 0 | yes | rejected |
| LL25 [[19,1,5]] [2,2,2,1,1,1]^T | valid | 1 (`end()`) | 0 | yes | rejected |

- Bha23 [[5,1,3]] and fig5: after a flag, the adaptive measurements leave
  errors with different syndromes under one record (Condition 1), and a
  decoding terminal cannot fall back on Condition 3. In [[5,1,3]], a false
  alarm (`IIIII`) and the hook error `IIZXI` both read 0 on the circuits that
  follow but differ on `IXZZX`.
- Du24: after the trivial rounds, the flag circuit is not 2-flag: two faults
  that raise no flag can leave `IIIIIIIXIIIIIIZXZ`, of weight 3.
- LL25 distance 5: the protocol ends after one trivial round. An input error
  of weight 1 and one fault can then leave a weight-2 error, which Condition 3
  forbids (r + s ≤ t, weight ≤ s); in [[19,1,5]] [2,2,2,1,1,1]^T two faults
  alone leave a weight-3 error.

The full table, with times and memory for all four methods, is on the
website's results page.

## Tests

`make check` runs four test programs:

- `parser_tests` parses every FPDL file under `protocols/`.
- `qasm_tests` parses every QASM circuit.
- `circuit_check_tests` runs the circuit check on every protocol (the two
  LL25 [2,2] protocols must fail it, the rest must pass) and on fourteen
  deliberately broken circuits.
- `fqcv_tests` verifies every distance-3 protocol once with each of the four
  methods. The four results must agree with each other and with the verdicts
  recorded in `tests/expected.txt`, correctability and protocol validity both.
  The correctability verdicts were cross-checked against an independent,
  hand-written BuDDy backend for error-free inputs.

## Website

`site/` is the static website: a landing page and a playground that runs the
verifier in the browser as WebAssembly, on the bundled protocols or on your
own `.fpdl` and `.qasm` files.

`make site` compiles the verifier with Emscripten and bundles the protocols into
`site/playground/protocols.json`.

The results page (`site/results/`) lists every bundled protocol with the
columns the reader picks: verdicts, the time of each check (circuit validity,
protocol validity, correctability), peak memory and sizes, for each E1E2
method and for a run without the validity check. Its data come from
`tools/site_bench.py`, which runs each protocol natively in its own process and
merges the parsed statistics into `web/results.json`; `make site` publishes
that file.

```bash
tools/site_bench.py --configs table --jobs 2 --skip 35,1,5 --skip 45,1,5 --skip 49,1,7
tools/site_bench.py --configs image,square,bruteforce --resume --skip 35,1,5 --skip 45,1,5 --skip 49,1,7
``` The build outputs are not committed to
`main`; the published copy lives on the `gh-pages` branch. To preview locally,
serve the directory: `python3 -m http.server -d site`.

A second playground, for SPBDD itself, is parked in `web/spbdd-playground/` and
not published for now. It interprets a subset of C++17 (`cpp.js`) on top of
SPBDD's WebAssembly build; `make check-spbdd-playground` builds it and checks
that each of its examples prints the same as when compiled with g++.

## Layout

```
Makefile
include/fpdl/parser.hpp      FPDL front end
include/ftec/                verifier core: qasm, dag (path trie), verify (walk),
                             backend (interface), spbdd_backend
src/                         implementations; ftec_verify_main.cpp is the CLI
tests/                       parser, qasm and fqcv tests, expected.txt
tools/bench_fqcv.py          batch measurements
tools/gen_cb18_surface.py    CB18 protocol generator
protocols/                   test protocols, one directory each (.fpdl + the .qasm it uses)
docs/FPDL.md                 the FPDL language
web/                         WebAssembly builds for the website; the parked SPBDD playground
site/                        the website
```

## Known limitations

- `Outcome` packs the syndrome into a `uint64`. Codes whose syndrome is longer
  than 64 bits (rotated surface codes above 65 qubits) need this changed first.
- `--max-paths` defaults to 5000.
- The browser build runs about 2.6× slower than the native one, and its memory
  is capped at 4 GiB.
