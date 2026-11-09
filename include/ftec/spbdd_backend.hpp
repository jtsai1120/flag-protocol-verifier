#pragma once

#include "ftec/backend.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ftec {

struct SpbddOptions {
    // How the final E1E2 query is answered, i.e. which of spbdd::code's four
    // ways of finding two errors whose product is a non-trivial logical
    // operator. All four decide the same question; they differ in cost, and
    // which witness pair comes back is the library's business.
    //
    //   bruteforce  enumerate the errors and multiply every pair
    //   table       enumerate the errors once, keyed by syndrome
    //   image       dynamic programming over the diagram of the errors
    //   square      is E*E meeting N(S)-S?
    //
    // The default is the one the library itself defaults to.
    std::string fqcv = "image";
};

[[nodiscard]] std::vector<std::string> spbdd_fqcv_methods();
[[nodiscard]] bool is_spbdd_fqcv_method(const std::string& name);

// The error model, over the SPBDD library: Pauli sets as decision diagrams,
// a state being the family of sets indexed by fault count, a two-qubit gate
// spawning level t into level t+1, and a measurement splitting the state on the
// outcome it can produce. The set operations this needs (fault injection,
// reset, the measurement split, the E1E2 query) are the library's own
// vocabulary.
//
// statistics() reports what the final E1E2 query cost, separately from
// everything before it: time, the largest memory growth during one call, and
// the size of the diagrams it was given (for "square", also N(S)-S and E*E).
// tools/bench_fqcv.py tabulates it.
std::unique_ptr<Backend> make_spbdd_backend(const SpbddOptions& options = {});

} // namespace ftec
