#pragma once

// Static check that each syndrome-extraction circuit measures what its SE
// declaration says it does.
//
// The traversal itself never looks at `g:`: it propagates error sets through
// the gates and reads whatever the ancilla measurements happen to report. A
// circuit wired to the wrong data qubit, with a Pauli of the wrong type, or
// measured in the wrong basis is therefore verified as written, and since the
// final check asks only whether two errors that share a record differ by a
// logical operator, such a circuit can still pass. This check closes that gap
// before any verification runs.
//
// For every measurement into the syndrome register, the measured observable
// is propagated backwards through the ideal circuit (Heisenberg picture,
// phases ignored) to the start of the circuit:
//
//   * at a reset or at the start of the circuit, an ancilla carries |0>, so a Z
//     there only fixes the sign and is dropped, and an X or Y means the
//     outcome is random;
//   * an earlier measurement of the same qubit would make the bit depend on
//     that earlier outcome, which is reported too;
//
// and what is left must be the declared generator on the data qubits: m[i]
// measures the i-th entry of `g:`. Every syndrome bit must be measured. A flag
// measurement must be deterministic and must not reveal anything about the
// encoded state: what is left on the data must be in the stabilizer group
// (the identity, for a circuit without errors).

#include "ftec/dag.hpp"
#include "ftec/qasm.hpp"

#include <string>
#include <vector>

namespace ftec {

// Problems with one circuit, one line each; empty if it measures what it
// declares. `stabilizers` generates the code's stabilizer group.
[[nodiscard]] std::vector<std::string> check_se_circuit(const QasmProgram&             program,
                                                        const CircuitRef&              circuit,
                                                        const std::vector<std::string>& stabilizers);

// check_se_circuit for every distinct circuit the protocol runs, each once.
// Problems are prefixed with the SE name and the circuit file.
[[nodiscard]] std::vector<std::string> check_se_circuits(const Dag& dag);

} // namespace ftec
