// JavaScript bindings for SPBDD, for the SPBDD playground.
//
// The API mirrors the C++ one name for name (snake_case kept), with the
// operators spelled as methods:
//
//   a | b  -> a.union(b)        a & b -> a.intersect(b)    a - b -> a.minus(b)
//   a ^ b  -> a.xor(b)          ~a    -> a.complement()     a * b -> a.mul(b)
//   a == b -> a.equals(b)
//
// Gates mutate the set in place, as in C++; web/spbdd/prelude.js makes them
// return the set so they chain (s.h(0).cx(0, 1)). Vectors cross the boundary
// as JS arrays, syndromes and logical components as bit strings ("0101"), and
// a QEC-condition violation as [e1, e2] or null.

#include "spbdd/spbdd.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <memory>
#include <string>
#include <vector>

using namespace emscripten;
using spbdd::code;
using spbdd::context;
using spbdd::pset;

namespace {

std::vector<std::string> strings(const val& array) {
    return vecFromJSArray<std::string>(array);
}

std::vector<int> ints(const val& array) { return vecFromJSArray<int>(array); }

val to_array(const std::vector<std::string>& items) {
    val out = val::array();
    for (std::size_t i = 0; i < items.size(); ++i) out.set(i, items[i]);
    return out;
}

std::string bits(const std::vector<bool>& b) {
    std::string s;
    for (bool x : b) s += x ? '1' : '0';
    return s;
}

std::vector<bool> from_bits(const std::string& s) {
    std::vector<bool> b;
    for (char c : s) {
        if (c == '0' || c == '1') b.push_back(c == '1');
        else if (c != ' ' && c != '_') throw std::runtime_error("not a bit string: " + s);
    }
    return b;
}

// --- context -------------------------------------------------------------------

std::unique_ptr<context> make_context(int n) { return std::make_unique<context>(n); }
std::unique_ptr<context> make_context2(int data, int anc) {
    return std::make_unique<context>(data, anc);
}

pset ctx_from_list(const context& c, const val& a) { return c.from_list(strings(a)); }
pset ctx_generated_by(const context& c, const val& a) { return c.generated_by(strings(a)); }
pset ctx_coset_of(const context& c, const std::string& base, const val& a) {
    return c.coset_of(base, strings(a));
}
pset ctx_coset_of_set(const context& c, const std::string& base, const pset& s) {
    return c.coset_of(base, s);
}
pset ctx_identity_on(const context& c, const val& a) { return c.identity_on(ints(a)); }
pset ctx_pauli_at(const context& c, int q, const std::string& p) {
    if (p.size() != 1) throw std::runtime_error("pauli_at: give one of I, X, Y, Z");
    return c.pauli_at(q, p[0]);
}
pset ctx_weight_exactly_on(const context& c, int w, const val& a) {
    return c.weight_exactly(w, ints(a));
}
pset ctx_weight_at_most_on(const context& c, int w, const val& a) {
    return c.weight_at_most(w, ints(a));
}
pset ctx_weight_between_on(const context& c, int lo, int hi, const val& a) {
    return c.weight_between(lo, hi, ints(a));
}
pset ctx_commute_with(const context& c, const val& p) {
    if (p.isString()) return c.commute_with(p.as<std::string>());
    return c.commute_with(strings(p));
}

// --- pset ----------------------------------------------------------------------

pset p_union(const pset& a, const pset& b) { return a | b; }
pset p_intersect(const pset& a, const pset& b) { return a & b; }
pset p_minus(const pset& a, const pset& b) { return a - b; }
pset p_xor(const pset& a, const pset& b) { return a ^ b; }
pset p_complement(const pset& a) { return ~a; }
pset p_mul(const pset& a, const pset& b) { return a * b; }
bool p_equals(const pset& a, const pset& b) { return a == b; }
pset p_clone(const pset& a) { return a; }

void g_x(pset& s, int q) { s.x(q); }
void g_y(pset& s, int q) { s.y(q); }
void g_z(pset& s, int q) { s.z(q); }
void g_h(pset& s, int q) { s.h(q); }
void g_s(pset& s, int q) { s.s(q); }
void g_sdg(pset& s, int q) { s.sdg(q); }
void g_sx(pset& s, int q) { s.sx(q); }
void g_sxdg(pset& s, int q) { s.sxdg(q); }
void g_cx(pset& s, int a, int b) { s.cx(a, b); }
void g_cy(pset& s, int a, int b) { s.cy(a, b); }
void g_cz(pset& s, int a, int b) { s.cz(a, b); }
void g_swap(pset& s, int a, int b) { s.swap(a, b); }
void g_reset(pset& s, const val& qs) { s.reset(ints(qs)); }

pset p_fault_inject(const pset& s, const val& qs) { return s.fault_inject(ints(qs)); }

// measure_z(q) -> {meas_0, meas_1}; measure_z(q, outcome) -> the part with it.
val p_measure(const pset& s, char basis, int q, const val& outcome) {
    const auto pick = [&](bool o) {
        return basis == 'z' ? s.measure_z(q, o) : basis == 'x' ? s.measure_x(q, o)
                                                                : s.measure_y(q, o);
    };
    if (!outcome.isUndefined()) return val(pick(outcome.as<int>() != 0));
    val out = val::object();
    out.set("meas_0", pick(false));
    out.set("meas_1", pick(true));
    return out;
}
val p_measure_z(const pset& s, int q, const val& o) { return p_measure(s, 'z', q, o); }
val p_measure_x(const pset& s, int q, const val& o) { return p_measure(s, 'x', q, o); }
val p_measure_y(const pset& s, int q, const val& o) { return p_measure(s, 'y', q, o); }

// Up to `limit` elements, in the library's order.
val p_elements(const pset& s, int limit) {
    std::vector<std::string> out;
    s.for_each_element([&](const std::string& e) {
        out.push_back(e);
        return static_cast<int>(out.size()) < limit;
    });
    return to_array(out);
}

val p_any_element(const pset& s) {
    const auto e = s.any_element();
    return e ? val(*e) : val::null();
}

double p_hash(const pset& s) { return static_cast<double>(s.hash()); }
double p_node_count(const pset& s) { return static_cast<double>(s.node_count()); }

// --- code ----------------------------------------------------------------------

std::unique_ptr<code> make_code(const context& c, const val& gens) {
    return std::make_unique<code>(c, strings(gens));
}
std::unique_ptr<code> make_code_d(const context& c, const val& gens, int d) {
    return std::make_unique<code>(c, strings(gens), d);
}

val c_stab(const code& c) { return to_array(c.stabilizer_generators()); }
val c_lx(const code& c) { return to_array(c.logical_X_operators()); }
val c_lz(const code& c) { return to_array(c.logical_Z_operators()); }
std::string c_syndrome_of(const code& c, const std::string& e) { return bits(c.syndrome_of(e)); }
std::string c_logical_of(const code& c, const std::string& e) {
    return bits(c.logical_component_of(e));
}
pset c_by_syndrome(const code& c, const std::string& s) { return c.by_syndrome(from_bits(s)); }
pset c_by_logical(const code& c, const std::string& s) {
    return c.by_logical_component(from_bits(s));
}

code::fqcv_method method_of(const std::string& m) {
    if (m == "bruteforce") return code::fqcv_method::bruteforce;
    if (m == "table") return code::fqcv_method::table;
    if (m == "image") return code::fqcv_method::image;
    if (m == "square") return code::fqcv_method::square;
    throw std::runtime_error("unknown fqcv method '" + m +
                             "'; use bruteforce, table, image or square");
}

val c_fqcv(const code& c, const pset& errors, const val& method) {
    const auto m = method.isUndefined() ? code::fqcv_method::image
                                        : method_of(method.as<std::string>());
    const auto v = c.find_qec_cond_violation(errors, m);
    if (!v) return val::null();
    return to_array({v->first, v->second});
}

// A C++ exception that escapes into JS is only a pointer; turn it back into a
// message for the playground to print.
std::string exception_message(std::intptr_t ptr) {
    return reinterpret_cast<std::exception*>(ptr)->what();
}

} // namespace

