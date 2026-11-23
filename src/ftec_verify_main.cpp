#include "ftec/dag.hpp"
#include "ftec/circuit_check.hpp"
#include "ftec/spbdd_backend.hpp"
#include "ftec/verify.hpp"

#include <sys/resource.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

// Peak resident set of this process. getrusage reports ru_maxrss in bytes on
// macOS and in kilobytes everywhere else, which is a difference that silently
// makes the number wrong by 1024 if it is not handled.
std::size_t peak_memory_bytes() {
#if defined(__EMSCRIPTEN__)
    // In the browser: the WebAssembly heap only ever grows, so its size is
    // the peak.
    return emscripten_get_heap_size();
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

std::string human_bytes(std::size_t bytes) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    if (bytes >= (1u << 30)) out << static_cast<double>(bytes) / (1u << 30) << " GiB";
    else if (bytes >= (1u << 20)) out << static_cast<double>(bytes) / (1u << 20) << " MiB";
    else out << static_cast<double>(bytes) / (1u << 10) << " KiB";
    return out.str();
}

std::string human_seconds(std::chrono::steady_clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::ostringstream out;
    out << std::fixed << std::setprecision(seconds < 1 ? 4 : seconds < 10 ? 2 : 1) << seconds << " s";
    return out.str();
}

void usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " <protocol.fpdl> [options]\n"
        << "  --fqcv=METHOD    how the final E1E2 query (two errors with the same\n"
        << "                   syndrome whose product is a logical operator) is\n"
        << "                   answered: bruteforce, table, image (default) or square\n"
        << "  --bound=N        fix the BMC bound instead of searching for one\n"
        << "  --max-paths=N    give up past this many symbolic paths (default 5000)\n"
        << "  --first          stop at the first unprotected path\n"
        << "  --tree[=N]       print the decision tree as JSON and exit; past N nodes\n"
        << "                   (default 2000) print only its size\n"
        << "  --no-input-errors\n"
        << "                   start every run from an error-free input (r = 0)\n"
        << "  --no-validity    skip the protocol validity check (Def. 3.9)\n"
        << "  --by-path        list, for every path, how many of its records satisfy\n"
        << "                   Conditions 1, 2 and 3 (Def. 3.9)\n"
        << "  --decode-needs-cond12\n"
        << "                   do not let Condition 3 settle a decode() terminal (as the\n"
        << "                   paper's experiments do); by default Def. 3.9 lets any\n"
        << "                   terminating path use any condition\n"
        << "  --skip-circuit-check\n"
        << "                   verify the circuits as written even if they are not\n"
        << "                   valid (correct and deterministic)\n";
}

// Symbolic expansion needs a finite transition bound, and a protocol that has
// not finished expanding is silently incomplete. Rather than make the caller
// guess, raise the bound until the parser stops reporting truncation.
struct Expansion {
    fpdl::ParseResult result;
    std::size_t       bound = 0;
};

