#include "ftec/verify.hpp"

#include <algorithm>
#include <cctype>
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

std::string PathFailure::record_string() const {
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

    std::vector<RecordEntry> record;
    Walker walker(dag, backend, options, result);
    walker.visit(dag.root(), backend.initial_state(), record);

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
