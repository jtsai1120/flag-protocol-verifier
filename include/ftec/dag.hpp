#pragma once

#include "fpdl/parser.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ftec {

// Which QASM to run and what the registers in it mean. Assembled from an SE
// declaration; the qasm path is resolved against the .fpdl's directory.
//
// Only the quantum registers appear. Where a measurement outcome lands is not
// a separate fact to declare -- `m[0] = measure syn;` already binds the
// classical register to the qubit -- so a backend reads it off the circuit.
// Whether an SE is flagged is `flag_qubits.has_value()`.
struct CircuitRef {
    std::string           se_name;
    std::filesystem::path qasm;

    std::string                data_qubits;      // qd:
    std::string                syndrome_qubits;  // qm:
    std::optional<std::string> flag_qubits;      // qf:

    std::vector<std::string> measures;           // g:
};

struct DagNode {
    enum class Kind { Start, Se, Terminal };

    Kind        kind = Kind::Start;
    std::size_t id   = 0;

    // Kind::Se
    std::size_t round      = 0;
    std::size_t invocation = 0;
    std::string phase;
    CircuitRef  circuit;

    // Kind::Terminal
    std::size_t                path_id = 0;
    std::string                terminal_action;
    std::optional<int>         terminal_condition;
    std::optional<std::string> decode_record;
};

// Control moves along an edge when every guard holds. A guard is satisfied
// when its condition evaluates to `expected`.
struct DagEdge {
    std::size_t                          from = 0;
    std::size_t                          to   = 0;
    std::vector<fpdl::SymbolicConstraint> guards;
};

// The symbolic paths, merged on their common prefixes.
//
// This is a trie: every node has exactly one parent, because two paths are
// merged precisely while their (guard, SE) steps agree and never rejoin after
// they differ. That is what makes a depth-first walk visit each SE once
// instead of once per path -- for CB18 [[17,1,5]] the difference is 1000
// circuit applications against 8716.
struct Dag {
    std::string              protocol;
    fpdl::CodeSpec           code;
    std::vector<DagNode>     nodes;
    std::vector<DagEdge>     edges;
    std::size_t              path_count = 0;
    bool                     truncated  = false;

    [[nodiscard]] std::vector<std::size_t> successors(std::size_t node) const;
    [[nodiscard]] const DagEdge&           edge_to(std::size_t from, std::size_t to) const;
    [[nodiscard]] std::size_t              root() const { return 0; }
};

// `source_dir` is the directory the .fpdl lived in; SE `file:` values are
// relative to it.
[[nodiscard]] Dag build_dag(const fpdl::ParseResult& parsed,
                            const std::filesystem::path& source_dir);

} // namespace ftec
