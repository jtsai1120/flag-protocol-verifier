#include "ftec/qasm.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace ftec {

namespace {

// ---------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------

struct Statement {
    std::string text;
    std::size_t line = 1;
};

[[noreturn]] void fail(const std::string& where, std::size_t line, const std::string& message) {
    std::ostringstream out;
    out << where << ':' << line << ": " << message;
    throw QasmError(out.str());
}

std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(begin, end - begin + 1));
}

// Split on ';' and on the braces of a gate body, keeping enough position
// information to point at the right line when something is wrong. Comments go
// here so nothing downstream has to think about them.
std::vector<Statement> split_statements(const std::string& source, const std::string& where) {
    std::vector<Statement> out;
    std::string            current;
    std::size_t            line = 1;
    std::size_t            start = 1;
    bool                   started = false;

    const auto flush = [&](char terminator) {
        std::string text = trim(current);
        if (!text.empty()) out.push_back({text, start});
        if (terminator == '{' || terminator == '}') out.push_back({std::string(1, terminator), line});
        current.clear();
        started = false;
    };

    for (std::size_t i = 0; i < source.size();) {
        const char c = source[i];

        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            const std::size_t open = line;
            i += 2;
            while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) {
                if (source[i] == '\n') ++line;
                ++i;
            }
            if (i + 1 >= source.size()) fail(where, open, "unterminated /* comment");
            i += 2;
            continue;
        }

        if (c == '\n') ++line;
        if (c == ';' || c == '{' || c == '}') {
            flush(c);
            ++i;
            continue;
        }
        if (!started && !std::isspace(static_cast<unsigned char>(c))) {
            started = true;
            start   = line;
        }
        current += c;
        ++i;
    }
    if (!trim(current).empty()) fail(where, start, "statement is not terminated by ';'");
    return out;
}

// ---------------------------------------------------------------------------
// Small lexical helpers
// ---------------------------------------------------------------------------

bool is_identifier(std::string_view text) {
    if (text.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(text.front())) && text.front() != '_') return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> out;
    std::string              current;
    for (const char c : text) {
        if (c == ',') {
            out.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }
    const std::string last = trim(current);
    if (!last.empty() || !out.empty()) out.push_back(last);
    return out;
}

// "q", "q[3]" -> name and optional index.
struct Operand {
    std::string                name;
    std::optional<std::size_t> index;
};

std::optional<Operand> parse_operand(const std::string& text) {
    const auto bracket = text.find('[');
    if (bracket == std::string::npos) {
        if (!is_identifier(text)) return std::nullopt;
        return Operand{text, std::nullopt};
    }
    if (text.back() != ']') return std::nullopt;
    const std::string name   = trim(text.substr(0, bracket));
    const std::string digits = trim(text.substr(bracket + 1, text.size() - bracket - 2));
    if (!is_identifier(name) || digits.empty() ||
        digits.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }
    return Operand{name, std::stoull(digits)};
}

