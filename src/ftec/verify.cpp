#include "ftec/verify.hpp"

#include "ftec/circuit_check.hpp"
#include "ftec/qasm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>

namespace ftec {

namespace {

std::uint64_t pack(const std::vector<bool>& bits) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bits.size() && i < 64; ++i) {
        if (bits[i]) value |= (std::uint64_t{1} << i);
    }
    return value;
}

} // namespace

std::uint64_t Outcome::syndrome_value() const { return pack(syndrome); }
std::uint64_t Outcome::flag_value() const { return pack(flag); }

std::string Outcome::to_string() const {
    std::string out = "s=";
    for (const bool bit : syndrome) out += bit ? '1' : '0';
    if (syndrome.empty()) out += '-';
    if (!flag.empty()) {
        out += " f=";
        for (const bool bit : flag) out += bit ? '1' : '0';
    }
    return out;
}

namespace {
std::string record_text(const std::vector<RecordEntry>& record) {
    std::string out;
    for (std::size_t i = 0; i < record.size(); ++i) {
        if (i != 0) out += '|';
        for (const bool bit : record[i].read_syndrome()) out += bit ? '1' : '0';
        if (!record[i].outcome.flag.empty()) {
            out += ',';
            for (const bool bit : record[i].outcome.flag) out += bit ? '1' : '0';
        }
    }
    return out;
}
} // namespace

std::string PathFailure::record_string() const { return record_text(record); }
std::string ValidityFailure::record_string() const { return record_text(record); }
std::string FlagFailure::record_string() const { return record_text(record); }

namespace {

// ---------------------------------------------------------------------------
// Evaluating a branch guard against what has actually been measured.
//
// Terms are not just references and literals. Protocols that repeat a round
// and compare the two compare whole measurement records, so the grammar is
//
//   term := integer | true | false
//         | id_N ( .s | .f ) ( [ i ] )?      the Nth SE's cm / cf register
//         | [] | mr[]                        an empty record
//         | push( term , term )              a record with one more entry
//         | { term , ... }                   a concatenation, e.g. {s, f}
//
// Everything evaluates to a Value, which is either a scalar or a list, and
// equality is structural. That is what makes "the syndromes of rounds 17..32
// match those of rounds 1..16" -- CB18's branch condition -- decidable here
// rather than something the driver has to give up on.
// ---------------------------------------------------------------------------

struct Value {
    bool               is_list = false;
    std::uint64_t      scalar  = 0;
    std::vector<Value> items;

    bool operator==(const Value& other) const {
        if (is_list != other.is_list) return false;
        if (!is_list) return scalar == other.scalar;
        if (items.size() != other.items.size()) return false;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (!(items[i] == other.items[i])) return false;
        }
        return true;
    }
};

class TermReader {
public:
    TermReader(const std::string& text, const std::vector<RecordEntry>& record)
        : text_(text), record_(record) {}

