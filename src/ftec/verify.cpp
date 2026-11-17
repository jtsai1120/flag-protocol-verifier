#include "ftec/verify.hpp"

#include "ftec/circuit_check.hpp"
#include "ftec/qasm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
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
        for (const bool bit : record[i].outcome.syndrome) out += bit ? '1' : '0';
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
        const Outcome& outcome = record_[event - 1].outcome;
        const std::vector<bool>& bits = is_flag ? outcome.flag : outcome.syndrome;
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
            record.push_back(RecordEntry{node, current.circuit.se_name, outcome});
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
        result_.failures.push_back(
            PathFailure{terminal.path_id, terminal.terminal_action, record, failure});
    }

    // ---- protocol validity -------------------------------------------------

    // The form of Condition 2, read off the record (structural, no faults
    // searched): w faults exposed, then t - w + 1 identical, unflagged, full
    // rounds. The flag condition of the suffix is checked separately.
    struct Cond2 {
        bool                     holds = false;
        int                      k     = 0;        // t - w
        std::vector<std::size_t> suffix;           // record entries of the repeated rounds
        std::string              why_not;
    };

    // Condition 2 is a property of a path (its guards make the suffix
    // repeat), not of a record that happens to repeat. A path counts as having
    // the form of Cond 2 when every record reaching it has that form; this is
    // only known once the walk is over, so the records that need Cond 2 and
    // the flag conditions it asks for are kept per path until finish().
    struct PathCond2 {
        bool                                    every_record = true;
        int                                     k = -1;        // t - w, the same on every record
        std::map<std::pair<std::string, int>, std::pair<const CircuitRef*, std::size_t>> flags;
        std::vector<RecordEntry>                flags_example;
        std::size_t                             needing = 0;   // records Cond 1 does not settle
        std::optional<ValidityFailure>          needing_example;
        std::size_t                             settled_by_2 = 0;
    };
    std::map<std::size_t, PathCond2> cond2_;

    void check_validity(const DagNode& terminal, Backend::StateId state,
                        const std::vector<RecordEntry>& record) {
        const auto facts = backend_.terminal_facts(state);
        if (terminal.terminal_action == "end") {
            if (facts.weight_bounded) { ++result_.by_condition[2]; return; }
            result_.validity_failures.push_back(ValidityFailure{
                terminal.path_id, terminal.terminal_action, record,
                "ends without decoding, but " + facts.weight_detail});
            return;
        }
        PathCond2& path = cond2_[terminal.path_id];
        const Cond2 form = condition_2(record);
        // The guards make the suffix repeat only if every record has it.
        if (!form.holds) {
            path.every_record = false;
        } else if (path.every_record) {
            if (path.flags_example.empty()) path.flags_example = record;
            for (const std::size_t e : form.suffix) {
                const CircuitRef& circuit = dag_.nodes[record[e].node].circuit;
                auto& slot = path.flags[{circuit.qasm.string() + "\n" + circuit.se_name, form.k}];
                slot.first = &circuit;
                ++slot.second;
            }
        }
        if (facts.same_syndrome) { ++result_.by_condition[0]; return; }
        ++path.needing;
        if (!path.needing_example) {
            path.needing_example = ValidityFailure{
                terminal.path_id, terminal.terminal_action, record,
                "Cond 1: " + facts.syndrome_detail + "; Cond 2: " +
                    (form.holds ? std::string("other records of this path do not repeat")
                                : form.why_not)};
        }
    }

  public:
    // Settle Cond 2 path by path, and check the flag condition of every path
    // that has its form (Def. 3.10 (3)), whichever condition settles it.
    void finish() {
        for (auto& [path_id, path] : cond2_) {
            if (path.every_record) {
                result_.by_condition[1] += path.needing;
                for (const auto& [key, slot] : path.flags) {
                    const CircuitRef& circuit = *slot.first;
                    const int k = key.second;
                    std::optional<std::string> problem;
                    if (k > 0) problem = backend_.flag_condition(circuit, k);
                    if (!problem && !flags_ignore(circuit)) {
                        problem = std::string("raises a flag without any fault");
                    }
                    if (!problem) continue;
                    FlagFailure* entry = nullptr;
                    for (auto& f : result_.flag_failures) {
                        if (f.se_name == circuit.se_name && f.k == k) entry = &f;
                    }
                    if (!entry) {
                        result_.flag_failures.push_back(
                            FlagFailure{circuit.se_name, k, *problem, {}, 0, path_id, path.flags_example});
                        entry = &result_.flag_failures.back();
                    }
                    entry->paths.insert(path_id);
                    entry->records += slot.second;
                }
            } else if (path.needing_example) {
                // Cond 1 fails on these records and the path has no Cond 2 form.
                for (std::size_t i = 0; i < path.needing; ++i) {
                    if (i == 0) result_.validity_failures.push_back(*path.needing_example);
                    else ++extra_invalid_records_;
                }
            }
        }
        result_.invalid_records = result_.validity_failures.size() + extra_invalid_records_;
    }

  private:
    std::size_t extra_invalid_records_ = 0;

    // One round of syndrome extraction, reassembled from consecutive SEs.
    struct Round {
        std::map<std::size_t, bool> syndrome;   // generator index -> bit
        bool                        flagged = false;
        std::vector<std::size_t>    entries;    // into the record
    };

    // Which code generator each bit of an SE reports, or nullopt for a bit
    // that is not one of them (a product, say).
    const std::vector<std::optional<std::size_t>>& bits_of(const CircuitRef& circuit) {
        const std::string key = circuit.qasm.string() + "\n" + circuit.se_name;
        if (const auto found = bits_.find(key); found != bits_.end()) return found->second;
        std::vector<std::optional<std::size_t>> bits;
        for (const auto& g : circuit.measures) {
            std::optional<std::size_t> index;
            for (std::size_t i = 0; i < dag_.code.generators.size(); ++i) {
                if (dag_.code.generators[i] == g) index = i;
            }
            bits.push_back(index);
        }
        return bits_.emplace(key, std::move(bits)).first->second;
    }

    bool flags_ignore(const CircuitRef& circuit) {
        const std::string key = circuit.qasm.string();
        if (const auto found = ignore_.find(key); found != ignore_.end()) return found->second;
        const bool ok = flags_ignore_data(parse_qasm_file(circuit.qasm), circuit);
        return ignore_.emplace(key, ok).first->second;
    }

    Cond2 condition_2(const std::vector<RecordEntry>& record) {
        const auto no = [](std::string why) { Cond2 c; c.why_not = std::move(why); return c; };
        const std::size_t generators = dag_.code.generators.size();
        std::vector<Round> rounds;
        bool readable = true;
        for (std::size_t i = 0; i < record.size(); ++i) {
            const CircuitRef& circuit = dag_.nodes[record[i].node].circuit;
            const auto& bits = bits_of(circuit);
            bool overlap = rounds.empty();
            for (const auto& b : bits) {
                if (!b) { readable = false; continue; }
                if (!rounds.empty() && rounds.back().syndrome.count(*b)) overlap = true;
            }
            if (overlap) rounds.emplace_back();
            Round& round = rounds.back();
            for (std::size_t j = 0; j < bits.size() && j < record[i].outcome.syndrome.size(); ++j) {
                if (bits[j]) round.syndrome[*bits[j]] = record[i].outcome.syndrome[j];
            }
            for (const bool f : record[i].outcome.flag) round.flagged = round.flagged || f;
            round.entries.push_back(i);
        }
        if (!readable) return no(std::string("an SE measures something other than the code's generators"));
        if (rounds.empty()) return no("no syndrome was measured");

        const auto full = [&](const Round& r) { return r.syndrome.size() == generators; };

        // w: the faults the record exposes. A flag is one; so is a change of
        // syndrome between consecutive unflagged full rounds, except that the
        // change right after another one may be the same fault seen twice.
        // The input is a codeword up to the input error, so the first unflagged
        // full round is compared with the trivial syndrome: a non-trivial one
        // exposes an input error or a fault, either of which uses the budget.
        Round baseline;
        for (std::size_t g = 0; g < generators; ++g) baseline.syndrome[g] = false;
        int  w = 0;
        bool skip = false;
        const Round* prev = &baseline;
        for (const auto& r : rounds) {
            if (r.flagged) { ++w; prev = nullptr; skip = false; continue; }
            if (!full(r))  { prev = nullptr; skip = false; continue; }
            if (prev) {
                if (prev->syndrome != r.syndrome) {
                    if (!skip) ++w;
                    skip = !skip;
                } else {
                    skip = false;
                }
            }
            prev = &r;
        }

        const int t = dag_.code.fault_budget();
        if (w > t) return no("the record exposes " + std::to_string(w) + " faults, more than t");
        const std::size_t need = static_cast<std::size_t>(t - w + 1);
        if (rounds.size() < need) {
            return no("w = " + std::to_string(w) + " needs " + std::to_string(need) +
                      " repeated rounds, the record has " + std::to_string(rounds.size()));
        }
        const Round& last = rounds.back();
        for (std::size_t i = rounds.size() - need; i < rounds.size(); ++i) {
            const Round& r = rounds[i];
            if (!full(r) || r.flagged || r.syndrome != last.syndrome) {
                return no("w = " + std::to_string(w) + ", but the last " + std::to_string(need) +
                          " rounds are not identical unflagged full rounds");
            }
        }
        Cond2 form;
        form.holds = true;
        form.k     = t - w;
        for (std::size_t i = rounds.size() - need; i < rounds.size(); ++i) {
            for (const std::size_t e : rounds[i].entries) form.suffix.push_back(e);
        }
        return form;
    }

    std::map<std::string, std::vector<std::optional<std::size_t>>> bits_;
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

    std::sort(result.failures.begin(), result.failures.end(),
              [](const PathFailure& a, const PathFailure& b) {
                  if (a.failure.fault_count != b.failure.fault_count) {
                      return a.failure.fault_count < b.failure.fault_count;
                  }
                  return a.path_id < b.path_id;
              });
    return result;
}

} // namespace ftec
