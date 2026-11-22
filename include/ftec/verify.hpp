#pragma once

#include "ftec/backend.hpp"
#include "ftec/dag.hpp"

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ftec {

// One SE invocation along a path, together with what it measured.
struct RecordEntry {
    std::size_t node = 0;      // the Se node in the dag
    std::string se_name;
    Outcome     outcome;       // flips: what errors did to each bit
    // The SE's g: rows written -[...]: those bits read 1 without errors.
    std::vector<bool> negated;

    // Syndrome bit i as the circuit reads it: the flip, XOR 1 for a -[...]
    // row. Branch guards and printed records use these; everything that
    // compares errors (validity, correctability) uses the flips.
    [[nodiscard]] bool read_bit(std::size_t i) const {
        return outcome.syndrome[i] != (i < negated.size() && negated[i]);
    }
    [[nodiscard]] std::vector<bool> read_syndrome() const {
        std::vector<bool> out(outcome.syndrome.size());
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = read_bit(i);
        return out;
    }
};

// A path the code does not protect.
struct PathFailure {
    std::size_t              path_id = 0;
    std::string              terminal_action;
    std::vector<RecordEntry> record;      // what was measured on the way here
    Failure                  failure;

    [[nodiscard]] std::string record_string() const;   // "01|1|0"
};

// A terminal at which the protocol is not valid (Def. 3.9): none of the three
// control-flow conditions holds for what can have happened along this record.
struct ValidityFailure {
    std::size_t              path_id = 0;
    std::string              terminal_action;
    std::vector<RecordEntry> record;
    std::string              reason;

    [[nodiscard]] std::string record_string() const;
};

// A circuit used in the repeated suffix of a record that has the form of
// Condition 2 but does not satisfy the (t - w)-flag condition (Def. 3.10 (3)),
// or raises a flag without any fault (Def. 3.7 (2)). One entry per circuit
// and k, with the first record that needed it.
struct FlagFailure {
    std::string              se_name;
    int                      k = 0;
    std::string              problem;
    std::set<std::size_t>    paths;
    std::size_t              records = 0;
    std::size_t              path_id = 0;      // first example
    std::vector<RecordEntry> record;

    [[nodiscard]] std::string record_string() const;
};

struct VerifyOptions {
    // Stop as soon as a path fails. The smallest failing fault count is still
    // correct: the walk visits states in fault-count order within a node, and
    // min_fault_count is taken over everything seen so far.
    bool stop_at_first_failure = false;

    // Protocol validity (Def. 3.10), checked as in the paper's algorithm:
    //   control flow (Def. 3.9): a terminal that decodes needs Cond 1 (the
    //     record determines the ideal syndrome) or Cond 2 (it ends in t - w + 1
    //     identical, unflagged, full rounds, w counting the faults it exposes);
    //     a terminal that ends without decoding needs Cond 3 (an error from s
    //     faults is within weight s of the code space);
    //   flag condition (Def. 3.10 (3)): wherever a record has the form of
    //     Cond 2, the circuits of its repeated suffix satisfy the
    //     (t - w)-flag condition, whether or not Cond 1 also holds.
    bool check_validity = true;
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

    // Protocol validity, when checked: how many terminal records each
    // condition settled (decoding terminals: Cond 1, else Cond 2; ending
    // terminals: Cond 3), the records none did (control-flow bugs), and the
    // circuits that fail the flag condition a Cond 2 suffix needs (flag bugs).
    bool                         validity_checked = false;
    double                       validity_seconds = 0;   // all validity work at terminals
    std::size_t                  by_condition[3]  = {0, 0, 0};
    std::vector<ValidityFailure> validity_failures;   // at most one per decoding path
    std::size_t                  invalid_records = 0; // all records behind them
    std::vector<FlagFailure>     flag_failures;

    [[nodiscard]] bool clean() const { return failures.empty(); }
    [[nodiscard]] bool control_flow_valid() const { return validity_failures.empty(); }
    [[nodiscard]] bool flags_valid() const { return flag_failures.empty(); }
    [[nodiscard]] bool valid() const { return control_flow_valid() && flags_valid(); }
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
