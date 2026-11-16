// Tests for the circuit validity check (ftec/circuit_check.hpp).
//
//   circuit_check_tests [--bad] <protocol.fpdl>...
//
// Every protocol given must pass the check, except one marked --bad, which
// must fail it; then a set of deliberately broken
// circuits, each a small edit of a correct one, must each be caught with the
// expected message.

#include "ftec/dag.hpp"
#include "ftec/qasm.hpp"
#include "ftec/circuit_check.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "  FAILED: " << what << '\n';
    ++failures;
}

fpdl::ParseResult expand(const std::filesystem::path& source) {
    fpdl::ParseOptions options;
    options.max_paths = 5000;
    for (std::size_t bound = 16; bound <= (1u << 16); bound *= 2) {
        options.bmc_bound = bound;
        auto parsed = fpdl::Parser::parse_file(source, options);
        bool any_path_bounded = false;
        for (const auto& path : parsed.paths) any_path_bounded |= path.bound_exceeded;
        if (!parsed.truncated && !any_path_bounded) return parsed;
    }
    throw std::runtime_error("symbolic expansion did not converge");
}

// The [[5,1,3]] code, and LL25's fully parallel flagged circuit for it (four
// syndrome qubits, four flags), as the base the broken variants edit.
const std::vector<std::string> kCode = {"XZZXI", "IXZZX", "XIXZZ", "ZXIXZ"};
const std::vector<std::string> kLogicals = {"XXXXX", "ZZZZZ"};

const std::string kGood = R"(OPENQASM 3.0;
qubit[5] data;
qubit[4] syn;
qubit[4] flag;
bit[4] m;
bit[4] f;
reset syn[0]; h syn[0]; reset syn[1]; h syn[1];
reset syn[2]; h syn[2]; reset syn[3]; h syn[3];
reset flag[0]; reset flag[1]; reset flag[2]; reset flag[3];
cx syn[0], data[0]; cx syn[1], data[1]; cx syn[2], data[2]; cx syn[3], data[3];
cx syn[0], flag[0]; cx syn[1], flag[1]; cx syn[2], flag[2]; cx syn[3], flag[3];
cz syn[0], data[1]; cz syn[1], data[2]; cz syn[2], data[3]; cz syn[3], data[4];
cz syn[0], data[2]; cz syn[1], data[3]; cz syn[2], data[4]; cz syn[3], data[0];
cx syn[0], flag[0]; cx syn[1], flag[1]; cx syn[2], flag[2]; cx syn[3], flag[3];
cx syn[0], data[3]; cx syn[1], data[4]; cx syn[2], data[0]; cx syn[3], data[1];
h syn[0]; m[0] = measure syn[0];
h syn[1]; m[1] = measure syn[1];
h syn[2]; m[2] = measure syn[2];
h syn[3]; m[3] = measure syn[3];
f[0] = measure flag[0]; f[1] = measure flag[1]; f[2] = measure flag[2]; f[3] = measure flag[3];
)";

std::string edit(std::string text, const std::string& from, const std::string& to) {
    const auto at = text.find(from);
    if (at == std::string::npos) throw std::runtime_error("test setup: '" + from + "' not found");
    return text.replace(at, from.size(), to);
}

std::vector<std::string> run(const std::string& qasm) {
    ftec::CircuitRef c;
    c.se_name         = "FSE_f";
    c.data_qubits     = "data";
    c.syndrome_qubits = "syn";
    c.flag_qubits     = "flag";
    c.measures        = kCode;
    std::vector<std::string> issues;
    for (const auto& i : ftec::check_circuit(ftec::parse_qasm(qasm, "test"), c, kCode, kLogicals)) {
        issues.push_back(i.to_string());
    }
    return issues;
}

void expect(const std::string& name, const std::string& qasm, const std::string& message) {
    const auto issues = run(qasm);
    bool found = false;
    for (const auto& i : issues) found |= i.find(message) != std::string::npos;
    check(found, name + ": expected an issue containing \"" + message + "\"" +
                     (issues.empty() ? ", got none" : ", got: " + issues.front()));
    if (found) std::cout << "  caught: " << name << '\n';
}

} // namespace

