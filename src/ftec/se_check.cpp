#include "ftec/se_check.hpp"

#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ftec {
namespace {

// A Pauli operator without its phase, as X and Z bits per qubit.
struct Pauli {
    std::vector<bool> x, z;
    explicit Pauli(std::size_t n) : x(n, false), z(n, false) {}

    [[nodiscard]] char at(std::size_t q) const {
        return x[q] ? (z[q] ? 'Y' : 'X') : (z[q] ? 'Z' : 'I');
    }
};

// Conjugation by a primitive gate, phases ignored. Every primitive is its own
// inverse up to a phase (s and sdg differ only by one), so the same map serves
// for propagating backwards.
void conjugate(Pauli& p, const std::string& gate, const std::vector<std::size_t>& q) {
    if (gate == "i" || gate == "id" || gate == "x" || gate == "y" || gate == "z") return;
    if (gate == "h") {
        const bool x = p.x[q[0]];
        p.x[q[0]] = p.z[q[0]];
        p.z[q[0]] = x;
        return;
    }
    if (gate == "s" || gate == "sdg") {
        p.z[q[0]] = p.z[q[0]] != p.x[q[0]];
        return;
    }
    const std::size_t a = q.at(0), b = q.at(1);
    if (gate == "cx") {            // X_a -> X_a X_b, Z_b -> Z_a Z_b
        p.x[b] = p.x[b] != p.x[a];
        p.z[a] = p.z[a] != p.z[b];
        return;
    }
    if (gate == "cz") {            // X_a -> X_a Z_b, X_b -> Z_a X_b
        const bool xa = p.x[a], xb = p.x[b];
        p.z[a] = p.z[a] != xb;
        p.z[b] = p.z[b] != xa;
        return;
    }
    if (gate == "cy") {            // X_a -> X_a Y_b, X_b -> Z_a X_b, Z_b -> Z_a Z_b
        const bool xa = p.x[a], xb = p.x[b], zb = p.z[b];
        p.x[b] = xb != xa;
        p.z[b] = zb != xa;
        p.z[a] = p.z[a] != (xb != zb);
        return;
    }
    throw std::runtime_error("unsupported gate '" + gate + "'");
}

// Is `p` (over the data qubits) in the group the generators span? Phases are
// ignored, so this is linear algebra over GF(2) on (x | z) vectors.
bool in_group(const std::string& p, const std::vector<std::string>& generators) {
    const std::size_t n = p.size();
    const auto vec = [n](const std::string& s) {
        std::vector<bool> v(2 * n, false);
        for (std::size_t i = 0; i < n; ++i) {
            v[i]     = s[i] == 'X' || s[i] == 'Y';
            v[n + i] = s[i] == 'Z' || s[i] == 'Y';
        }
        return v;
    };
    std::vector<std::vector<bool>> rows;
    for (const auto& g : generators) {
        if (g.size() == n) rows.push_back(vec(g));
    }
    std::vector<bool> target = vec(p);
    // Gaussian elimination, reducing the target along the way.
    std::size_t rank = 0;
    for (std::size_t col = 0; col < 2 * n && rank < rows.size(); ++col) {
        std::size_t pivot = rank;
        while (pivot < rows.size() && !rows[pivot][col]) ++pivot;
        if (pivot == rows.size()) continue;
        std::swap(rows[rank], rows[pivot]);
        for (std::size_t r = 0; r < rows.size(); ++r) {
            if (r != rank && rows[r][col]) {
                for (std::size_t c = 0; c < 2 * n; ++c) rows[r][c] = rows[r][c] != rows[rank][c];
            }
        }
        if (target[col]) {
            for (std::size_t c = 0; c < 2 * n; ++c) target[c] = target[c] != rows[rank][c];
        }
        ++rank;
    }
    for (bool b : target) {
        if (b) return false;
    }
    return true;
}

} // namespace

