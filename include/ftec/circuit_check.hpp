#pragma once

// Circuit validity (Def. 3.3 of the verification paper; Thm. 3.4): every
// syndrome-extraction circuit the protocol runs must be *correct* and
// *deterministic*. Checked once per circuit, before any verification, by
// propagating signed Pauli operators through the ideal circuit (the
// Heisenberg picture of a stabilizer simulation). No decision diagrams.
//
// Ancillas start in |0>, and so does any qubit right after a reset. The data
// start in the code space with the logical state unknown.
//
// Correctness (Def. 3.1)
//   * syndrome: measurement m[i] of the qm register observes the generator g:
//     lists at index i, so an input data error E flips it exactly when E
//     anticommutes with g_i; every syndrome bit is measured;
//   * data errors: an input data error E leaves the circuit as E' ~ E, i.e.
//     E' E is a stabilizer (checked on the 2n basis errors X_j, Z_j).
//
// Determinism (Def. 3.2)
//   * every measurement, of any ancilla, observes a stabilizer s (up to sign):
//     its error-free outcome is fixed whatever the encoded state, and it does
//     not depend on an earlier measurement of a reused qubit;
//   * every logical operator L comes out as L times a stabilizer (up to sign),
//     and never touches a qubit while that qubit is measured or reset: the
//     circuit leaves the encoded state alone.
//
// Signs (Thm. 3.4 keeps them). An error-free run must read 0 on every
// ancilla and leave every logical operator with its sign; a measurement that
// deterministically reads 1 (it observes -s), or a logical that comes out as
// -L, makes the circuit invalid. One exception, an FPDL extension: a syndrome
// row may declare its sign in g: as -[...], meaning the bit is read inverted
// (a classical NOT on the readout), so it reads 1 without errors by design;
// the declaration must then match the circuit. The verifier records outcomes
// as flips; branch guards see a -[...] bit as read (flip XOR 1), validity and
// correctability use the flips.
//
// A flag measurement that observes a stabilizer other than the identity is
// valid here; it does violate part (2) of the flag condition, which protocol
// validity checks for the circuits that need it.

#include "ftec/dag.hpp"
#include "ftec/qasm.hpp"

#include <string>
#include <vector>

namespace ftec {

struct CircuitIssue {
    enum class Kind { Correctness, Determinism, Sign };
    Kind        kind = Kind::Correctness;
    std::size_t line = 0;        // 0: about the circuit as a whole
    std::string message;

    [[nodiscard]] std::string to_string() const;
};

// One circuit. `stabilizers` generate the code's stabilizer group and
// `logicals` are representatives of its logical Pauli generators (2k of them).
[[nodiscard]] std::vector<CircuitIssue> check_circuit(const QasmProgram&              program,
                                                      const CircuitRef&               circuit,
                                                      const std::vector<std::string>& stabilizers,
                                                      const std::vector<std::string>& logicals);

// Does the flag register see the data at all? True when every flag
// measurement of an error-free run observes exactly the identity on the data
// -- part (2) of the flag condition (Def. 3.7). Circuits without flags: true.
[[nodiscard]] bool flags_ignore_data(const QasmProgram& program, const CircuitRef& circuit);

// check_circuit for every distinct circuit the protocol runs, each once;
// each line is prefixed with the SE name and the circuit file.
struct CircuitReport {
    std::vector<std::string> errors;     // the protocol is not valid
    std::vector<std::string> warnings;   // signs only
};
[[nodiscard]] CircuitReport check_circuits(const Dag& dag);

// The logical operators of a code (X_1..X_k, Z_1..Z_k), via SPBDD. Creates
// and destroys an SPBDD context, so call it while no other context is alive.
[[nodiscard]] std::vector<std::string> logical_operators(const fpdl::CodeSpec& code);

} // namespace ftec
