// Clifford gates act on every element of a set at once, and a measurement
// splits the set by outcome.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
using namespace spbdd;

void print(const std::string &name, const pset &s) {
    std::cout << name << " = {";
    bool first = true;
    s.for_each_element([&](const std::string &e) {
        std::cout << (first ? "" : ", ") << e;
        first = false;
        return true;
    });
    std::cout << "}\n";
}

int main() {
    context ctx(3);

    pset e = ctx.from_list({"XII", "IZI", "IIY"});
    print("before", e);

    e.h(0).cx(0, 1).cz(1, 2);   // gates change the set in place, and chain
    print("after H(0) CX(0,1) CZ(1,2)", e);

    // meas_0: the elements that commute with Z on qubit 2 (outcome 0),
    // meas_1: the ones that anticommute with it (outcome 1).
    meas_result m = e.measure_z(2);
    print("commute with Z2", m.meas_0);
    print("anticommute with Z2", m.meas_1);
}