std::vector<std::string> check_se_circuit(const QasmProgram&             program,
                                          const CircuitRef&              circuit,
                                          const std::vector<std::string>& stabilizers) {
    std::vector<std::string> issues;
    const auto issue = [&](std::size_t line, const std::string& what) {
        issues.push_back((line ? "line " + std::to_string(line) + ": " : std::string()) + what);
    };

    // One index per qubit, data first.
    std::map<std::string, std::size_t> base;
    std::size_t total = 0;
    if (!program.has_qubit_register(circuit.data_qubits)) {
        issue(0, "no qubit register '" + circuit.data_qubits + "' (qd)");
        return issues;
    }
    const std::size_t data = program.qubit_width(circuit.data_qubits);
    base[circuit.data_qubits] = 0;
    total = data;
    for (const auto& reg : program.qubits) {
        if (reg.name == circuit.data_qubits) continue;
        base[reg.name] = total;
        total += reg.width;
    }
    const auto index = [&](const QubitRef& ref) { return base.at(ref.reg) + ref.index; };
    const auto qubit_name = [&](std::size_t q) {
        for (const auto& reg : program.qubits) {
            const std::size_t b = base.at(reg.name);
            if (q >= b && q < b + reg.width) return reg.name + "[" + std::to_string(q - b) + "]";
        }
        return std::string("?");
    };

    // Which classical register each role measures into.
    const auto register_of = [&](const std::string& qreg) -> std::optional<std::string> {
        for (const auto& in : program.instructions) {
            if (in.kind == QasmInstruction::Kind::Measure && in.qubits.at(0).reg == qreg) return in.target.reg;
        }
        return std::nullopt;
    };
    const auto syndrome_reg = register_of(circuit.syndrome_qubits);
    const auto flag_reg     = circuit.flag_qubits ? register_of(*circuit.flag_qubits) : std::nullopt;
    if (!syndrome_reg) {
        issue(0, "never measures its qm register '" + circuit.syndrome_qubits + "'");
        return issues;
    }

    const std::size_t width = program.has_bit_register(*syndrome_reg) ? program.bit_width(*syndrome_reg) : 0;
    if (circuit.measures.size() != width) {
        issue(0, "'" + *syndrome_reg + "' has " + std::to_string(width) + " bit(s) but g: declares " +
                     std::to_string(circuit.measures.size()) + " generator(s)");
    }

    // What one measurement observes, propagated back to the start of the
    // circuit; nullopt (with an issue recorded) when its outcome is not a
    // fixed function of the data.
    const auto observable = [&](std::size_t at, const std::string& label) -> std::optional<std::string> {
        const auto& meas = program.instructions[at];
        Pauli p(total);
        p.z[index(meas.qubits.at(0))] = true;
        bool ok = true;
        for (std::size_t j = at; j-- > 0;) {
            const auto& in = program.instructions[j];
            switch (in.kind) {
                case QasmInstruction::Kind::Barrier:
                    break;
                case QasmInstruction::Kind::Gate: {
                    std::vector<std::size_t> qs;
                    for (const auto& q : in.qubits) qs.push_back(index(q));
                    conjugate(p, in.gate, qs);
                    break;
                }
                case QasmInstruction::Kind::Reset:
                case QasmInstruction::Kind::Measure: {
                    const std::size_t q = index(in.qubits.at(0));
                    if (!p.x[q] && !p.z[q]) break;
                    if (p.x[q]) {
                        issue(meas.line, label + " is random: it reads " + std::string(1, p.at(q)) + " on " +
                                             qubit_name(q) + " right after the " +
                                             (in.kind == QasmInstruction::Kind::Reset ? "reset" : "measurement") +
                                             " on line " + std::to_string(in.line));
                        ok = false;
                    } else if (in.kind == QasmInstruction::Kind::Measure) {
                        issue(meas.line, label + " also depends on the earlier measurement of " +
                                             qubit_name(q) + " on line " + std::to_string(in.line) +
                                             "; reset the qubit before reusing it");
                        ok = false;
                    }
                    p.x[q] = p.z[q] = false;   // |0>: a Z there only fixes the sign
                    break;
                }
            }
            if (!ok) return std::nullopt;
        }
        // Ancillas start in |0>.
        for (std::size_t q = data; q < total; ++q) {
            if (p.x[q]) {
                issue(meas.line, label + " is random: it reads " + std::string(1, p.at(q)) + " on " +
                                     qubit_name(q) + ", which is never reset");
                return std::nullopt;
            }
        }
        std::string on_data;
        for (std::size_t q = 0; q < data; ++q) on_data += p.at(q);
        return on_data;
    };

    std::set<std::size_t> measured_bits;
    for (std::size_t i = 0; i < program.instructions.size(); ++i) {
        const auto& in = program.instructions[i];
        if (in.kind != QasmInstruction::Kind::Measure) continue;
        const std::string label = in.target.reg + "[" + std::to_string(in.target.index) + "]";
        if (in.target.reg == *syndrome_reg) {
            measured_bits.insert(in.target.index);
            const auto got = observable(i, label);
            if (!got || in.target.index >= circuit.measures.size()) continue;
            const std::string& want = circuit.measures[in.target.index];
            if (*got != want) {
                issue(in.line, label + " measures " + *got + ", but g: declares " + want);
            }
        } else if (flag_reg && in.target.reg == *flag_reg) {
            const auto got = observable(i, label);
            if (got && !in_group(*got, stabilizers)) {
                issue(in.line, "flag " + label + " measures " + *got +
                                   " on the data, which is not a stabilizer: the flag would disturb "
                                   "the encoded state");
            }
        }
    }
    for (std::size_t b = 0; b < width; ++b) {
        if (!measured_bits.count(b)) issue(0, *syndrome_reg + "[" + std::to_string(b) + "] is never measured");
    }
    return issues;
}

std::vector<std::string> check_se_circuits(const Dag& dag) {
    std::vector<std::string> issues;
    std::set<std::string>    seen;
    for (const auto& node : dag.nodes) {
        if (node.kind != DagNode::Kind::Se) continue;
        const CircuitRef& c = node.circuit;
        std::ostringstream key;
        key << c.qasm.string() << '|' << c.data_qubits << '|' << c.syndrome_qubits << '|'
            << c.flag_qubits.value_or("") << '|';
        for (const auto& g : c.measures) key << g << ',';
        if (!seen.insert(key.str()).second) continue;

        const std::string where = c.se_name + " (" + c.qasm.filename().string() + ")";
        try {
            const auto program = parse_qasm_file(c.qasm);
            for (const auto& problem : check_se_circuit(program, c, dag.code.generators)) {
                issues.push_back(where + ", " + problem);
            }
        } catch (const std::exception& error) {
            issues.push_back(where + ": " + error.what());
        }
    }
    return issues;
}

} // namespace ftec
