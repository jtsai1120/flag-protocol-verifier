// Every error that one fault can leave behind in a small circuit:
// CX(0,1), then CX(1,2), with each two-qubit gate a fault location.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
#include <vector>
using namespace spbdd;

int main() {
    context ctx(3);

    // by_t[t]: the errors made by exactly t faults
    std::vector<pset> by_t = {ctx.identity(), ctx.empty()};

    const std::vector<std::vector<int>> gates = {{0, 1}, {1, 2}};
    for (const auto &g : gates) {
        for (auto &level : by_t) level.cx(g[0], g[1]);
        by_t[1] |= by_t[0].fault_inject(g);   // a fault right after this gate
    }

    std::cout << "no fault : " << by_t[0].size() << " error\n";
    std::cout << "one fault: " << by_t[1].size() << " errors\n";

    pset heavy = by_t[1] & ctx.weight_exactly(3);
    std::cout << "of weight 3: " << heavy.size() << " -";
    heavy.for_each_element([](const std::string &e) {
        std::cout << ' ' << e;
        return true;
    });
    std::cout << '\n';
}