Expansion expand(const std::filesystem::path& source, std::optional<std::size_t> fixed,
                 std::size_t max_paths) {
    fpdl::ParseOptions options;
    options.max_paths = max_paths;

    if (fixed) {
        options.bmc_bound = *fixed;
        return {fpdl::Parser::parse_file(source, options), *fixed};
    }

    // Doubling keeps the number of attempts logarithmic; the cap is a safety
    // net for a protocol that genuinely never terminates.
    static constexpr std::size_t kCeiling = 1u << 16;
    for (std::size_t bound = 16; bound <= kCeiling; bound *= 2) {
        options.bmc_bound = bound;
        auto parsed = fpdl::Parser::parse_file(source, options);
        const bool bounded_out = parsed.truncated;
        bool any_path_bounded = false;
        for (const auto& path : parsed.paths) any_path_bounded |= path.bound_exceeded;
        if (!bounded_out && !any_path_bounded) return {std::move(parsed), bound};
    }
    throw std::runtime_error(
        "symbolic expansion did not converge below a bound of " + std::to_string(kCeiling) +
        "; pass --bound=N to choose one explicitly");
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

// The decision tree (the merged symbolic paths) as one JSON object, for the
// playground to draw. Past `limit` nodes only the counts are written: a tree
// that large cannot be read as a picture anyway.
void print_tree_json(const ftec::Dag& dag, std::size_t limit) {
    std::ostringstream out;
    out << "{\"protocol\":" << json_string(dag.protocol) << ",\"code\":\"[[" << dag.code.n << ','
        << dag.code.k << ',' << dag.code.d << "]]\",\"tau\":" << dag.code.fault_budget()
        << ",\"paths\":" << dag.path_count << ",\"node_count\":" << dag.nodes.size()
        << ",\"limit\":" << limit;
    if (dag.nodes.size() > limit) {
        out << ",\"too_large\":true}";
        std::cout << out.str() << "\n";
        return;
    }

    // An SE node's outcomes are named after its position on the path: the
    // k-th SE of a path writes id_k.s (and id_k.f when flagged).
    std::vector<std::size_t> depth(dag.nodes.size(), 0);
    for (const auto& edge : dag.edges) depth[edge.to] = depth[edge.from] + 1;

    out << ",\"nodes\":[";
    for (std::size_t i = 0; i < dag.nodes.size(); ++i) {
        const auto& node = dag.nodes[i];
        if (i) out << ',';
        out << "{\"id\":" << node.id;
        switch (node.kind) {
        case ftec::DagNode::Kind::Start:
            out << ",\"kind\":\"start\"";
            break;
        case ftec::DagNode::Kind::Se:
            out << ",\"kind\":\"se\",\"name\":" << json_string(node.circuit.se_name)
                << ",\"phase\":" << json_string(node.phase) << ",\"round\":" << node.round
                << ",\"invocation\":" << node.invocation << ",\"step\":" << depth[i]
                << ",\"flagged\":" << (node.circuit.flag_qubits ? "true" : "false")
                << ",\"g\":[";
            for (std::size_t g = 0; g < node.circuit.measures.size(); ++g) {
                if (g) out << ',';
                const bool negated = g < node.circuit.negated.size() && node.circuit.negated[g];
                out << json_string((negated ? "-" : "") + node.circuit.measures[g]);
            }
            out << ']';
            break;
        case ftec::DagNode::Kind::Terminal:
            out << ",\"kind\":\"terminal\",\"path\":" << node.path_id
                << ",\"action\":" << json_string(node.terminal_action);
            if (node.terminal_condition) out << ",\"condition\":" << *node.terminal_condition;
            if (node.decode_record) out << ",\"record\":" << json_string(*node.decode_record);
            break;
        }
        out << '}';
    }
    out << "],\"edges\":[";
    for (std::size_t i = 0; i < dag.edges.size(); ++i) {
        const auto& edge = dag.edges[i];
        if (i) out << ',';
        out << "{\"from\":" << edge.from << ",\"to\":" << edge.to << ",\"guards\":[";
        for (std::size_t g = 0; g < edge.guards.size(); ++g) {
            if (g) out << ',';
            out << "{\"expr\":" << json_string(edge.guards[g].expression)
                << ",\"expected\":" << (edge.guards[g].expected ? "true" : "false") << '}';
        }
        out << "]}";
    }
    out << "]}";
    std::cout << out.str() << "\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    std::filesystem::path      source;
    ftec::SpbddOptions         spbdd_options;
    std::optional<std::size_t> bound;
    std::size_t                max_paths = 5000;
    std::optional<std::size_t> tree_limit;
    bool                       skip_circuit_check = false;
    bool                       by_path = false;
    ftec::VerifyOptions        options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--fqcv=", 0) == 0) {
            spbdd_options.fqcv = arg.substr(7);
            if (!ftec::is_spbdd_fqcv_method(spbdd_options.fqcv)) {
                std::cerr << "error: unknown fqcv method '" << spbdd_options.fqcv << "'\n";
                usage(argv[0]);
                return 2;
            }
        } else if (arg.rfind("--bound=", 0) == 0) {
            bound = std::stoull(arg.substr(8));
        } else if (arg.rfind("--max-paths=", 0) == 0) {
            max_paths = std::stoull(arg.substr(12));
        } else if (arg == "--first") {
            options.stop_at_first_failure = true;
        } else if (arg == "--skip-circuit-check" || arg == "--skip-se-check") {
            skip_circuit_check = true;
        } else if (arg == "--no-input-errors") {
            spbdd_options.input_errors = false;
        } else if (arg == "--no-validity") {
            options.check_validity = false;
        } else if (arg == "--by-path") {
            by_path = true;
        } else if (arg == "--decode-needs-cond12") {
            options.decode_may_use_cond3 = false;
        } else if (arg == "--tree") {
            tree_limit = 2000;
        } else if (arg.rfind("--tree=", 0) == 0) {
            tree_limit = std::stoull(arg.substr(7));
        } else if (arg.rfind("--", 0) == 0) {
            std::cerr << "error: unknown option " << arg << "\n";
            usage(argv[0]);
            return 2;
        } else if (source.empty()) {
            source = arg;
        } else {
            std::cerr << "error: more than one protocol given\n";
            return 2;
        }
    }
    if (source.empty()) {
        usage(argv[0]);
        return 2;
    }

    const auto started = std::chrono::steady_clock::now();
    ftec::BackendPtr backend;
    try {
        const auto expansion = expand(source, bound, max_paths);
        const auto dag = ftec::build_dag(expansion.result, source.parent_path());
        const auto expanded_at = std::chrono::steady_clock::now();

        // Printed at the end of every route out of here, including the early
        // ones, so a run that was abandoned still says what it cost.
        const auto report_cost = [&](std::chrono::steady_clock::duration traversal) {
            if (backend) {
                const std::string extra = backend->statistics();
                if (!extra.empty()) std::cout << "\n" << extra << "\n";
            }
            std::cout << "\nexpansion       : " << human_seconds(expanded_at - started) << "\n"
                      << "traversal       : " << human_seconds(traversal) << "\n"
                      << "total runtime   : "
                      << human_seconds(std::chrono::steady_clock::now() - started) << "\n"
                      << "peak memory     : "
                      << human_bytes(std::max(peak_memory_bytes(),
                                              backend ? backend->peak_memory_bytes() : 0))
                      << "\n";
        };

        if (tree_limit) {
            print_tree_json(dag, *tree_limit);
            return 0;
        }

        // The traversal verifies the circuits as written; this makes sure
        // they are the circuits the protocol says they are.
        std::chrono::steady_clock::duration circuit_check_time{};
        if (!skip_circuit_check) {
            const auto check_started = std::chrono::steady_clock::now();
            const auto report = ftec::check_circuits(dag);
            circuit_check_time = std::chrono::steady_clock::now() - check_started;
            // One line for tools: the cost of circuit validity on its own.
            std::cout << "circuit check   : "
                      << (report.errors.empty() ? std::string("valid")
                                                : std::to_string(report.errors.size()) + " problem(s)")
                      << ", " << report.warnings.size() << " warning(s), " << std::fixed
                      << std::setprecision(4)
                      << std::chrono::duration<double>(circuit_check_time).count() << " s, peak "
                      << human_bytes(peak_memory_bytes()) << "\n";
            std::cout.unsetf(std::ios::floatfield);
            for (const auto& warning : report.warnings) std::cerr << "warning: " << warning << "\n";
            const auto& issues = report.errors;
            if (!issues.empty()) {
                std::cerr << "error: " << issues.size()
                          << " circuit validity problem(s); the protocol is not valid:\n";
                for (const auto& issue : issues) std::cerr << "  " << issue << "\n";
                std::cerr << "(--skip-circuit-check verifies the circuits as written anyway)\n";
                return 2;
            }
        }

        backend = ftec::make_spbdd_backend(spbdd_options);

        const auto traversal_started = std::chrono::steady_clock::now();
        const auto result = ftec::verify(dag, *backend, options);
        const auto traversal = std::chrono::steady_clock::now() - traversal_started;

        std::cout << "protocol        : " << result.protocol << "\n"
                  << "code            : [[" << dag.code.n << ',' << dag.code.k << ','
                  << dag.code.d << "]], tau = " << result.tau << "\n"
                  << "fqcv method     : " << spbdd_options.fqcv << "\n"
                  << "input errors    : " << (spbdd_options.input_errors ? "r + s <= tau" : "none (r = 0)")
                  << "\n"
                  << "paths reached   : " << result.paths_reached << " of " << dag.path_count
                  << "\n"
                  << "records reached : " << result.records_reached << "\n"
                  << "circuits run    : " << result.se_applications << "\n\n";

        std::cout << "correctability\n";
        if (result.clean()) {
            std::cout << "no unprotected path found\n";
        } else {
            std::set<std::size_t> bad_paths;
            for (const auto& failure : result.failures) bad_paths.insert(failure.path_id);
            std::cout << bad_paths.size() << " unprotected path(s), " << result.failures.size()
                      << " record(s); first failure at t = " << result.min_fault_count << "\n\n";
            for (const auto& failure : result.failures) {
                std::cout << "  path " << failure.path_id << "  t=" << failure.failure.fault_count
                          << "  record=" << failure.record_string() << "\n";
                if (!failure.failure.detail.empty()) {
                    std::cout << "        " << failure.failure.detail << "\n";
                }
            }
        }

        if (result.validity_checked) {
            std::cout << "\nprotocol validity\n"
                      << "terminal records: Cond 1 " << result.by_condition[0] << ", Cond 2 "
                      << result.by_condition[1] << ", Cond 3 " << result.by_condition[2] << "\n";
            if (result.control_flow_valid()) {
                std::cout << "control-flow bugs: 0 path(s)\n";
            } else {
                std::set<std::size_t> paths;
                for (const auto& v : result.validity_failures) paths.insert(v.path_id);
                std::cout << "control-flow bugs: " << paths.size() << " path(s), "
                          << result.invalid_records << " terminal record(s)\n";
                std::set<std::size_t> shown;
                for (const auto& v : result.validity_failures) {
                    if (!shown.insert(v.path_id).second) continue;   // one per path
                    std::cout << "  path " << v.path_id << "  " << v.terminal_action
                              << "  record=" << v.record_string() << "\n        " << v.reason
                              << "\n";
                }
            }
            if (by_path) {
                std::cout << "by path (records satisfying each condition; a record may satisfy several):\n";
                for (const auto& p : result.by_path) {
                    ftec::PathFailure f{p.path_id, p.terminal_action, p.example, {}};
                    std::cout << "  path " << p.path_id << "  " << p.terminal_action << "  records " << p.records
                              << "  Cond1 " << p.cond1 << "  Cond2 " << p.cond2 << "  Cond3 " << p.cond3
                              << "  none " << p.failing << "  e.g. " << f.record_string() << "\n";
                }
            }
            std::cout << "control-flow bugs if decode() may not use Cond 3: "
                      << result.invalid_paths_strict << " path(s)\n";
            if (result.flags_valid()) {
                std::cout << "flag bugs       : 0 circuit(s)\n";
            } else {
                std::set<std::string> circuits;
                std::set<std::size_t> paths;
                for (const auto& f : result.flag_failures) {
                    circuits.insert(f.se_name);
                    paths.insert(f.paths.begin(), f.paths.end());
                }
                std::cout << "flag bugs       : " << circuits.size() << " circuit(s), needed on "
                          << paths.size() << " path(s)\n";
                for (const auto& f : result.flag_failures) {
                    std::cout << "  " << f.se_name << " is not " << f.k << "-flag: " << f.problem
                              << "\n        needed by " << f.records << " record(s) on "
                              << f.paths.size() << " path(s), e.g. path " << f.path_id
                              << "  record=" << f.record_string() << "\n";
                }
            }
            std::cout << "validity time   : " << std::fixed << std::setprecision(3)
                      << std::setprecision(4) << result.validity_seconds << " s at the terminals";
            if (!skip_circuit_check) {
                std::cout << ", plus "
                          << std::chrono::duration<double>(circuit_check_time).count()
                          << " s for the circuit check";
            }
            std::cout.unsetf(std::ios::floatfield);
            std::cout << "\n";
            std::cout << "\nFTEC            : "
                      << (result.clean() && result.valid() ? "accepted" : "rejected") << "\n";
        }
        report_cost(traversal);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
