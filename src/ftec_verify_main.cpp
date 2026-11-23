#include "ftec/dag.hpp"
#include "ftec/circuit_check.hpp"
#include "ftec/spbdd_backend.hpp"
#include "ftec/verify.hpp"
#include "ftec/qasm.hpp"

#include <sys/resource.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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
        << "  --first          stop at the first failure: an unprotected record, or a\n"
        << "                   record no condition of Def. 3.9 can settle\n"
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
        << "  --explain        trace every counterexample back to an input error and\n"
        << "                   circuit faults that produce it\n"
        << "  --json-out=FILE  also write the decision tree and every counterexample, as\n"
        << "                   JSON, to FILE (implies --explain)\n"
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
// that large cannot be read as a picture anyway. With `only`, just those
// nodes (a root-to-terminal chain) and the edges between them.
std::string tree_json(const ftec::Dag& dag, std::size_t limit,
                      const std::set<std::size_t>* only = nullptr) {
    std::ostringstream out;
    out << "{\"protocol\":" << json_string(dag.protocol) << ",\"code\":\"[[" << dag.code.n << ','
        << dag.code.k << ',' << dag.code.d << "]]\",\"tau\":" << dag.code.fault_budget()
        << ",\"paths\":" << dag.path_count << ",\"node_count\":" << dag.nodes.size()
        << ",\"limit\":" << limit;
    if (only) out << ",\"chain\":true";
    if (!only && dag.nodes.size() > limit) {
        out << ",\"too_large\":true}";
        return out.str();
    }

    // An SE node's outcomes are named after its position on the path: the
    // k-th SE of a path writes id_k.s (and id_k.f when flagged).
    std::vector<std::size_t> depth(dag.nodes.size(), 0);
    for (const auto& edge : dag.edges) depth[edge.to] = depth[edge.from] + 1;

    out << ",\"nodes\":[";
    bool first = true;
    for (std::size_t i = 0; i < dag.nodes.size(); ++i) {
        if (only && !only->count(i)) continue;
        const auto& node = dag.nodes[i];
        if (!first) out << ',';
        first = false;
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
    first = true;
    for (const auto& edge : dag.edges) {
        if (only && (!only->count(edge.from) || !only->count(edge.to))) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"from\":" << edge.from << ",\"to\":" << edge.to << ",\"guards\":[";
        for (std::size_t g = 0; g < edge.guards.size(); ++g) {
            if (g) out << ',';
            out << "{\"expr\":" << json_string(edge.guards[g].expression)
                << ",\"expected\":" << (edge.guards[g].expected ? "true" : "false") << '}';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string bits(const std::vector<bool>& v) {
    std::string out;
    for (const bool b : v) out += b ? '1' : '0';
    return out;
}

// ---- counterexamples --------------------------------------------------------

std::string fault_text(const ftec::FaultSite& f, const std::string& se_name) {
    std::string where;
    for (std::size_t i = 0; i < f.qubits.size(); ++i) where += (i ? ", " : "") + f.qubits[i];
    return f.pauli + " on (" + where + ") after " + f.gate + " #" + std::to_string(f.instruction + 1) +
           " of " + se_name + " (circuit " + std::to_string(f.step) + ")";
}

void print_provenance(const std::string& label, const ftec::Provenance& p,
                      const std::vector<ftec::RecordEntry>& record, const std::string& single_circuit = "") {
    std::cout << "        " << label << " " << p.residual;
    if (!p.found) { std::cout << ": could not trace it back\n"; return; }
    if (p.up_to_stabilizer) std::cout << " (the reported error times a stabilizer)";
    const bool input = p.input_error.find_first_not_of('I') != std::string::npos;
    std::cout << " <- " << (input ? "input error " + p.input_error : std::string("no input error"));
    if (p.faults.empty()) std::cout << ", no fault";
    for (const auto& f : p.faults) {
        const std::string se = !single_circuit.empty() ? single_circuit
                             : f.step >= 1 && f.step <= record.size() ? record[f.step - 1].se_name : "?";
        std::cout << "\n          fault " << fault_text(f, se);
    }
    std::cout << "\n";
}

std::string provenance_json(const ftec::Provenance& p) {
    std::ostringstream out;
    out << "{\"found\":" << (p.found ? "true" : "false") << ",\"r\":" << p.r << ",\"s\":" << p.s
        << ",\"residual\":" << json_string(p.residual) << ",\"input_error\":" << json_string(p.input_error)
        << ",\"up_to_stabilizer\":" << (p.up_to_stabilizer ? "true" : "false")
        << ",\"faults\":[";
    for (std::size_t i = 0; i < p.faults.size(); ++i) {
        const auto& f = p.faults[i];
        if (i) out << ',';
        out << "{\"step\":" << f.step << ",\"instruction\":" << f.instruction << ",\"gate\":"
            << json_string(f.gate) << ",\"pauli\":" << json_string(f.pauli) << ",\"qubits\":[";
        for (std::size_t q = 0; q < f.qubits.size(); ++q) out << (q ? "," : "") << json_string(f.qubits[q]);
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string record_json(const std::vector<ftec::RecordEntry>& record) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < record.size(); ++i) {
        const auto& e = record[i];
        if (i) out << ',';
        out << "{\"node\":" << e.node << ",\"step\":" << i + 1 << ",\"se\":" << json_string(e.se_name)
            << ",\"s\":" << json_string(bits(e.read_syndrome())) << ",\"f\":" << json_string(bits(e.outcome.flag)) << '}';
    }
    out << ']';
    return out.str();
}

std::size_t terminal_of(const ftec::Dag& dag, std::size_t path_id) {
    for (const auto& node : dag.nodes) {
        if (node.kind == ftec::DagNode::Kind::Terminal && node.path_id == path_id) return node.id;
    }
    return 0;
}

// The nodes of a record's path, root to terminal, and (when the whole tree is
// too large to draw) the tree of just those nodes.
std::string chain_json(const ftec::Dag& dag, const std::vector<ftec::RecordEntry>& record,
                       std::size_t path_id, std::size_t limit) {
    std::set<std::size_t> ids{dag.root(), terminal_of(dag, path_id)};
    for (const auto& e : record) ids.insert(e.node);
    std::ostringstream out;
    out << ",\"terminal\":" << terminal_of(dag, path_id) << ",\"chain\":[";
    bool first = true;
    for (const auto id : ids) { out << (first ? "" : ",") << id; first = false; }
    out << ']';
    if (dag.nodes.size() > limit) out << ",\"chain_tree\":" << tree_json(dag, limit, &ids);
    return out.str();
}

void write_result_json(const std::filesystem::path& file, const ftec::Dag& dag,
                       const ftec::VerifyResult& result, std::size_t limit) {
    std::ostringstream out;
    out << "{\"tree\":" << tree_json(dag, limit);

    // Every circuit, as a flat list of operations, to draw.
    out << ",\"circuits\":{";
    std::set<std::string> done;
    bool first = true;
    for (const auto& node : dag.nodes) {
        if (node.kind != ftec::DagNode::Kind::Se || !done.insert(node.circuit.se_name).second) continue;
        const auto& c = node.circuit;
        ftec::QasmProgram program;
        try { program = ftec::parse_qasm_file(c.qasm); } catch (const std::exception&) { continue; }
        if (!first) out << ',';
        first = false;
        out << json_string(c.se_name) << ":{\"file\":" << json_string(c.qasm.filename().string())
            << ",\"registers\":[";
        for (std::size_t i = 0; i < program.qubits.size(); ++i) {
            const auto& reg = program.qubits[i];
            const char* role = reg.name == c.data_qubits ? "data"
                             : reg.name == c.syndrome_qubits ? "syndrome"
                             : c.flag_qubits && reg.name == *c.flag_qubits ? "flag" : "other";
            out << (i ? "," : "") << "{\"name\":" << json_string(reg.name) << ",\"width\":" << reg.width
                << ",\"role\":\"" << role << "\"}";
        }
        out << "],\"ops\":[";
        for (std::size_t i = 0; i < program.instructions.size(); ++i) {
            const auto& in = program.instructions[i];
            const char* kind = in.kind == ftec::QasmInstruction::Kind::Gate ? "gate"
                             : in.kind == ftec::QasmInstruction::Kind::Measure ? "measure"
                             : in.kind == ftec::QasmInstruction::Kind::Reset ? "reset" : "barrier";
            out << (i ? "," : "") << "{\"kind\":\"" << kind << "\",\"gate\":" << json_string(in.gate)
                << ",\"qubits\":[";
            for (std::size_t q = 0; q < in.qubits.size(); ++q) {
                out << (q ? "," : "")
                    << json_string(in.qubits[q].reg + "[" + std::to_string(in.qubits[q].index) + "]");
            }
            out << ']';
            if (in.kind == ftec::QasmInstruction::Kind::Measure) {
                out << ",\"target\":" << json_string(in.target.reg + "[" + std::to_string(in.target.index) + "]");
            }
            out << '}';
        }
        out << "]}";
    }
    out << '}';

    // Correctability: the first record of every failing path.
    out << ",\"correctability\":[";
    std::map<std::size_t, std::size_t> per_path;
    for (const auto& f : result.failures) ++per_path[f.path_id];
    std::set<std::size_t> shown;
    first = true;
    for (const auto& f : result.failures) {
        if (!shown.insert(f.path_id).second) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"path\":" << f.path_id << ",\"action\":" << json_string(f.terminal_action)
            << ",\"t\":" << f.failure.fault_count << ",\"records\":" << per_path[f.path_id]
            << ",\"detail\":" << json_string(f.failure.detail) << ",\"record\":" << record_json(f.record)
            << chain_json(dag, f.record, f.path_id, limit) << ",\"witnesses\":[";
        for (std::size_t i = 0; i < f.explained.size(); ++i) out << (i ? "," : "") << provenance_json(f.explained[i]);
        out << "]}";
    }
    out << ']';

    // Control-flow bugs: one record per path.
    out << ",\"validity\":[";
    shown.clear();
    first = true;
    for (const auto& v : result.validity_failures) {
        if (!shown.insert(v.path_id).second) continue;
        if (!first) out << ',';
        first = false;
        out << "{\"path\":" << v.path_id << ",\"action\":" << json_string(v.terminal_action)
            << ",\"reason\":" << json_string(v.reason) << ",\"why1\":" << json_string(v.why1)
            << ",\"why2\":" << json_string(v.why2) << ",\"why3\":" << json_string(v.why3)
            << ",\"detected\":" << v.detected << ",\"t\":" << result.tau
            << ",\"record\":" << record_json(v.record) << chain_json(dag, v.record, v.path_id, limit)
            << ",\"rounds\":[";
        for (std::size_t i = 0; i < v.rounds.size(); ++i) {
            const auto& r = v.rounds[i];
            out << (i ? "," : "") << "{\"flagged\":" << (r.flagged ? "true" : "false")
                << ",\"counted\":" << r.counted << ",\"steps\":[";
            for (std::size_t j = 0; j < r.steps.size(); ++j) out << (j ? "," : "") << r.steps[j];
            out << "]}";
        }
        out << "],\"split\":[";
        for (std::size_t i = 0; i < v.split.size(); ++i) out << (i ? "," : "") << provenance_json(v.split[i]);
        out << ']';
        if (v.heavy) out << ",\"heavy\":" << provenance_json(*v.heavy);
        out << '}';
    }
    out << ']';

    // Flag bugs.
    out << ",\"flags\":[";
    for (std::size_t i = 0; i < result.flag_failures.size(); ++i) {
        const auto& f = result.flag_failures[i];
        out << (i ? "," : "") << "{\"se\":" << json_string(f.se_name) << ",\"k\":" << f.k
            << ",\"problem\":" << json_string(f.problem) << ",\"records\":" << f.records
            << ",\"paths\":" << f.paths.size() << ",\"path\":" << f.path_id
            << ",\"record\":" << record_json(f.record) << chain_json(dag, f.record, f.path_id, limit)
            << ",\"witness\":" << provenance_json(f.witness) << '}';
    }
    out << "]}";

    std::ofstream file_out(file);
    if (!file_out) throw std::runtime_error("cannot write " + file.string());
    file_out << out.str() << "\n";
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
    std::optional<std::filesystem::path> json_out;
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
        } else if (arg == "--explain") {
            options.explain = true;
        } else if (arg.rfind("--json-out=", 0) == 0) {
            json_out = arg.substr(11);
            options.explain = true;
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
            std::cout << tree_json(dag, *tree_limit) << "\n";
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

        if (result.stopped_early) {
            std::cout << "stopped at the first failure: the rest was not explored\n\n";
        }
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
                for (std::size_t i = 0; i < failure.explained.size(); ++i) {
                    print_provenance("E" + std::to_string(i + 1), failure.explained[i], failure.record);
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
                    for (std::size_t i = 0; i < v.split.size(); ++i) {
                        print_provenance("Cond 1 error " + std::to_string(i + 1), v.split[i], v.record);
                    }
                    if (v.heavy) print_provenance("Cond 3 error", *v.heavy, v.record);
                }
            }
            if (by_path) {
                std::cout << "by path (records satisfying each condition; a record may satisfy several):\n";
                for (const auto& p : result.by_path) {
                    ftec::PathFailure f{p.path_id, p.terminal_action, p.example, {}, {}};
                    std::cout << "  path " << p.path_id << "  " << p.terminal_action << "  records " << p.records
                              << "  Cond1 " << p.cond1 << "  Cond2 " << p.cond2 << "  Cond3 " << p.cond3
                              << "  none " << p.failing << "  e.g. " << f.record_string() << "\n";
                }
            }
            std::cout << "control-flow bugs if decode() may not use Cond 3: "
                      << result.invalid_paths_strict << " path(s)\n";
            if (result.stopped_early) {
                std::cout << "flag bugs       : not checked (stopped at the first failure)\n";
            } else if (result.flags_valid()) {
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
                    if (options.explain && f.k > 0) print_provenance("error", f.witness, {}, f.se_name);
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
        if (json_out) write_result_json(*json_out, dag, result, 1000);
        report_cost(traversal);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
