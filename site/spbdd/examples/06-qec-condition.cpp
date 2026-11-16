// The QEC condition on the distance-3 rotated surface code: every error of
// weight <= 1 can be corrected, but not every error of weight <= 2. The four
// methods give the same verdict at different cost.
#include "spbdd/spbdd.hpp"
#include <iostream>
#include <string>
using namespace spbdd;

std::string name_of(code::fqcv_method m) {
    if (m == code::fqcv_method::bruteforce) return "bruteforce";
    if (m == code::fqcv_method::table) return "table";
    if (m == code::fqcv_method::image) return "image";
    return "square";
}

int main() {
    context ctx(9);
    code c(ctx, {"ZIIZIIIII", "IZZIZZIII", "IIIZZIZZI", "IIIIIZIIZ",
                 "IXXIIIIII", "XXIXXIIII", "IIIIXXIXX", "IIIIIIXXI"}, 3);

    for (int w = 1; w <= 2; ++w) {
        pset errors = ctx.weight_at_most(w);
        for (auto m : {code::fqcv_method::bruteforce, code::fqcv_method::table,
                       code::fqcv_method::image, code::fqcv_method::square}) {
            std::cout << "weight <= " << w << ", " << name_of(m) << ": ";
            if (auto v = c.find_qec_cond_violation(errors, m))
                std::cout << "violated by " << v->first << " and " << v->second << '\n';
            else
                std::cout << "satisfied\n";
        }
    }
}