EMSCRIPTEN_BINDINGS(spbdd) {
    function("exception_message", &exception_message);

    class_<context>("Context")
        .constructor(&make_context)
        .constructor(&make_context2)
        .class_function("alive", &context::alive)
        .function("grow_num_qubits_to", &context::grow_num_qubits_to)
        .function("num_qubits", &context::num_qubits)
        .function("num_data", &context::num_data)
        .function("num_ancillas", &context::num_ancillas)
        .function("num_vars", &context::num_vars)
        .function("set_dynamic_reordering", &context::set_dynamic_reordering)
        .function("dynamic_reordering", &context::dynamic_reordering)
        .function("reorder_now", &context::reorder_now)
        .function("live_nodes", &context::live_nodes)
        .function("peak_nodes", &context::peak_nodes)
        .function("table_size", &context::table_size)
        .function("empty", &context::empty)
        .function("all", &context::all)
        .function("identity", &context::identity)
        .function("from_str", &context::from_str)
        .function("from_list", &ctx_from_list)
        .function("generated_by", &ctx_generated_by)
        .function("coset_of", &ctx_coset_of)
        .function("coset_of_set", &ctx_coset_of_set)
        .function("identity_on", &ctx_identity_on)
        .function("pauli_at", &ctx_pauli_at)
        .function("matching", &context::matching)
        .function("weight_exactly", select_overload<pset(int) const>(&context::weight_exactly))
        .function("weight_at_most", select_overload<pset(int) const>(&context::weight_at_most))
        .function("weight_between",
                  select_overload<pset(int, int) const>(&context::weight_between))
        .function("weight_exactly_on", &ctx_weight_exactly_on)
        .function("weight_at_most_on", &ctx_weight_at_most_on)
        .function("weight_between_on", &ctx_weight_between_on)
        .function("commute_with", &ctx_commute_with);

    class_<pset>("PSet")
        .function("union", &p_union)
        .function("intersect", &p_intersect)
        .function("minus", &p_minus)
        .function("xor", &p_xor)
        .function("complement", &p_complement)
        .function("mul", &p_mul)
        .function("equals", &p_equals)
        .function("clone", &p_clone)
        .function("is_empty", &pset::is_empty)
        .function("contains", &pset::contains)
        .function("subset_of", &pset::subset_of)
        .function("size", &pset::size)
        .function("node_count", &p_node_count)
        .function("hash", &p_hash)
        .function("x", &g_x)
        .function("y", &g_y)
        .function("z", &g_z)
        .function("h", &g_h)
        .function("s", &g_s)
        .function("sdg", &g_sdg)
        .function("sx", &g_sx)
        .function("sxdg", &g_sxdg)
        .function("cx", &g_cx)
        .function("cy", &g_cy)
        .function("cz", &g_cz)
        .function("swap", &g_swap)
        .function("reset", &g_reset)
        .function("fault_inject", &p_fault_inject)
        .function("measure_z", &p_measure_z)
        .function("measure_x", &p_measure_x)
        .function("measure_y", &p_measure_y)
        .function("elements", &p_elements)
        .function("any_element", &p_any_element);

    class_<code>("Code")
        .constructor(&make_code)
        .constructor(&make_code_d)
        .function("n", &code::n)
        .function("k", &code::k)
        .function("r", &code::r)
        .function("d", &code::d)
        .function("stabilizer_generators", &c_stab)
        .function("logical_X_operators", &c_lx)
        .function("logical_Z_operators", &c_lz)
        .function("S", &code::S)
        .function("N", &code::N)
        .function("NmS", &code::NmS)
        .function("syndrome_of", &c_syndrome_of)
        .function("logical_component_of", &c_logical_of)
        .function("by_syndrome", &c_by_syndrome)
        .function("by_logical_component", &c_by_logical)
        .function("find_qec_cond_violation", &c_fqcv);
}
