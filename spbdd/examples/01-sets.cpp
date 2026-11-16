// Sets of Pauli operators, and their setwise product.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
using namespace spbdd;

// Print a set as {A, B, ...}.
void print(const std::string &name, const pset &s) {
    std::cout << name << " = {";
    bool first = true;
    s.for_each_element([&](const std::string &e) {
        std::cout << (first ? "" : ", ") << e;
        first = false;
        return true;   // keep going
    });
    std::cout << "}\n";
}

int main() {
    context ctx(2);   // two qubits; only one context can be alive at a time

    pset s = ctx.from_list({"XI", "ZZ"});
    print("s", s);
    print("s * s", s * s);   // setwise product, phases ignored
    std::cout << "|s * s| = " << (s * s).size() << '\n';
    print("s | {YY}", s | ctx.from_str("YY"));
    print("s & {ZZ}", s & ctx.from_str("ZZ"));
    print("<XI, ZZ>", ctx.generated_by({"XI", "ZZ"}));
    std::cout << "all two-qubit Paulis: " << ctx.all().size() << '\n';
}
