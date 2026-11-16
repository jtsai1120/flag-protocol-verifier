#pragma once

#include "ftec/dag.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ftec {

// What one SE invocation produced classically: the value of its cm register
// and, for a flagged SE, its cf register. These are what the protocol's
// branch conditions read as id_N.s and id_N.f.
struct Outcome {
    std::vector<bool> syndrome;
    std::vector<bool> flag;

    [[nodiscard]] std::uint64_t syndrome_value() const;
    [[nodiscard]] std::uint64_t flag_value() const;
    [[nodiscard]] std::string   to_string() const;   // "s=0101 f=1"
};

// Why a path is not protected: two errors it cannot tell apart whose product
// acts non-trivially on the encoded state.
struct Failure {
    int         fault_count = 0;
    std::string detail;      // backend-specific, e.g. the two Paulis
};

// A representation of "the set of error configurations still possible".
//
// The verifier owns the search; a backend owns how states are represented and
// how a circuit acts on them. Splitting on the measurement outcome lives here
// rather than in the driver because only the backend knows which outcomes are
// reachable -- that is exactly the information a decision diagram holds.
//
// States are handles, not values: a backend may keep them in whatever store it
// likes, and the driver only ever passes back handles it was given.
class Backend {
public:
    using StateId = std::size_t;

    virtual ~Backend() = default;

    // Called once, before any stepping. The fault budget comes from the code
    // distance, so a backend never has to work it out.
    virtual void begin(const fpdl::CodeSpec& code, int tau) = 0;

    // The state before anything has run: no faults, empty record.
    //
    // A state is the whole family of fault counts at once, not one level of
    // it. A circuit moves probability between levels -- a fault at a two-qubit
    // gate takes the t-fault set into the t+1-fault one -- so a backend that
    // could only be handed one level would have nowhere to put the result.
    [[nodiscard]] virtual StateId initial_state() = 0;

    // Run one SE and split on what it measured. Every returned state is
    // non-empty; an outcome that cannot occur is simply absent.
    //
    // Merging belongs here rather than to the driver. Reaching a point with
    // the same record after t faults is one situation however the faults were
    // distributed over the circuit, so a backend unions those within a state;
    // but two *different* outcomes are different records and never merge,
    // since later branches and the decoder both read them.
    [[nodiscard]] virtual std::vector<std::pair<Outcome, StateId>>
    step(StateId, const CircuitRef&) = 0;

    // Does this state contain two errors whose product is a logical operator?
    [[nodiscard]] virtual std::optional<Failure> check(StateId) = 0;

    // What protocol validity (Def. 3.9 of the verification paper) asks of the
    // residual errors at a terminal:
    //   same_syndrome   Condition 1: every error has the same ideal syndrome;
    //   weight_bounded  Condition 3: every error from an input of weight r and
    //                   s faults has minimum weight at most s.
    // `syndrome_detail` and `weight_detail` name witnesses when the
    // condition fails.
    struct TerminalFacts {
        bool        same_syndrome  = false;
        bool        weight_bounded = false;
        std::string syndrome_detail;
        std::string weight_detail;
    };
    [[nodiscard]] virtual TerminalFacts terminal_facts(StateId) {
        throw std::runtime_error("this backend cannot check protocol validity");
    }

    // Part (1) of the k-flag condition (Def. 3.7) for one circuit: with no
    // input error and 1 <= s <= k faults in it, every execution that raises no
    // flag leaves a data error of minimum weight at most s. nullopt if it
    // holds; otherwise a description of a counterexample.
    [[nodiscard]] virtual std::optional<std::string> flag_condition(const CircuitRef&, int /*k*/) {
        throw std::runtime_error("this backend cannot check the flag condition");
    }

    // For diagnostics only.
    [[nodiscard]] virtual std::string describe(StateId) const { return {}; }

    // Whatever the backend wants to say about how the run went; printed after
    // the result. Empty when it has nothing to add.
    [[nodiscard]] virtual std::string statistics() const { return {}; }

    // The peak resident memory of the process, for a backend that resets the
    // kernel's high-water mark to measure one phase of the run and so has to
    // keep the overall peak itself. 0 means "ask the kernel".
    [[nodiscard]] virtual std::size_t peak_memory_bytes() const { return 0; }
};

using BackendPtr = std::unique_ptr<Backend>;

} // namespace ftec