bool is_primitive(std::string_view gate) {
    return std::find(std::begin(kPrimitiveGates), std::end(kPrimitiveGates), gate) !=
           std::end(kPrimitiveGates);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

struct GateDef {
    std::vector<std::string> parameters;
    std::vector<Statement>   body;
    std::size_t              line = 0;
};

class Parser {
public:
    Parser(const std::string& source, std::string name)
        : where_(std::move(name)), statements_(split_statements(source, where_)) {}

    QasmProgram run() {
        program_.name = where_;
        while (position_ < statements_.size()) {
            const Statement statement = statements_[position_++];
            if (statement.text == "{" || statement.text == "}") {
                fail(where_, statement.line, "unexpected '" + statement.text + "'");
            }
            // A missing version header usually means the wrong file was
            // handed over, which is worth catching before anything is read
            // into a circuit.
            if (!seen_version_ && statement.text.rfind("OPENQASM", 0) != 0) {
                fail(where_, statement.line,
                     "expected an 'OPENQASM 3;' header before any other statement, found '" +
                         statement.text + "'");
            }
            handle(statement);
        }
        if (!seen_version_) throw QasmError(where_ + ": file has no statements");
        return std::move(program_);
    }

private:
    void handle(const Statement& statement) {
        const std::string& text = statement.text;

        if (text.rfind("OPENQASM", 0) == 0) {
            const std::string version = trim(text.substr(8));
            if (version.rfind("3", 0) != 0) {
                fail(where_, statement.line, "only OpenQASM 3 is supported, found '" + version + "'");
            }
            if (seen_version_) fail(where_, statement.line, "duplicate OPENQASM header");
            seen_version_ = true;
            return;
        }
        if (text.rfind("include", 0) == 0) return;    // stdgates only defines what we already know
        if (text.rfind("barrier", 0) == 0) {
            program_.instructions.push_back(
                {QasmInstruction::Kind::Barrier, {}, {}, {}, statement.line});
            return;
        }
        if (text.rfind("gate ", 0) == 0) {
            define_gate(statement);
            return;
        }
        if (declare(statement, "qubit", program_.qubits)) return;
        if (declare(statement, "bit", program_.bits)) return;
        if (handle_measure(statement)) return;
        if (text.rfind("reset ", 0) == 0) {
            for (const auto& qubit : expand_operand(trim(text.substr(6)), statement, true)) {
                program_.instructions.push_back(
                    {QasmInstruction::Kind::Reset, {}, {qubit}, {}, statement.line});
            }
            return;
        }
        emit_call(statement, {});
    }

    // `qubit[3] q` / `qubit q` / `bit[4] m` / `bit f`
    bool declare(const Statement& statement, const std::string& keyword,
                 std::vector<RegisterDecl>& into) {
        const std::string& text = statement.text;
        if (text.rfind(keyword, 0) != 0) return false;
        const char after = text.size() > keyword.size() ? text[keyword.size()] : '\0';
        if (after != '[' && after != ' ' && after != '\t') return false;

        std::string rest  = trim(text.substr(keyword.size()));
        std::size_t width = 1;
        if (!rest.empty() && rest.front() == '[') {
            const auto close = rest.find(']');
            if (close == std::string::npos) {
                fail(where_, statement.line, "malformed " + keyword + " declaration");
            }
            const std::string digits = trim(rest.substr(1, close - 1));
            if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
                fail(where_, statement.line, keyword + " width must be a number");
            }
            width = std::stoull(digits);
            rest  = trim(rest.substr(close + 1));
        }
        if (!is_identifier(rest)) {
            fail(where_, statement.line, "expected a name in the " + keyword + " declaration");
        }
        if (width == 0) fail(where_, statement.line, keyword + " register '" + rest + "' is empty");
        for (const auto& existing : into) {
            if (existing.name == rest) {
                fail(where_, statement.line, "duplicate " + keyword + " register '" + rest + "'");
            }
        }
        into.push_back({rest, width});
        return true;
    }

    // `m = measure q;` / `m[0] = measure q[2];`
    bool handle_measure(const Statement& statement) {
        const auto equals = statement.text.find('=');
        if (equals == std::string::npos) return false;
        const std::string rhs = trim(statement.text.substr(equals + 1));
        if (rhs.rfind("measure", 0) != 0) return false;

        const std::string lhs_text = trim(statement.text.substr(0, equals));
        const auto        lhs      = parse_operand(lhs_text);
        if (!lhs) fail(where_, statement.line, "malformed measurement target '" + lhs_text + "'");

        const std::string source_text = trim(rhs.substr(7));
        const auto        qubits      = expand_operand(source_text, statement, true);

        std::vector<BitRef> targets;
        if (lhs->index) {
            targets.push_back({lhs->name, *lhs->index});
        } else {
            for (std::size_t i = 0; i < bit_width(*lhs, statement); ++i) {
                targets.push_back({lhs->name, i});
            }
        }
        if (targets.size() != qubits.size()) {
            fail(where_, statement.line,
                 "measurement writes " + std::to_string(qubits.size()) + " qubit(s) into " +
                     std::to_string(targets.size()) + " bit(s)");
        }
        check_bit(targets, statement);
        for (std::size_t i = 0; i < qubits.size(); ++i) {
            program_.instructions.push_back(
                {QasmInstruction::Kind::Measure, {}, {qubits[i]}, targets[i], statement.line});
        }
        return true;
    }

    void define_gate(const Statement& statement) {
        const std::string header = trim(statement.text.substr(5));
        const auto        space  = header.find_first_of(" \t");
        if (space == std::string::npos) {
            fail(where_, statement.line, "gate definition needs a name and parameters");
        }
        GateDef definition;
        definition.line = statement.line;
        const std::string name = trim(header.substr(0, space));
        if (!is_identifier(name)) fail(where_, statement.line, "invalid gate name '" + name + "'");
        if (is_primitive(name)) {
            fail(where_, statement.line, "'" + name + "' is a primitive and cannot be redefined");
        }
        for (const auto& parameter : split_commas(header.substr(space))) {
            if (!is_identifier(parameter)) {
                fail(where_, statement.line, "invalid gate parameter '" + parameter + "'");
            }
            definition.parameters.push_back(parameter);
        }

        if (position_ >= statements_.size() || statements_[position_].text != "{") {
            fail(where_, statement.line, "gate definition has no body");
        }
        ++position_;
        int depth = 1;
        while (position_ < statements_.size()) {
            const Statement& inner = statements_[position_];
            if (inner.text == "{") ++depth;
            if (inner.text == "}") {
                if (--depth == 0) { ++position_; break; }
            }
            definition.body.push_back(inner);
            ++position_;
        }
        if (depth != 0) fail(where_, statement.line, "gate definition is not closed");

        if (!gates_.emplace(name, std::move(definition)).second) {
            fail(where_, statement.line, "duplicate gate definition '" + name + "'");
        }
    }

    // A gate application, primitive or user-defined. `binding` maps a custom
    // gate's parameters to the qubits it was called with, so a body statement
    // resolves its operands through the caller's frame.
    void emit_call(const Statement& statement, const std::map<std::string, QubitRef>& binding,
                   std::size_t depth = 0) {
        if (depth > 64) {
            fail(where_, statement.line, "gate expansion is too deep; is a definition recursive?");
        }
        const std::string& text  = statement.text;
        const auto         space = text.find_first_of(" \t");
        const std::string  name  = trim(space == std::string::npos ? text : text.substr(0, space));
        if (name.empty()) return;
        if (!is_identifier(name)) {
            fail(where_, statement.line, "unsupported statement '" + text + "'");
        }
        const std::string arguments = space == std::string::npos ? "" : text.substr(space);

        std::vector<QubitRef> operands;
        for (const auto& argument : split_commas(arguments)) {
            if (argument.empty()) continue;
            if (const auto found = binding.find(argument); found != binding.end()) {
                operands.push_back(found->second);
                continue;
            }
            const auto expanded = expand_operand(argument, statement, false);
            if (expanded.size() != 1) {
                fail(where_, statement.line,
                     "'" + argument + "' names a whole register; gates take single qubits");
            }
            operands.push_back(expanded.front());
        }

        if (is_primitive(name)) {
            const std::size_t arity = (name == "cx" || name == "cy" || name == "cz") ? 2 : 1;
            if (operands.size() != arity) {
                fail(where_, statement.line,
                     name + " takes " + std::to_string(arity) + " qubit(s), got " +
                         std::to_string(operands.size()));
            }
            if (arity == 2 && operands[0].reg == operands[1].reg &&
                operands[0].index == operands[1].index) {
                fail(where_, statement.line, name + " needs two distinct qubits");
            }
            program_.instructions.push_back(
                {QasmInstruction::Kind::Gate, name, operands, {}, statement.line});
            return;
        }

        const auto definition = gates_.find(name);
        if (definition == gates_.end()) {
            fail(where_, statement.line, "unknown gate '" + name + "'");
        }
        if (operands.size() != definition->second.parameters.size()) {
            fail(where_, statement.line,
                 name + " takes " + std::to_string(definition->second.parameters.size()) +
                     " qubit(s), got " + std::to_string(operands.size()));
        }

        std::map<std::string, QubitRef> inner;
        for (std::size_t i = 0; i < operands.size(); ++i) {
            inner.emplace(definition->second.parameters[i], operands[i]);
        }
        for (const auto& body : definition->second.body) {
            if (body.text == "{" || body.text == "}") {
                fail(where_, body.line, "a gate body may only contain gate applications");
            }
            if (body.text.rfind("measure", 0) == 0 || body.text.find("= measure") != std::string::npos ||
                body.text.rfind("reset", 0) == 0 || body.text.rfind("gate ", 0) == 0) {
                fail(where_, body.line, "a gate body may only contain gate applications");
            }
            emit_call(body, inner, depth + 1);
        }
    }

    // A bare register name expands to all of its qubits when `whole_allowed`;
    // gate operands must name one.
    std::vector<QubitRef> expand_operand(const std::string& text, const Statement& statement,
                                         bool whole_allowed) {
        const auto operand = parse_operand(text);
        if (!operand) fail(where_, statement.line, "malformed qubit reference '" + text + "'");

        const auto declaration =
            std::find_if(program_.qubits.begin(), program_.qubits.end(),
                         [&](const RegisterDecl& r) { return r.name == operand->name; });
        if (declaration == program_.qubits.end()) {
            fail(where_, statement.line, "unknown qubit register '" + operand->name + "'");
        }
        if (operand->index) {
            if (*operand->index >= declaration->width) {
                fail(where_, statement.line,
                     "qubit index " + std::to_string(*operand->index) + " is outside " +
                         operand->name + "[" + std::to_string(declaration->width) + "]");
            }
            return {QubitRef{operand->name, *operand->index}};
        }
        if (!whole_allowed && declaration->width != 1) {
            fail(where_, statement.line,
                 "'" + operand->name + "' names a whole register; gates take single qubits");
        }
        std::vector<QubitRef> out;
        for (std::size_t i = 0; i < declaration->width; ++i) out.push_back({operand->name, i});
        return out;
    }

    std::size_t bit_width(const Operand& operand, const Statement& statement) {
        const auto declaration =
            std::find_if(program_.bits.begin(), program_.bits.end(),
                         [&](const RegisterDecl& r) { return r.name == operand.name; });
        if (declaration == program_.bits.end()) {
            fail(where_, statement.line, "unknown bit register '" + operand.name + "'");
        }
        return declaration->width;
    }

    void check_bit(const std::vector<BitRef>& targets, const Statement& statement) {
        for (const auto& target : targets) {
            const auto declaration =
                std::find_if(program_.bits.begin(), program_.bits.end(),
                             [&](const RegisterDecl& r) { return r.name == target.reg; });
            if (declaration == program_.bits.end()) {
                fail(where_, statement.line, "unknown bit register '" + target.reg + "'");
            }
            if (target.index >= declaration->width) {
                fail(where_, statement.line,
                     "bit index " + std::to_string(target.index) + " is outside " + target.reg +
                         "[" + std::to_string(declaration->width) + "]");
            }
        }
    }

    std::string             where_;
    std::vector<Statement>  statements_;
    std::size_t             position_ = 0;
    bool                    seen_version_ = false;
    std::map<std::string, GateDef> gates_;
    QasmProgram             program_;
};

} // namespace

