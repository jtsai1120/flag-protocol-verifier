// Sets far too large to list are still small decision diagrams.
#include "spbdd/spbdd.hpp"
#include <iostream>
using namespace spbdd;

int main() {
    context ctx(17);

    for (int w = 1; w <= 4; ++w) {
        pset e = ctx.weight_at_most(w);
        std::cout << "weight <= " << w << ": " << static_cast<long long>(e.size())
                  << " Paulis in " << e.node_count() << " BDD nodes\n";
    }

    // In a pattern, '*' stands for any of I, X, Y, Z.
    pset p = ctx.matching("X*Z**************");
    std::cout << "X on qubit 0 and Z on qubit 2: " << p.size() << " Paulis\n";

    pset q = p & ctx.weight_exactly(3);
    std::cout << "...of weight 3: " << q.size() << ", for example " << *q.any_element() << '\n';
}
