#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ftec {

// A neutral reading of an OpenQASM 3 circuit: what registers exist, and a flat
// list of things that happen to them.
//
// This deliberately says nothing about Paulis, faults or error models. Which
// register holds the encoded state and which are ancillas is a question the
// protocol answers (qd:/qm:/qf: in the .fpdl), not the circuit; and how a
// gate acts on a symbolic state is a backend's business. Keeping the front end
// ignorant of both is what lets a second backend reuse it.

struct QubitRef {
    std::string reg;
    std::size_t index = 0;
};

struct BitRef {
    std::string reg;
    std::size_t index = 0;
};

struct QasmInstruction {
    enum class Kind { Gate, Measure, Reset, Barrier };

    Kind        kind = Kind::Gate;
    std::string gate;                 // Kind::Gate: a primitive, see kPrimitiveGates
    std::vector<QubitRef> qubits;
    BitRef      target;               // Kind::Measure
    std::size_t line = 0;
};

struct RegisterDecl {
    std::string name;
    std::size_t width = 1;            // a scalar `qubit q;` has width 1
};

struct QasmProgram {
    std::string               name;   // for diagnostics
    std::vector<RegisterDecl> qubits;
    std::vector<RegisterDecl> bits;
    std::vector<QasmInstruction> instructions;

    [[nodiscard]] bool        has_qubit_register(std::string_view) const;
    [[nodiscard]] bool        has_bit_register(std::string_view) const;
    [[nodiscard]] std::size_t qubit_width(std::string_view) const;
    [[nodiscard]] std::size_t bit_width(std::string_view) const;
    [[nodiscard]] std::size_t total_qubits() const;
};

class QasmError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The gates a backend has to understand. Custom `gate` definitions are inlined
// away during parsing, so nothing else ever reaches the instruction list.
inline constexpr std::string_view kPrimitiveGates[] = {
    "i", "id", "x", "y", "z", "h", "s", "sdg", "cx", "cy", "cz"};

[[nodiscard]] QasmProgram parse_qasm(std::string_view source, const std::string& name);
[[nodiscard]] QasmProgram parse_qasm_file(const std::filesystem::path& path);

} // namespace ftec
