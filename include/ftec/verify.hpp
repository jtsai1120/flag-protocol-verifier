#pragma once

#include "ftec/backend.hpp"
#include "ftec/dag.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ftec {

// One SE invocation along a path, together with what it measured.
struct RecordEntry {
    std::size_t node = 0;      // the Se node in the dag
    std::string se_name;
    Outcome     outcome;
};

// A path the code does not protect.
struct PathFailure {
    std::size_t              path_id = 0;
    std::string              terminal_action;
    std::vector<RecordEntry> record;      // what was measured on the way here
    Failure                  failure;

    [[nodiscard]] std::string record_string() const;   // "01|1|0"
};

struct VerifyOptions {
    // Stop as soon as a path fails. The smallest failing fault count is still
    // correct: the walk visits states in fault-count order within a node, and
    // min_fault_count is taken over everything seen so far.
    bool stop_at_first_failure = false;
};

struct VerifyResult {
    std::string protocol;
    int         tau              = 0;
    // A symbolic path can be reached by many concrete measurement records:
    // the guards partition outcomes coarsely ("s != 0" covers several), and
    // every distinct record descends on its own because later branches and the
    // decoder both read it. Both numbers matter, and conflating them hides
    // whichever one is exploding.
    std::size_t paths_reached     = 0;   // distinct symbolic paths
    std::size_t records_reached   = 0;   // terminal visits, one per record
    std::size_t se_applications   = 0;   // circuits actually run
    std::size_t states_explored   = 0;
    int         min_fault_count  = -1;  // fewest faults that break the protocol
    std::vector<PathFailure> failures;

    [[nodiscard]] bool clean() const { return failures.empty(); }
};

// Walk the trie depth-first, carrying symbolic states through each SE and
// routing every measurement outcome to the branch whose guards it satisfies.
//
// Depth-first is what makes the shared prefixes pay off. Each node's states
// are computed once and live in the stack frame while every subtree below it
// runs, so a node is never recomputed for a sibling; and only the states along
// the current root-to-node chain are alive at any moment, rather than a whole
// frontier.
[[nodiscard]] VerifyResult verify(const Dag& dag, Backend& backend,
                                  const VerifyOptions& options = {});

} // namespace ftec