    // Nullopt means "this driver cannot resolve the term", never "false".
    std::optional<Value> read_all() {
        auto value = read();
        if (!value) return std::nullopt;
        skip_space();
        if (position_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void skip_space() {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t')) {
            ++position_;
        }
    }

    bool consume(char c) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == c) { ++position_; return true; }
        return false;
    }

    bool consume_word(const std::string& word) {
        skip_space();
        if (text_.compare(position_, word.size(), word) != 0) return false;
        position_ += word.size();
        return true;
    }

    std::optional<std::size_t> read_index() {
        skip_space();
        const std::size_t begin = position_;
        while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            ++position_;
        }
        if (position_ == begin) return std::nullopt;
        return std::stoull(text_.substr(begin, position_ - begin));
    }

    std::optional<Value> read() {
        skip_space();
        if (position_ >= text_.size()) return std::nullopt;

        if (consume('(')) {
            auto inner = read();
            if (!inner || !consume(')')) return std::nullopt;
            return inner;
        }
        if (consume_word("push")) {
            if (!consume('(')) return std::nullopt;
            auto base = read();
            if (!base || !consume(',')) return std::nullopt;
            auto item = read();
            if (!item || !consume(')')) return std::nullopt;
            if (!base->is_list) return std::nullopt;
            base->items.push_back(*item);
            return base;
        }
        if (consume_word("mr[]") || consume_word("[]")) {
            Value empty;
            empty.is_list = true;
            return empty;
        }
        if (consume('{')) {
            Value list;
            list.is_list = true;
            if (!consume('}')) {
                do {
                    auto item = read();
                    if (!item) return std::nullopt;
                    list.items.push_back(*item);
                } while (consume(','));
                if (!consume('}')) return std::nullopt;
            }
            return list;
        }
        if (consume_word("true"))  { Value v; v.scalar = 1; return v; }
        if (consume_word("false")) { Value v; v.scalar = 0; return v; }

        if (consume_word("id_")) {
            const auto event = read_index();
            if (!event || !consume('.')) return std::nullopt;
            const bool is_flag = consume_word("f");
            if (!is_flag && !consume_word("s")) return std::nullopt;

            std::optional<std::size_t> bit;
            if (consume('[')) {
                bit = read_index();
                if (!bit || !consume(']')) return std::nullopt;
            }
            return resolve(*event, is_flag, bit);
        }

        skip_space();
        if (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) {
            const auto literal = read_index();
            if (!literal) return std::nullopt;
            Value v;
            v.scalar = *literal;
            return v;
        }
        return std::nullopt;
    }

    std::optional<Value> resolve(std::size_t event, bool is_flag,
                                 std::optional<std::size_t> bit) const {
        if (event == 0 || event > record_.size()) return std::nullopt;
        // Guards see the bits as the circuit reads them, so a -[...] row in
        // g: reads 1 when nothing happened.
        const RecordEntry& entry = record_[event - 1];
        const std::vector<bool> bits = is_flag ? entry.outcome.flag : entry.read_syndrome();
        Value value;
        if (bit) {
            if (*bit >= bits.size()) return std::nullopt;
            value.scalar = bits[*bit] ? 1u : 0u;
            return value;
        }
        value.scalar = pack(bits);
        return value;
    }

    const std::string&              text_;
    const std::vector<RecordEntry>& record_;
    std::size_t                     position_ = 0;
};

std::optional<Value> evaluate_term(const std::string& text,
                                   const std::vector<RecordEntry>& record) {
    TermReader reader(text, record);
    return reader.read_all();
}

std::optional<bool> evaluate(const fpdl::Condition& condition,
                             const std::vector<RecordEntry>& record) {
    using Kind = fpdl::Condition::Kind;
    switch (condition.kind) {
        case Kind::Constant:
            return condition.constant;
        case Kind::Equals: {
            const auto lhs = evaluate_term(condition.lhs, record);
            const auto rhs = evaluate_term(condition.rhs, record);
            if (!lhs || !rhs) return std::nullopt;
            return *lhs == *rhs;
        }
        case Kind::And: {
            if (condition.operands.size() != 2) return std::nullopt;
            const auto a = evaluate(condition.operands[0], record);
            const auto b = evaluate(condition.operands[1], record);
            // Short-circuit on a decided false even if the other side is not
            // resolvable: the conjunction is false either way.
            if (a && !*a) return false;
            if (b && !*b) return false;
            if (!a || !b) return std::nullopt;
            return *a && *b;
        }
        case Kind::Or: {
            if (condition.operands.size() != 2) return std::nullopt;
            const auto a = evaluate(condition.operands[0], record);
            const auto b = evaluate(condition.operands[1], record);
            if (a && *a) return true;
            if (b && *b) return true;
            if (!a || !b) return std::nullopt;
            return *a || *b;
        }
        case Kind::Not: {
            if (condition.operands.size() != 1) return std::nullopt;
            const auto inner = evaluate(condition.operands[0], record);
            if (!inner) return std::nullopt;
            return !*inner;
        }
    }
    return std::nullopt;
}