int main(int argc, char** argv) {
    // "--bad" before a protocol: its circuits are known to be wrong, and the
    // check must say so.
    bool bad = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--bad") { bad = true; continue; }
        const std::filesystem::path source = argv[i];
        const std::string name = source.parent_path().filename().string();
        const bool expect_bad = bad;
        bad = false;
        try {
            const auto dag    = ftec::build_dag(expand(source), source.parent_path());
            const auto issues = ftec::check_circuits(dag).errors;
            if (expect_bad) {
                check(!issues.empty(), name + ": expected SE-check issues, found none");
                if (!issues.empty()) std::cout << name << ": caught (" << issues.size() << " issues), as expected\n";
                continue;
            }
            for (const auto& issue : issues) check(false, name + ": " + issue);
            if (issues.empty()) std::cout << name << ": ok\n";
        } catch (const std::exception& error) {
            check(false, source.string() + ": " + error.what());
        }
    }

    check(run(kGood).empty(), "the unedited circuit has issues: " + (run(kGood).empty() ? "" : run(kGood).front()));
    std::cout << "broken circuits:\n";
    expect("data qubit coupled to the wrong ancilla",
           edit(kGood, "cz syn[0], data[1];", "cz syn[1], data[1];"), "m[0] is random");
    expect("wrong data qubit", edit(kGood, "cz syn[0], data[2];", "cz syn[0], data[4];"), "m[0] is random");
    expect("wrong Pauli type", edit(kGood, "cx syn[0], data[0];", "cz syn[0], data[0];"),
           "m[0] measures ZZZXI, but g: declares XZZXI");
    expect("a generator never coupled",
           edit(edit(edit(edit(kGood, "cx syn[0], data[0];", ""), "cz syn[0], data[1];", ""), "cz syn[0], data[2];", ""),
                "cx syn[0], data[3];", ""),
           "m[0] measures IIIII, but g: declares XZZXI");
    expect("measured in Z instead of X", edit(kGood, "h syn[0]; m[0]", "m[0]"), "m[0] is random");
    expect("read-out order swapped",
           edit(edit(kGood, "m[0] = measure syn[0];", "m[0] = measure syn[1];"), "m[1] = measure syn[1];",
                "m[1] = measure syn[0];"),
           "m[0] is random: it reads X on syn[1]");
    expect("a syndrome bit never measured", edit(kGood, "h syn[3]; m[3] = measure syn[3];", ""), "m[3] is never measured");
    expect("ancilla reused without a reset",
           edit(kGood, "h syn[1]; m[1] = measure syn[1];", "h syn[1]; m[1] = measure syn[1]; h syn[1]; cz syn[1], data[0]; h syn[1]; m[1] = measure syn[1];"),
           "depends on the earlier measurement");
    expect("ancilla never prepared in |+>", edit(kGood, "reset syn[2]; h syn[2];", "reset syn[2];"), "m[2] is random");
    expect("flag coupled to a data qubit", edit(kGood, "cx syn[0], flag[0]; cx syn[1], flag[1];",
                                                "cx syn[0], flag[0]; cx data[4], flag[1];"),
           "f[1] is random");

    expect("syndrome sign flipped (a warning)",
           edit(kGood, "h syn[0]; m[0]", "z syn[0]; h syn[0]; m[0]"), "m[0] observes -XZZXI");
    expect("a stray Hadamard on the data",
           edit(kGood, "h syn[0]; m[0]", "h data[2]; h syn[0]; m[0]"), "the circuit disturbs the encoded state");
    expect("a stray X on the data flips the logical sign (a warning)",
           edit(kGood, "h syn[0]; m[0]", "x data[0]; h syn[0]; m[0]"), "sign: logical ZZZZZ comes out as -ZZZZZ");
    expect("data error does not pass through (a CX between data qubits)",
           edit(kGood, "h syn[0]; m[0]", "cx data[0], data[1]; h syn[0]; m[0]"), "is not the same error up to a stabilizer");

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "every circuit is valid, and every broken one is caught\n";
    return 0;
}
