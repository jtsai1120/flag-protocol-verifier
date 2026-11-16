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
2. **Protocol (control-flow) validity.** At every terminal, what can have
   happened along the measurement record is something the decoder can act on.
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
  settled, and each invalid path with a counterexample;
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
tools/gen_cb18_surface.py 5 --cols 9 --out protocols   # [[45,1,5]], 5×9
tools/gen_cb18_surface.py 7 --out protocols            # [[49,1,7]]
```

Reference measurements (2 vCPU, 7.8 GiB, two runs in parallel):

| Protocol | Before E1E2 (s) | table fqcv | image fqcv | square fqcv | Peak memory |
|---|---|---|---|---|---|
| CB18 [[25,1,5]] | 201–210 | 0.96 s | 1.85 s | 1.65 s | 157 MiB |
| CB18 [[35,1,5]] 5×7 | 741–762 | 3.22 s | 4.45 s | 4.90 s | 331 MiB |
| CB18 [[45,1,5]] 5×9 | 2215–2238 | 7.81 s | 10.47 s | 12.91 s | 601 MiB |
| CB18 [[49,1,7]] | did not finish in 2000 s | | | | |

The bottleneck is state propagation and fault injection before the final
query. The query itself is about 0.5% of the total.

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
out with a minus sign, is reported as a warning only. Circuits that fail are
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

At every terminal record, one of the control-flow conditions has to hold:

| Condition | Checked |
|---|---|
| 1 | Every error in E_m has the same ideal syndrome (no generator splits E_m) |
| 2 | The record ends in t − w + 1 identical, unflagged, full rounds, where w counts the faults the record exposes (flags, and changes of syndrome between consecutive unflagged rounds), and those circuits satisfy the (t − w)-flag condition |
| 3 | Every error from s faults is within weight s of the code space |

A terminal that ends without decoding (`end()`) needs Condition 3. The
k-flag condition of a circuit is checked by running the circuit alone with up
to k faults: whenever no flag is raised, s faults must leave an error within
weight s of the code space, and a fault-free run must never raise a flag.
Every invalid path is reported with a counterexample, for example two residual
errors that the record cannot tell apart.

### Results on the bundled protocols

Distance 3 (`tests/expected.txt`); input errors included:

| Protocol | Correctable | Control flow | FTEC |
|---|---|---|---|
| CR17 [[5,1,3]] | yes | valid | accepted |
| LL25 [[5,1,3]] [1,1,1,1]^T | yes | valid | accepted |
| Bha23 [[7,1,3]] fig5 | yes | valid | accepted |
| Bha23 [[7,1,3]] fig6 | yes | valid | accepted |
| Bha23 [[5,1,3]] | yes | 4 invalid paths | rejected |

In Bha23 [[5,1,3]], after a flag the adaptive measurements cannot tell a false
alarm (`IIIII`) from the hook error `IIZXI`: both read 0 on `XZZXI`, `YXXYI` and
`XIXZZ`, but they differ on `IXZZX`.

Distance 5, `--fqcv=table`, 2 vCPU, two runs in parallel. Validity cost is the
difference to the same run with `--no-validity`:

| Protocol | Correctable | Control flow | Validity time | Traversal (with / without validity) | Peak memory (with / without) |
|---|---|---|---|---|---|
| CB18 [[17,1,5]] | yes | valid (Cond 2 settles 711 records) | 8.4 s | 176 s / 168 s | 202 / 193 MiB |
| Du24 [[17,1,5]] plain | no, 36 records at t = 2 | 52 invalid records, 1 path | 11.4 s | 307 s / 293 s | 250 / 236 MiB |
| LL25 [[17,1,5]] [1,1,1,1,1,...]^T | yes | 4016 invalid records, 2 paths | 17.8 s | 267 s / 242 s | 178 / 158 MiB |

- Du24: after three trivial rounds the flag circuit `FSE_f` is not 2-flag: two
  faults that raise no flag can leave `IIIIIIIXIIIIIIZXZ`, of weight 3.
- LL25: the final rounds use the unflagged `FSE_b`, which is not 1-flag, so
  Condition 2 cannot hold (also with `--no-input-errors`); and the protocol
  ends after one trivial round, where an input error of weight 1 and one fault
  can leave a weight-2 error (input errors only).

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
`site/playground/protocols.json`. The build outputs are not committed to
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