bool edge_admits(const DagEdge& edge, const std::vector<RecordEntry>& record) {
    for (const auto& guard : edge.guards) {
        const auto held = evaluate(guard.condition, record);
        if (!held) {
            throw std::runtime_error(
                "ftec::verify: cannot evaluate branch guard '" + guard.expression +
                "' against the measurement record; the protocol branches on something "
                "this driver does not know how to resolve");
        }
        if (*held != guard.expected) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

class Walker {
public:
    Walker(const Dag& dag, Backend& backend, const VerifyOptions& options,
           VerifyResult& result)
        : dag_(dag), backend_(backend), options_(options), result_(result) {}

    // Returns false to unwind when the caller asked to stop at the first failure.
    bool visit(std::size_t node, Backend::StateId state, std::vector<RecordEntry>& record) {
        ++result_.states_explored;
        const DagNode& current = dag_.nodes[node];

        if (current.kind == DagNode::Kind::Terminal) {
            ++result_.records_reached;
            if (reached_.insert(current.path_id).second) ++result_.paths_reached;
            if (auto failure = backend_.check(state)) {
                note_failure(current, record, *failure);
                if (options_.stop_at_first_failure) return false;
            }
            if (options_.check_validity) {
                const auto started = std::chrono::steady_clock::now();
                check_validity(current, state, record);
                result_.validity_seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
            }
            return true;
        }

        // Start carries no circuit; its single child runs the first SE.
        if (current.kind == DagNode::Kind::Start) {
            return descend(node, state, record);
        }

        // An Se node runs its circuit, then each outcome picks a branch.
        ++result_.se_applications;
        auto outcomes = backend_.step(state, current.circuit);

        // Each outcome descends on its own and nothing is merged across them.
        // Two outcomes may well satisfy the same guard -- "s != 0 or f != 0"
        // accepts three of the four -- but they are different measurement
        // records: later guards read them, and so does the decoder, so they
        // stay apart. Merging happens inside a state instead, between fault
        // counts and between branches of a circuit whose measured bits agree.
        for (auto& [outcome, next_state] : outcomes) {
            record.push_back(RecordEntry{node, current.circuit.se_name, outcome, current.circuit.negated});
            const auto child = route(node, record);
            if (child) {
                if (!visit(*child, next_state, record)) {
                    record.pop_back();
                    return false;
                }
            }
            record.pop_back();
        }
        return true;
    }

private:
    bool descend(std::size_t node, Backend::StateId state, std::vector<RecordEntry>& record) {
        for (const auto& edge : dag_.edges) {
            if (edge.from != node) continue;
            if (!edge_admits(edge, record)) continue;
            if (!visit(edge.to, state, record)) return false;
        }
        return true;
    }

    std::optional<std::size_t> route(std::size_t node, const std::vector<RecordEntry>& record) {
        for (const auto& edge : dag_.edges) {
            if (edge.from != node) continue;
            if (edge_admits(edge, record)) return edge.to;
        }
        return std::nullopt;
    }

    void note_failure(const DagNode& terminal, const std::vector<RecordEntry>& record,
                      const Failure& failure) {
        if (result_.min_fault_count < 0 || failure.fault_count < result_.min_fault_count) {
            result_.min_fault_count = failure.fault_count;
        }
        result_.failures.push_back(PathFailure{terminal.path_id, terminal.terminal_action, record, failure, {}});
    }

  public:
    // The first counterexample of every path (in the reported order), traced.
    void explain_failures() {
        std::set<std::size_t> done;
        for (auto& f : result_.failures) {
            if (!done.insert(f.path_id).second) continue;
            const int budget = f.failure.fault_count;
            for (const auto& w : f.failure.witnesses) {
                f.explained.push_back(backend_.explain(
                    steps_of(f.record), w, [budget](int r, int s) { return r + s <= budget; }));
            }
        }
    }

  private:

    std::vector<ExplainStep> steps_of(const std::vector<RecordEntry>& record) const {
        std::vector<ExplainStep> steps;
        for (const auto& entry : record) steps.push_back(ExplainStep{&dag_.nodes[entry.node].circuit, entry.outcome});
        return steps;
    }


    // ---- protocol validity (Def. 3.9) ---------------------------------------
    //
    // Every record that reaches a terminal must satisfy one of:
    //   Cond 1  the record proves t circuit faults, and the residual errors of
    //           the record have one ideal syndrome (phi_CF1 of Eq. 14 is
    //           unsat). With k = t proved, g + k <= t leaves only level (0, t);
    //   Cond 2  at least m < t detected faults, then t - m + 1 consecutive
    //           identical, unflagged full rounds (structural, no faults searched);
    //   Cond 3  every residual error from input weight r and s faults,
    //           r + s <= t, has minimum weight <= s.
    // A decode() terminal may use any of them (Thm. 3.10 builds the decoder;
    // under Cond 3 it applies no correction). An end() terminal applies no
    // correction by construction, so only Cond 3 can justify it (Sec. 4.1.3).
    // Part (2): wherever the path's guards force the form of Cond 2, the
    // repeated suffix must satisfy the (t - m)-flag condition, whichever
    // condition settles the records.

    // Circuit faults the record proves (detected-fault lower bound, after
    // CB18 Secs. 2.2 and 3.1). The measurements are grouped into rounds (a
    // round ends when a generator is about to be measured again). Each SE
    // circuit of a round that raises a flag is one detected fault (one fault
    // sits in one circuit, and data errors raise no flag); afterwards nothing
    // measured earlier is trusted. An unflagged round that disagrees with an
    // earlier measurement of the same syndrome is one detected fault, except
    // the disagreement right after a counted one, which may be the same fault
    // seen twice (CB18's rule for consecutive rounds); after a disagreement
    // only the new round is trusted. A first measurement is never compared
    // with the trivial syndrome: an input error could explain it, and only
    // circuit faults count (Def. 3.9). Knowledge is kept as GF(2) equations
    // over the generators, so a bit that measures a product of generators is
    // compared too.
    struct Exposure {
        int                      w = 0;
        std::vector<std::size_t> round_of;   // record entry -> round index
        struct Round {
            std::vector<std::pair<std::uint64_t, bool>> bits;   // generator mask, value
            bool                     flagged = false;
            int                      flagged_ses = 0;          // SEs of the round that raised a flag
            bool                     full    = false;          // spans every generator
            std::vector<std::size_t> entries;
        };
        std::vector<Round> rounds;
        bool readable = true;   // every bit is a product of generators
        std::vector<int> w_upto;   // detected faults in the first i rounds
    };

    Exposure exposure(const std::vector<RecordEntry>& record) {
        Exposure ex;
        const std::size_t r = dag_.code.generators.size();
        if (r > 64) throw std::runtime_error("protocol validity supports at most 64 stabilizer generators");
        // rounds
        for (std::size_t i = 0; i < record.size(); ++i) {
            const auto& masks = masks_of(dag_.nodes[record[i].node].circuit);
            std::uint64_t seen = 0;
            for (const auto& b : ex.rounds.empty() ? std::vector<std::pair<std::uint64_t, bool>>{}
                                                   : ex.rounds.back().bits) seen |= b.first;
            bool overlap = ex.rounds.empty();
            for (const auto& m : masks) {
                if (!m) { ex.readable = false; continue; }
                if (!ex.rounds.empty() && (seen & *m)) overlap = true;
            }
            if (overlap) ex.rounds.emplace_back();
            auto& round = ex.rounds.back();
            for (std::size_t j = 0; j < masks.size() && j < record[i].outcome.syndrome.size(); ++j) {
                if (masks[j]) round.bits.emplace_back(*masks[j], record[i].outcome.syndrome[j]);
            }
            bool raised = false;
            for (const bool f : record[i].outcome.flag) raised = raised || f;
            if (raised) { round.flagged = true; ++round.flagged_ses; }
            round.entries.push_back(i);
        }
        for (auto& round : ex.rounds) {
            std::vector<std::uint64_t> masks;
            for (const auto& b : round.bits) masks.push_back(b.first);
            round.full = static_cast<std::size_t>(rank(masks)) == r;
        }
        // count
        std::vector<std::pair<std::uint64_t, bool>> known;   // reduced rows
        bool skip = false;
        ex.w_upto.push_back(0);
        for (const auto& round : ex.rounds) {
            struct Push { Exposure& e; ~Push() { e.w_upto.push_back(e.w); } } push{ex};
            if (round.flagged) { ex.w += round.flagged_ses; known.clear(); skip = false; continue; }
            bool compared = false, changed = false;
            for (const auto& [mask, value] : round.bits) {
                if (const auto p = predict(known, mask)) {
                    compared = true;
                    if (*p != value) changed = true;
                }
            }
            if (compared) {
                if (changed) { if (!skip) ++ex.w; skip = !skip; }
                else skip = false;
            }
            if (changed) known.clear();
            for (const auto& b : round.bits) learn(known, b.first, b.second);
        }
        return ex;
    }

    static int rank(std::vector<std::uint64_t> rows) {
        int rk = 0;
        for (int bit = 63; bit >= 0; --bit) {
            const std::uint64_t m = std::uint64_t{1} << bit;
            auto pivot = std::find_if(rows.begin() + rk, rows.end(), [&](std::uint64_t v) { return v & m; });
            if (pivot == rows.end()) continue;
            std::swap(*pivot, rows[static_cast<std::size_t>(rk)]);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (i != static_cast<std::size_t>(rk) && (rows[i] & m)) rows[i] ^= rows[static_cast<std::size_t>(rk)];
            }
            ++rk;
        }
        return rk;
    }
    // Value of the product `mask` if the known equations fix it.
    static std::optional<bool> predict(const std::vector<std::pair<std::uint64_t, bool>>& known,
                                       std::uint64_t mask) {
        bool value = false;
        for (const auto& [row, v] : known) {   // rows are reduced: distinct leading bits
            const std::uint64_t lead = std::uint64_t{1} << (63 - __builtin_clzll(row));
            if (mask & lead) { mask ^= row; value ^= v; }
        }
        if (mask) return std::nullopt;
        return value;
    }
    static void learn(std::vector<std::pair<std::uint64_t, bool>>& known, std::uint64_t mask, bool value) {
        for (const auto& [row, v] : known) {
            const std::uint64_t lead = std::uint64_t{1} << (63 - __builtin_clzll(row));
            if (mask & lead) { mask ^= row; value ^= v; }
        }
        if (!mask) return;   // already implied
        const std::uint64_t lead = std::uint64_t{1} << (63 - __builtin_clzll(mask));
        for (auto& [row, v] : known) {
            if (row & lead) { row ^= mask; v ^= value; }
        }
        known.emplace_back(mask, value);
    }

    // Trailing rounds of the record that are full, unflagged and identical
    // to the last one (0 if a bit is not a product of generators).
    static std::size_t repeated_suffix(const Exposure& ex) {
        if (!ex.readable || ex.rounds.empty()) return 0;
        const auto& last = ex.rounds.back();
        std::size_t n = 0;
        for (auto it = ex.rounds.rbegin(); it != ex.rounds.rend(); ++it) {
            if (!it->full || it->flagged || it->bits != last.bits) break;
            ++n;
        }
        return n;
    }

    // Fill the per-condition fields of a validity counterexample.
    void explain_validity(ValidityFailure& failure, const Backend::TerminalFacts& facts,
                          const Exposure& ex) {
        failure.detected = ex.w;
        for (std::size_t i = 0; i < ex.rounds.size(); ++i) {
            RoundSummary round;
            for (const std::size_t e : ex.rounds[i].entries) round.steps.push_back(e + 1);
            round.flagged = ex.rounds[i].flagged;
            round.counted = ex.w_upto[i + 1] - ex.w_upto[i];
            failure.rounds.push_back(std::move(round));
        }
        if (!options_.explain) return;
        const int t = dag_.code.fault_budget();
        const auto steps = steps_of(failure.record);
        if (!facts.same_syndrome) {
            for (const auto& w : facts.syndrome_witnesses) {
                failure.split.push_back(backend_.explain(steps, w, [t](int r, int s) { return r == 0 && s == t; }));
            }
        }
        if (!facts.weight_bounded) {
            const int wr = facts.weight_r, ws = facts.weight_s;
            failure.heavy = backend_.explain(steps, facts.weight_witness,
                                             [wr, ws](int r, int s) { return r == wr && s == ws; });
        }
    }

    static ValidityFailure validity_failure(std::size_t path, const std::string& action,
                                            const std::vector<RecordEntry>& record, std::string reason) {
        ValidityFailure failure;
        failure.path_id = path;
        failure.terminal_action = action;
        failure.record = record;
        failure.reason = std::move(reason);
        return failure;
    }

    // Def. 3.9 speaks of the paths of a decision tree that branches on the
    // outcomes, and of conditions those paths' guards guarantee. An FPDL path
    // can merge executions such a tree keeps apart (a guard on a counter), so:
    //   * Cond 1 and Cond 3 are checked record by record (Eq. 14 and the
    //     Cond 3 query quantify over the executions of one record anyway);
    //   * Cond 2 with m = min(c, t - 1) is checked for the class of records of
    //     the path whose rounds before the last t - m + 1 prove exactly c
    //     circuit faults (at least t for c = t). The class is what a guard on
    //     the faults detected so far can tell apart; Cond 2 holds for it when
    //     EVERY record in it ends in t - m + 1 identical, unflagged full
    //     rounds, so the guards, not chance, force the repetition.
    // A path is a control-flow bug when one of its records satisfies none.
    struct ClassState {
        bool                     form = true;
        std::size_t              records = 0;
        std::map<std::string, std::pair<const CircuitRef*, std::size_t>> flags;
        std::vector<RecordEntry> flags_example;
    };
    struct Needing {                    // a record only Cond 2 could settle
        std::uint32_t            classes = 0;   // classes it has the form in
        bool                     strict  = false;   // needs it only if Cond 3 may not settle decode()
    };
    struct PathState {
        bool                     decode = true;
        std::size_t              records = 0, c1 = 0, c3 = 0;
        std::vector<RecordEntry> first;
        std::size_t              failing = 0;   // records nothing settles (end paths)
        std::optional<ValidityFailure> example;
        std::map<int, ClassState> classes;
        std::vector<Needing>     needing;
        std::vector<ValidityFailure> needing_examples;   // parallel to needing, first few kept
    };
    std::map<std::size_t, PathState> paths_;

    void check_validity(const DagNode& terminal, Backend::StateId state,
                        const std::vector<RecordEntry>& record) {
        const auto facts = backend_.terminal_facts(state);
        const Exposure ex = exposure(record);
        const int t = dag_.code.fault_budget();
        PathState& path = paths_[terminal.path_id];
        path.decode = terminal.terminal_action != "end";
        if (path.records++ == 0) path.first = record;

        const bool c1 = ex.w >= t && facts.same_syndrome;
        const bool c3 = facts.weight_bounded;
        if (c1) ++path.c1;
        if (c3) ++path.c3;
        if (!path.decode) {
            if (c3) { ++result_.by_condition[2]; return; }
            if (!path.example) {
                path.example = validity_failure(terminal.path_id, terminal.terminal_action, record,
                    "ends without decoding, so only Cond 3 applies, but " + facts.weight_detail);
                path.example->why1 = path.example->why2 = "does not apply: an end() terminal applies no correction";
                path.example->why3 = facts.weight_detail;
                explain_validity(*path.example, facts, ex);
            }
            ++path.failing;
            return;
        }

        // Classes and the form of Cond 2.
        std::uint32_t with_form = 0;
        const std::size_t n = ex.rounds.size();
        const std::size_t have = repeated_suffix(ex);
        for (int c = 0; c <= t && t >= 1; ++c) {
            const int m = std::min(c, t - 1);
            const std::size_t need = static_cast<std::size_t>(t - m + 1);
            const std::size_t prefix = n >= need ? n - need : n;
            const int wp = ex.w_upto[prefix];
            if (c < t ? wp != c : wp < t) continue;
            ClassState& cls = path.classes[c];
            ++cls.records;
            const bool form = ex.readable && n >= need && have >= need;
            if (!form) { cls.form = false; cls.flags.clear(); continue; }
            with_form |= std::uint32_t{1} << c;
            if (!cls.form) continue;
            if (cls.flags_example.empty()) cls.flags_example = record;
            for (std::size_t r = n - need; r < n; ++r) {
                for (const std::size_t e : ex.rounds[r].entries) {
                    const CircuitRef& circuit = dag_.nodes[record[e].node].circuit;
                    auto& slot = cls.flags[circuit.qasm.string() + "\n" + circuit.se_name];
                    slot.first = &circuit;
                    ++slot.second;
                }
            }
        }

        if (c1) { ++result_.by_condition[0]; return; }
        const bool by3 = c3 && options_.decode_may_use_cond3;
        if (by3) ++result_.by_condition[2];
        path.needing.push_back(Needing{with_form, by3});
        if (path.needing_examples.size() < 1 && !by3) {
            const std::string why1 = !facts.same_syndrome ? facts.syndrome_detail
                : "the record proves " + std::to_string(ex.w) + " circuit fault(s), fewer than t = " + std::to_string(t);
            const std::string why2 = !ex.readable
                ? std::string("an SE measures something that is not a product of the code's generators")
                : with_form ? std::string("the guards do not force the repeated rounds (other records of this path in the same class do not repeat)")
                            : "the record does not end in t - m + 1 identical unflagged full rounds (it has " + std::to_string(have) + ")";
            path.needing_examples.push_back(validity_failure(terminal.path_id, terminal.terminal_action, record,
                "Cond 1: " + why1 + "; Cond 2: " + why2 + "; Cond 3: " + facts.weight_detail));
            auto& example = path.needing_examples.back();
            example.why1 = why1;
            example.why2 = why2;
            example.why3 = facts.weight_detail;
            explain_validity(example, facts, ex);
        }
    }

  public:
    // Settle Cond 2 class by class, report the paths with a record nothing
    // settles, and check the flag condition of every class where Cond 2 holds
    // (Def. 3.9 (2)), whichever condition settles its records.
    void finish() {
        const int t = dag_.code.fault_budget();
        for (auto& [path_id, path] : paths_) {
            std::uint32_t forced = 0;
            for (const auto& [c, cls] : path.classes) if (cls.form && cls.records) forced |= std::uint32_t{1} << c;
            std::optional<ValidityFailure> example = path.example;
            std::size_t failing = path.failing;
            bool strict_bug = path.failing > 0;
            PathValidity pv{path_id, path.decode ? "decode" : "end", path.records, path.c1, 0, path.c3, 0, {}};
            pv.example = path.first;
            for (const auto& nd : path.needing) {
                const bool by2 = (nd.classes & forced) != 0;
                if (by2) ++pv.cond2;
                if (!by2) strict_bug = true;
                if (by2) { if (!nd.strict) ++result_.by_condition[1]; continue; }
                if (nd.strict) continue;          // settled by Cond 3
                ++failing;
                if (!example && !path.needing_examples.empty()) example = path.needing_examples.front();
            }
            for (auto& [c, cls] : path.classes) {
                if (!(forced >> c & 1)) continue;
                const int k = t - std::min(c, t - 1);
                for (const auto& [key, slot] : cls.flags) {
                    const CircuitRef& circuit = *slot.first;
                    std::optional<std::string> problem;
                    if (k > 0) problem = backend_.flag_condition(circuit, k);
                    if (!problem && !flags_ignore(circuit)) problem = std::string("raises a flag without any fault");
                    if (!problem) continue;
                    FlagFailure* entry = nullptr;
                    for (auto& f : result_.flag_failures) {
                        if (f.se_name == circuit.se_name && f.k == k) entry = &f;
                    }
                    if (!entry) {
                        result_.flag_failures.push_back(
                            FlagFailure{circuit.se_name, k, *problem, {}, 0, path_id, cls.flags_example, circuit, {}});
                        entry = &result_.flag_failures.back();
                        if (options_.explain && k > 0) entry->witness = backend_.explain_flag(circuit, k);
                    }
                    entry->paths.insert(path_id);
                    entry->records += slot.second;
                }
            }
            if (strict_bug) ++result_.invalid_paths_strict;
            pv.failing = failing;
            result_.by_path.push_back(std::move(pv));
            if (example && failing) {
                result_.validity_failures.push_back(*example);
                extra_invalid_records_ += failing - 1;
            }
        }
        result_.invalid_records = result_.validity_failures.size() + extra_invalid_records_;
    }

  private:
    std::size_t extra_invalid_records_ = 0;

    // Each bit of an SE as a product of the code's generators (a bit mask
    // over them), or nullopt if it is not in the stabilizer group (up to
    // sign). Pauli strings are compared as binary symplectic vectors.
    const std::vector<std::optional<std::uint64_t>>& masks_of(const CircuitRef& circuit) {
        const std::string key = circuit.qasm.string() + "\n" + circuit.se_name;
        if (const auto found = masks_.find(key); found != masks_.end()) return found->second;
        const auto& gens = dag_.code.generators;
        const auto vec = [](const std::string& p) {
            std::vector<bool> v(2 * p.size());
            for (std::size_t q = 0; q < p.size(); ++q) {
                v[2 * q]     = p[q] == 'X' || p[q] == 'Y';
                v[2 * q + 1] = p[q] == 'Z' || p[q] == 'Y';
            }
            return v;
        };
        // Row-reduce the generators, remembering each row as a combination.
        std::vector<std::pair<std::vector<bool>, std::uint64_t>> rows;
        for (std::size_t g = 0; g < gens.size(); ++g) rows.emplace_back(vec(gens[g]), std::uint64_t{1} << g);
        std::vector<std::size_t> pivots;
        std::size_t rk = 0;
        const std::size_t width = rows.empty() ? 0 : rows[0].first.size();
        for (std::size_t c = 0; c < width && rk < rows.size(); ++c) {
            std::size_t p = rk;
            while (p < rows.size() && !rows[p].first[c]) ++p;
            if (p == rows.size()) continue;
            std::swap(rows[p], rows[rk]);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                if (i == rk || !rows[i].first[c]) continue;
                for (std::size_t j = 0; j < width; ++j) rows[i].first[j] = rows[i].first[j] ^ rows[rk].first[j];
                rows[i].second ^= rows[rk].second;
            }
            pivots.push_back(c);
            ++rk;
        }
        std::vector<std::optional<std::uint64_t>> out;
        for (const auto& m : circuit.measures) {
            auto v = vec(m);
            if (v.size() != width) { out.push_back(std::nullopt); continue; }
            std::uint64_t mask = 0;
            for (std::size_t i = 0; i < pivots.size(); ++i) {
                if (!v[pivots[i]]) continue;
                for (std::size_t j = 0; j < width; ++j) v[j] = v[j] ^ rows[i].first[j];
                mask ^= rows[i].second;
            }
            const bool zero = std::none_of(v.begin(), v.end(), [](bool b) { return b; });
            out.push_back(zero ? std::optional<std::uint64_t>(mask) : std::nullopt);
        }
        return masks_.emplace(key, std::move(out)).first->second;
    }

    bool flags_ignore(const CircuitRef& circuit) {
        const std::string key = circuit.qasm.string();
        if (const auto found = ignore_.find(key); found != ignore_.end()) return found->second;
        const bool ok = flags_ignore_data(parse_qasm_file(circuit.qasm), circuit);
        return ignore_.emplace(key, ok).first->second;
    }

    std::map<std::string, std::vector<std::optional<std::uint64_t>>> masks_;

    std::map<std::string, bool>                                    ignore_;

    std::set<std::size_t> reached_;
    const Dag&           dag_;
    Backend&             backend_;
    const VerifyOptions& options_;
    VerifyResult&        result_;
};

} // namespace

VerifyResult verify(const Dag& dag, Backend& backend, const VerifyOptions& options) {
    VerifyResult result;
    result.protocol = dag.protocol;
    result.tau      = dag.code.fault_budget();

    backend.begin(dag.code, result.tau);
    result.validity_checked = options.check_validity;

    std::vector<RecordEntry> record;
    Walker walker(dag, backend, options, result);
    walker.visit(dag.root(), backend.initial_state(), record);
    if (options.check_validity) {
        const auto started = std::chrono::steady_clock::now();
        walker.finish();
        result.validity_seconds +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }

    std::stable_sort(result.failures.begin(), result.failures.end(),
                     [](const PathFailure& a, const PathFailure& b) {
                         if (a.failure.fault_count != b.failure.fault_count) {
                             return a.failure.fault_count < b.failure.fault_count;
                         }
                         return a.path_id < b.path_id;
                     });
    if (options.explain) walker.explain_failures();
    return result;
}

} // namespace ftec
