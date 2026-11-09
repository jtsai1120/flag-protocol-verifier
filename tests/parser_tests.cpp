#include "fpdl/parser.hpp"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

bool rejects(const std::string& source) {
    fpdl::ParseOptions options;
    options.inspect_qasm_registers = false;
    options.bmc_bound = 32;
    options.max_paths = 1000;
    try {
        (void)fpdl::Parser::parse_string(source, "invalid.fpdl", options);
    } catch (const fpdl::ParseError&) {
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "expected at least one .fpdl example\n";
        return 2;
    }

    for (int i = 1; i < argc; ++i) {
        try {
            fpdl::ParseOptions options;
            options.bmc_bound = 48;
            options.max_paths = 5000;
            const auto result = fpdl::Parser::parse_file(argv[i], options);
            if (result.protocol_name.empty() || result.paths.empty()) {
                std::cerr << argv[i] << ": parser did not produce symbolic paths\n";
                return 1;
            }
            const auto json = result.to_json();
            if (json.find("\"protocol\"") == std::string::npos ||
                json.find("\"paths\"") == std::string::npos ||
                json.find("\"events\"") == std::string::npos) {
                std::cerr << argv[i] << ": invalid symbolic-path JSON\n";
                return 1;
            }
            // The code: block feeds a verifier backend directly, so every
            // example must yield a usable one: sane [[n,k,d]], generators that
            // are n qubits wide however they were written (dense letters or
            // indexed Z1/X12 form), and a fault budget of floor((d-1)/2).
            const auto& code = result.code;
            if (code.n <= 0 || code.k <= 0 || code.d <= 0 || code.k >= code.n) {
                std::cerr << argv[i] << ": implausible code parameters [["
                          << code.n << ',' << code.k << ',' << code.d << "]]\n";
                return 1;
            }
            if (code.fault_budget() != (code.d - 1) / 2) {
                std::cerr << argv[i] << ": fault budget disagrees with the distance\n";
                return 1;
            }
            if (code.generators.empty()) {
                std::cerr << argv[i] << ": code block produced no generators\n";
                return 1;
            }
            for (const auto& generator : code.generators) {
                if (static_cast<int>(generator.size()) != code.n) {
                    std::cerr << argv[i] << ": generator '" << generator << "' is "
                              << generator.size() << " wide, expected " << code.n << '\n';
                    return 1;
                }
                if (generator.find_first_not_of("IXYZ") != std::string::npos) {
                    std::cerr << argv[i] << ": generator '" << generator
                              << "' has a non-Pauli character\n";
                    return 1;
                }
            }
            // Every SE must name the quantum registers a backend needs, and a
            // flagged SE must expose both the flag qubits and the flag bits.
            for (const auto& path : result.paths) {
                for (const auto& event : path.events) {
                    if (event.data_qubits.empty() || event.syndrome_qubits.empty()) {
                        std::cerr << argv[i] << ": SE '" << event.se_name
                                  << "' does not name its qd/qm registers\n";
                        return 1;
                    }
                    for (const auto& measured : event.measures) {
                        if (static_cast<int>(measured.size()) != code.n) {
                            std::cerr << argv[i] << ": SE '" << event.se_name
                                      << "' measures a generator of the wrong width\n";
                            return 1;
                        }
                    }
                }
            }

            // Every constraint must arrive in a form a backend can evaluate.
            // The failure this guards against is silent: an atom that was not
            // recognised as a comparison degrades into Equals(whole text, "true"),
            // which still renders correctly but cannot be resolved against a
            // measurement outcome.
            {
                const auto check_condition = [&](const fpdl::Condition& condition,
                                                 const auto& self) -> bool {
                    switch (condition.kind) {
                        case fpdl::Condition::Kind::Constant:
                            return true;
                        case fpdl::Condition::Kind::Equals:
                            if (condition.lhs.empty()) return false;
                            // The degenerate shape: an unparsed comparison
                            // wrapped as "<text> == true".
                            return !(condition.rhs == "true" &&
                                     condition.lhs.find("==") != std::string::npos);
                        case fpdl::Condition::Kind::And:
                        case fpdl::Condition::Kind::Or:
                            return condition.operands.size() == 2 &&
                                   self(condition.operands[0], self) &&
                                   self(condition.operands[1], self);
                        case fpdl::Condition::Kind::Not:
                            return condition.operands.size() == 1 &&
                                   self(condition.operands[0], self);
                    }
                    return false;
                };
                for (const auto& path : result.paths) {
                    for (const auto& constraint : path.constraints) {
                        if (!check_condition(constraint.condition, check_condition)) {
                            std::cerr << argv[i] << ": constraint '" << constraint.expression
                                      << "' did not survive into an evaluable condition\n";
                            return 1;
                        }
                    }
                }
            }

            if (std::string(argv[i]).find("CB18_[[17,1,5]]_plain") != std::string::npos) {
                for (const auto& path : result.paths) {
                    for (const auto& constraint : path.constraints) {
                        if (constraint.expression.find(".f[") != std::string::npos) {
                            std::cerr << argv[i]
                                      << ": record-only flag-index search became a path branch\n";
                            return 1;
                        }
                    }
                }
                if (result.truncated || result.paths.size() >= 100) {
                    std::cerr << argv[i]
                              << ": record-only control flow caused path explosion\n";
                    return 1;
                }
            }
        } catch (const std::exception& error) {
            std::cerr << argv[i] << ": " << error.what() << '\n';
            return 1;
        }
    }

    if (!rejects("protocol bad: code: [[1,1,1]] [[I]] se: gvar: "
                 "bit x = 0 tc: 1: true af: while(!tc) {} tp: 1: { end(); }")) {
        std::cerr << "invalid declaration without semicolon was accepted\n";
        return 1;
    }

    const std::string prefix =
        "protocol bad: code: [[1,1,1]] [[I]] se: "
        "g { file: \"x.qasm\" qd: q qm: q g: [[I]] } ";
    if (!rejects(prefix +
                 "gvar: bit s = 0; tc: 1: true "
                 "af: while(!tc) { (s) = g(); } tp: 1: { end(); }")) {
        std::cerr << "old singleton tuple syntax was accepted\n";
        return 1;
    }

    if (!rejects("protocol bad: code: [[1,1,1]] [[I]] se: gvar: bit x = 0; "
                 "tc: 1: local == 0 af: while(!tc) {} tp: 1: { end(); }")) {
        std::cerr << "terminal condition referencing a nonglobal was accepted\n";
        return 1;
    }

    if (!rejects("protocol bad: code: [[1,1,1]] [[I]] se: gvar: bit x = 0; "
                 "tc: 1: true af: while(!tc) {} tp: 1: { check(); }")) {
        std::cerr << "check() in terminating policy was accepted\n";
        return 1;
    }

    fpdl::ParseOptions symbolic_options;
    symbolic_options.inspect_qasm_registers = false;
    symbolic_options.bmc_bound = 40;
    const auto symbolic = fpdl::Parser::parse_string(
        "protocol branching: code: [[1,1,1]] [[I]] se: "
        "first { file: \"first.qasm\" qd: q qm: q qf: qf g: [[I]] #-flag: 1 } "
        "left { file: \"left.qasm\" qd: q qm: q g: [[I]] } "
        "right { file: \"right.qasm\" qd: q qm: q g: [[I]] } "
        "gvar: cnt n = 0; bit s = 0; bit f = 0; bit x = 0; "
        "tc: 1: n == 2 "
        "af: while(!tc) { "
        "  if (n == 0) { (s, f) = first(); } "
        "  else if (s == 0) { (x,) = left(); } "
        "  else { (x,) = right(); } "
        "  n++; "
        "} "
        "tp: 1: { end(); }",
        "branching.fpdl", symbolic_options);
    std::set<std::string> second_calls;
    for (const auto& path : symbolic.paths) {
        if (path.events.size() >= 2) second_calls.insert(path.events[1].se_name);
    }
    if (!second_calls.contains("left") || !second_calls.contains("right")) {
        std::cerr << "symbolic SE result did not select both downstream SE paths\n";
        return 1;
    }
    for (const auto& path : symbolic.paths) {
        for (std::size_t i = 0; i < path.events.size(); ++i) {
            const auto& event = path.events[i];
            const std::string expected = event.se_name + ".qasm";
            if (event.qasm_file != expected) {
                std::cerr << "SE event did not retain its declared QASM filename\n";
                return 1;
            }
            const std::string symbolic_id = "id_" + std::to_string(i + 1);
            if (event.syndrome != symbolic_id + ".s" ||
                (event.flag && *event.flag != symbolic_id + ".f")) {
                std::cerr << "SE result did not use sequential id_N symbolic names\n";
                return 1;
            }
            // Only the quantum registers are declared now; a classical one is
            // read off the circuit's measure statements instead.
            if (event.data_qubits != "q" || event.syndrome_qubits != "q" ||
                (event.se_name == "first" && event.flag_qubits != "qf") ||
                (event.se_name != "first" && event.flag_qubits)) {
                std::cerr << "SE event did not retain its QASM quantum registers\n";
                return 1;
            }
        }
    }
    const auto symbolic_json = symbolic.to_json();
    if (symbolic_json.find("\"qasm_sequence\": [\"first.qasm\"") ==
            std::string::npos ||
        symbolic_json.find("\"qasm_file\": \"left.qasm\"") ==
            std::string::npos ||
        symbolic_json.find("\"qasm_file\": \"right.qasm\"") ==
            std::string::npos) {
        std::cerr << "symbolic-path JSON did not contain ordered QASM output\n";
        return 1;
    }

    const auto continue_control = fpdl::Parser::parse_string(
        "protocol continue_control: code: [[1,1,1]] [[I]] se: "
        "flagged { file: \"flagged.qasm\" qd: q qm: q qf: qf g: [[I]] #-flag: 1 } "
        "plain { file: \"plain.qasm\" qd: q qm: q g: [[I]] } "
        "gvar: cnt n = 0; bit s = ⊥; bit f = ⊥; bit other = ⊥; "
        "tc: 1: n == 2 "
        "af: while(!tc) { "
        "  (s, f) = flagged(); "
        "  n++; "
        "  if (f != 0) { continue; } "
        "  (other,) = plain(); "
        "} "
        "tp: 1: { end(); }",
        "continue-control.fpdl", symbolic_options);
    bool saw_continue_skip = false;
    bool saw_continue_fallthrough = false;
    for (const auto& path : continue_control.paths) {
        if (path.events.size() < 2) continue;
        if (path.events[0].se_name == "flagged" &&
            path.events[1].se_name == "flagged") {
            saw_continue_skip = true;
        }
        if (path.events[0].se_name == "flagged" &&
            path.events[1].se_name == "plain") {
            saw_continue_fallthrough = true;
        }
    }
    if (!saw_continue_skip || !saw_continue_fallthrough) {
        std::cerr << "continue did not control whether later path-relevant work executes\n";
        return 1;
    }

    const auto compound = fpdl::Parser::parse_string(
        "protocol compound: code: [[1,1,1]] [[I]] se: "
        "flagged { file: \"flagged.qasm\" qd: q qm: q qf: qf g: [[I]] #-flag: 1 } "
        "plain { file: \"plain.qasm\" qd: q qm: q g: [[I]] } "
        "gvar: bit s = ⊥; bit f = ⊥; bit other = ⊥; "
        "tc: 1: (s == 0) and (f == 0) "
        "af: while(!tc) { "
        "  if (s == ⊥) { (s, f) = flagged(); } "
        "  else { (other,) = plain(); } "
        "} "
        "tp: 1: { end(); }",
        "compound.fpdl", symbolic_options);
    if (compound.paths.size() != 2) {
        std::cerr << "one compound false outcome was expanded into multiple control-flow paths\n";
        return 1;
    }
    for (const auto& path : compound.paths) {
        if (path.terminal_condition == 1 && path.rounds > 1) {
            std::cerr << "infeasible !(A and B) then (A and B) path was retained\n";
            return 1;
        }
    }

    return 0;
}
