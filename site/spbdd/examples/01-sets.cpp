// Sets of Pauli operators, and their setwise product.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
using namespace spbdd;

// Print a set as {A, B, ...}, so that std::cout << s works.
std::ostream &operator<<(std::ostream &os, const pset &s) {
    os << '{';
    bool first = true;
    s.for_each_element([&](const std::string &e) {
        os << (first ? "" : ", ") << e;
        first = false;
        return true;   // keep going
    });
    return os << '}';
}

int main() {
    context ctx(2);   // two qubits; only one context can be alive at a time

    pset s = ctx.from_list({"XI", "ZZ"});
    std::cout << "s        = " << s << '\n';
    std::cout << "s * s    = " << s * s << '\n';   // setwise product, phases ignored
    std::cout << "|s * s|  = " << (s * s).size() << '\n';
    std::cout << "s | {YY} = " << (s | ctx.from_str("YY")) << '\n';
    std::cout << "s & {ZZ} = " << (s & ctx.from_str("ZZ")) << '\n';
    std::cout << "<XI, ZZ> = " << ctx.generated_by({"XI", "ZZ"}) << '\n';
    std::cout << "all two-qubit Paulis: " << ctx.all().size() << '\n';
}
