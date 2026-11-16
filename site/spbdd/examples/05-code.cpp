// The [[5,1,3]] code: its groups, syndromes, and errors by syndrome.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
#include <vector>
using namespace spbdd;

std::string bits(const std::vector<bool> &b) {
    std::string s;
    for (bool x : b) s += x ? '1' : '0';
    return s;
}

int main() {
    context ctx(5);
    code c(ctx, {"XZZXI", "IXZZX", "XIXZZ", "ZXIXZ"}, 3);

    std::cout << "[[" << c.n() << "," << c.k() << "," << c.d() << "]]\n";
    std::cout << "logical X: " << c.logical_X_operators()[0]
              << ", logical Z: " << c.logical_Z_operators()[0] << '\n';
    std::cout << "|S| = " << c.S().size() << ", |N(S)| = " << c.N().size()
              << ", |N(S)-S| = " << c.NmS().size() << '\n';

    for (const std::string e : {"IIIII", "XIIII", "IZIII", "IIYII"})
        std::cout << "syndrome of " << e << " = " << bits(c.syndrome_of(e)) << '\n';

    pset w1 = c.by_syndrome({true, false, false, false}) & ctx.weight_at_most(1);
    std::cout << "weight <= 1 with syndrome 1000: " << *w1.any_element()
              << " (" << w1.size() << " in all)\n";
}