bool QasmProgram::has_qubit_register(std::string_view name) const {
    return std::any_of(qubits.begin(), qubits.end(),
                       [&](const RegisterDecl& r) { return r.name == name; });
}

bool QasmProgram::has_bit_register(std::string_view name) const {
    return std::any_of(bits.begin(), bits.end(),
                       [&](const RegisterDecl& r) { return r.name == name; });
}

std::size_t QasmProgram::qubit_width(std::string_view name) const {
    for (const auto& r : qubits) {
        if (r.name == name) return r.width;
    }
    throw QasmError("no qubit register named '" + std::string(name) + "' in " + this->name);
}

std::size_t QasmProgram::bit_width(std::string_view name) const {
    for (const auto& r : bits) {
        if (r.name == name) return r.width;
    }
    throw QasmError("no bit register named '" + std::string(name) + "' in " + this->name);
}

std::size_t QasmProgram::total_qubits() const {
    std::size_t total = 0;
    for (const auto& r : qubits) total += r.width;
    return total;
}

QasmProgram parse_qasm(std::string_view source, const std::string& name) {
    Parser parser(std::string(source), name);
    return parser.run();
}

QasmProgram parse_qasm_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw QasmError("cannot open QASM file: " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_qasm(buffer.str(), path.string());
}

} // namespace ftec
