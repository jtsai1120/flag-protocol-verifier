#include "ftec/qasm.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "  FAILED: " << what << '\n';
    ++failures;
}

bool rejects(const std::string& source, const std::string& what) {
    try {
        [[maybe_unused]] const auto ignored = ftec::parse_qasm(source, "<test>");
    } catch (const ftec::QasmError&) {
        return true;
    }
    std::cerr << "  FAILED: accepted " << what << '\n';
    ++failures;
    return false;
}

const std::string kHead = "OPENQASM 3.0;\ninclude \"stdgates.inc\";\n";

ftec::QasmProgram parse(const std::string& body) {
    return ftec::parse_qasm(kHead + body, "<test>");
}

} // namespace

int main(int argc, char** argv) {
    std::cout << "== declarations ==\n";
    {
        const auto program = parse("qubit[5] data;\nqubit syn;\nbit[4] m;\nbit f;\n");
        check(program.qubit_width("data") == 5, "sized qubit register");
        check(program.qubit_width("syn") == 1, "a scalar qubit is a register of width one");
        check(program.bit_width("m") == 4, "sized bit register");
        check(program.bit_width("f") == 1, "scalar bit register");
        check(program.total_qubits() == 6, "total qubits");
        check(!program.has_qubit_register("nope"), "unknown register is absent");
    }
    rejects(kHead + "qubit[0] q;", "an empty register");
    rejects(kHead + "qubit[2] q;\nqubit[2] q;", "a duplicate register");
    rejects(kHead + "h q[0];", "a gate on an undeclared register");
    rejects(kHead + "qubit[2] q;\nh q[5];", "an out-of-range index");
    rejects("qubit[2] q;", "a file with no OPENQASM header");
    rejects("OPENQASM 2.0;\nqubit[2] q;", "OpenQASM 2");
    rejects(kHead + "qubit[2] q;\nh q[0]", "a missing semicolon");

    std::cout << "== custom gates are inlined away ==\n";
    {
        const auto program = parse(
            "qubit[2] q;\n"
            "gate meas_x d, a {\n"
            "  h d;\n"
            "  cx d, a;\n"
            "  h d;\n"
            "}\n"
            "meas_x q[0], q[1];\n");
        check(program.instructions.size() == 3, "the call expanded to three instructions");
        for (const auto& instruction : program.instructions) {
            check(instruction.kind == ftec::QasmInstruction::Kind::Gate, "all are gates");
            const bool primitive =
                std::find(std::begin(ftec::kPrimitiveGates), std::end(ftec::kPrimitiveGates),
                          instruction.gate) != std::end(ftec::kPrimitiveGates);
            check(primitive, "no custom gate survives into the instruction list");
        }
        check(program.instructions[0].gate == "h" && program.instructions[0].qubits[0].index == 0,
              "parameters bound to the caller's qubits");
        check(program.instructions[1].gate == "cx" && program.instructions[1].qubits[1].index == 1,
              "second operand bound");
    }
    {
        // A definition may call an earlier one; the expansion has to follow.
        const auto program = parse(
            "qubit[2] q;\n"
            "gate inner d { h d; }\n"
            "gate outer d, a { inner d; cx d, a; inner d; }\n"
            "outer q[0], q[1];\n");
        check(program.instructions.size() == 3, "nested definitions expand");
        check(program.instructions[0].gate == "h", "inner gate reached");
    }
    rejects(kHead + "qubit[2] q;\ngate g d { g d; }\ng q[0];", "a recursive gate");
    rejects(kHead + "qubit[2] q;\ngate h d { x d; }", "redefining a primitive");
    rejects(kHead + "qubit[2] q;\nnot_a_gate q[0];", "an unknown gate");
    rejects(kHead + "qubit[2] q;\nbit c;\ngate g d { c = measure d; }\ng q[0];",
            "a measurement inside a gate body");

    std::cout << "== measurement and reset are instructions, in order ==\n";
    {
        const auto program = parse(
            "qubit[2] q;\nbit[2] m;\n"
            "reset q[0];\nh q[0];\nm[0] = measure q[0];\nreset q[0];\nm[1] = measure q[0];\n");
        check(program.instructions.size() == 5, "five instructions");
        using Kind = ftec::QasmInstruction::Kind;
        check(program.instructions[0].kind == Kind::Reset, "reset first");
        check(program.instructions[2].kind == Kind::Measure, "measure third");
        check(program.instructions[2].target.reg == "m" && program.instructions[2].target.index == 0,
              "measurement names its destination bit");
        check(program.instructions[4].target.index == 1,
              "the second measurement of the same qubit lands in a different bit");
    }
    {
        // A whole-register measurement fans out, one instruction per qubit.
        const auto program = parse("qubit[3] q;\nbit[3] m;\nm = measure q;\n");
        check(program.instructions.size() == 3, "whole-register measurement fans out");
        check(program.instructions[2].qubits[0].index == 2 &&
                  program.instructions[2].target.index == 2,
              "qubit i goes to bit i");
    }
    rejects(kHead + "qubit[3] q;\nbit[2] m;\nm = measure q;", "a width mismatch on measurement");
    rejects(kHead + "qubit[2] q;\nm = measure q[0];", "an undeclared bit register");
    rejects(kHead + "qubit[2] q;\nbit[2] m;\nm[9] = measure q[0];", "an out-of-range bit index");

    std::cout << "== gate arity and operands ==\n";
    rejects(kHead + "qubit[2] q;\ncx q[0];", "cx with one operand");
    rejects(kHead + "qubit[2] q;\nh q[0], q[1];", "h with two operands");
    rejects(kHead + "qubit[2] q;\ncx q[0], q[0];", "cx on one qubit twice");
    rejects(kHead + "qubit[2] q;\nh q;", "a gate on a whole multi-qubit register");
    {
        // A width-one register may be named without an index, which is how
        // these protocols refer to their single ancillas.
        const auto program = parse("qubit[2] q;\nqubit syn;\ncx q[0], syn;\n");
        check(program.instructions.size() == 1, "scalar register as a gate operand");
        check(program.instructions[0].qubits[1].reg == "syn", "resolved to the scalar register");
    }
    {
        const auto program = parse("qubit[2] q;\nbarrier q;\ns q[0];\nsdg q[1];\n");
        check(program.instructions.size() == 3, "barrier is kept as an instruction");
        check(program.instructions[0].kind == ftec::QasmInstruction::Kind::Barrier, "barrier");
        check(program.instructions[1].gate == "s" && program.instructions[2].gate == "sdg",
              "s and sdg are primitives");
    }

    if (argc > 1) {
        std::cout << "== the protocols' own circuits ==\n";
        std::size_t gates = 0, measures = 0, resets = 0;
        for (int i = 1; i < argc; ++i) {
            try {
                const auto program = ftec::parse_qasm_file(argv[i]);
                check(!program.qubits.empty(),
                      std::string(argv[i]) + " declares qubits");
                for (const auto& instruction : program.instructions) {
                    using Kind = ftec::QasmInstruction::Kind;
                    if (instruction.kind == Kind::Gate) ++gates;
                    if (instruction.kind == Kind::Measure) ++measures;
                    if (instruction.kind == Kind::Reset) ++resets;
                    for (const auto& qubit : instruction.qubits) {
                        check(program.has_qubit_register(qubit.reg),
                              "every operand names a declared register");
                    }
                }
            } catch (const std::exception& error) {
                std::cerr << "  FAILED: " << argv[i] << ": " << error.what() << '\n';
                ++failures;
            }
        }
        std::cout << "  " << (argc - 1) << " circuits: " << gates << " gates, " << measures
                  << " measurements, " << resets << " resets\n";
        check(measures > 0 && resets > 0,
              "the corpus really does use mid-circuit measurement and reset");
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "\nAll QASM front-end tests passed.\n";
    return 0;
}
