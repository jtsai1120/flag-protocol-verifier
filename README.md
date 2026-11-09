# Flag protocol verifier

Checks whether a flag fault-tolerant error-correction protocol is actually
fault tolerant.

Given a protocol written in FPDL and the OpenQASM circuits it uses, the
verifier enumerates every execution path, injects every fault at every fault
location, and asks whether some path leaves two errors that the measurement
record cannot tell apart but whose product lies in `N(S)\S`. If such a pair
exists, correcting one of the errors damages the encoded state under the other,
and the protocol is not fault tolerant.

All Pauli-set representation and arithmetic is done by
[SPBDD](https://github.com/fmlab-iis/spbdd), a library that represents sets of
multi-qubit Pauli operators as BuDDy BDDs.

**Website and browser playgrounds:** <https://jtsai1120.github.io/flag-protocol-verifier/>

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
| `make site` | Build the website's WebAssembly playgrounds (needs Emscripten) |
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

The output lists:

- the number of paths, records and circuits reached;
- the verdict: `no unprotected path found`, or each unprotected path with its
  fault count and measurement record;
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
  `CB_18[[25,1,5]]_plain`.

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

## Tests

`make check` runs three test programs:

- `parser_tests` parses every FPDL file under `protocols/`.
- `qasm_tests` parses every QASM circuit.
- `fqcv_tests` verifies every distance-3 protocol once with each of the four
  methods. The four results must agree with each other and with the verdicts
  recorded in `tests/expected.txt`. Those verdicts were cross-checked against
  an independent, hand-written BuDDy backend when the file was made.

## Website

`site/` is the static website: a landing page and two playgrounds that run
in the browser as WebAssembly.

- The verifier playground runs the bundled protocols or your own `.fpdl` and
  `.qasm` files.
- The SPBDD playground runs JavaScript programs against SPBDD.

`make site` compiles both with Emscripten and bundles the protocols into
`site/playground/protocols.json`. The build outputs are not committed to
`main`; the published copy lives on the `gh-pages` branch. To preview locally,
serve the directory: `python3 -m http.server -d site`.

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
web/                         WebAssembly builds for the website (Makefile, SPBDD bindings)
site/                        the website
```

## Known limitations

- `Outcome` packs the syndrome into a `uint64`. Codes whose syndrome is longer
  than 64 bits (rotated surface codes above 65 qubits) need this changed first.
- `--max-paths` defaults to 5000.
- The browser build runs about 2.6× slower than the native one, and its memory
  is capped at 4 GiB.
