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

// A terminating path that satisfies none of Conditions 1-3 of Def. 3.9 (a
// path satisfies a condition when every record on it does); one per path,
// with an example record.
struct ValidityFailure {
    std::size_t              path_id = 0;
    std::string              terminal_action;
    std::vector<RecordEntry> record;
    std::string              reason;

    [[nodiscard]] std::string record_string() const;
};

// How the records of one path fare: how many satisfy each condition (a
// record may satisfy several) and how many satisfy none.
struct PathValidity {
    std::size_t              path_id = 0;
    std::string              terminal_action;
    std::size_t              records = 0, cond1 = 0, cond2 = 0, cond3 = 0, failing = 0;
    std::vector<RecordEntry> example;
};

// A circuit used in the repeated suffix of a path that satisfies Condition 2
// but does not satisfy the (t - m)-flag condition (Def. 3.9 (2), Def. 3.7):
// part (1), or a flag raised without any fault (part (2)). One entry per
// circuit and k, with an example record.
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

    // Protocol validity (Def. 3.9), path by path, as in Algorithm 1:
    //   (1) every terminating path satisfies Cond 1 (its records show t
    //     detected faults and leave residual errors of one ideal syndrome
    //     each), Cond 2 (m < t detected faults, then t - m + 1 identical,
    //     unflagged full rounds) or Cond 3 (input weight r plus s faults,
    //     r + s <= t, leave minimum weight <= s); an end() path applies no
    //     correction and needs Cond 3;
    //   (2) every path satisfying Cond 2 has its repeated suffix satisfy the
    //     (t - m)-flag condition, whichever condition settles the path.
    bool check_validity = true;
    // Def. 3.9 lets a decode() terminal be settled by Cond 3 too (the decoder
    // Thm. 3.10 builds then applies no correction). The paper's experiments
    // only try Cond 1 there; false reproduces that.
    bool decode_may_use_cond3 = true;
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

    // Protocol validity, when checked: how many terminating paths each
    // condition settled (first of 1, 2, 3 that holds), the paths none did
    // (control-flow bugs), and the circuits that fail the flag condition a
    // Cond 2 suffix needs (flag bugs).
    bool                         validity_checked = false;
    double                       validity_seconds = 0;   // all validity work at terminals
    std::size_t                  by_condition[3]  = {0, 0, 0};
    std::vector<ValidityFailure> validity_failures;   // one per invalid path
    std::size_t                  invalid_records = 0; // all records behind them
    std::size_t                  invalid_paths_strict = 0;   // if Cond 3 may not settle decode()
    std::vector<PathValidity>    by_path;
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
