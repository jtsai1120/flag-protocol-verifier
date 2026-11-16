// Clifford gates act on every element of a set at once, and a measurement
// splits the set by outcome.
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
    context ctx(3);

    pset e = ctx.from_list({"XII", "IZI", "IIY"});
    std::cout << "before: " << e << '\n';

    e.h(0).cx(0, 1).cz(1, 2);   // gates change the set in place, and chain
    std::cout << "after H(0) CX(0,1) CZ(1,2): " << e << '\n';

    // meas_0: the elements that commute with Z on qubit 2 (outcome 0),
    // meas_1: the ones that anticommute with it (outcome 1).
    meas_result m = e.measure_z(2);
    std::cout << "commute with Z2:     " << m.meas_0 << '\n';
    std::cout << "anticommute with Z2: " << m.meas_1 << '\n';
}
