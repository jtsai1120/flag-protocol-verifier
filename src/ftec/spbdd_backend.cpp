#include "ftec/spbdd_backend.hpp"

#include "ftec/qasm.hpp"

#include <spbdd/spbdd.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ftec {

namespace {

using spbdd::pset;
using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------
// Resident memory, to separate what the final E1E2 query costs from the rest.
//
// The kernel's high-water mark only ever rises, so the growth one call caused
// is invisible once an earlier phase has set a higher peak. Linux can reset it
// (writing 5 to /proc/self/clear_refs), which makes "peak during this call" a
// measurable thing; the overall peak is then kept here instead, as the largest
// value seen just before each reset. Where that file is not writable this
// degrades to the plain getrusage number and the per-call growth reads 0.
// ---------------------------------------------------------------------------

std::size_t status_kib(const char* field) {
    std::ifstream in("/proc/self/status");
    std::string   line;
    const std::string prefix = std::string(field) + ":";
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) == 0) {
            return static_cast<std::size_t>(std::stoull(line.substr(prefix.size())));
        }
    }
    return 0;
}

bool reset_high_water_mark() {
    std::ofstream out("/proc/self/clear_refs");
    if (!out) return false;
    out << "5";
    out.flush();
    return static_cast<bool>(out);
}

// Where each register of a circuit lands in the global qubit numbering; the
// Data qubits are fixed at 0..n-1 and carry the encoded state across the whole protocol;
// ancillas sit above them and are laid out per circuit, which is sound only
// because every step begins and ends with them at identity.
struct Layout {
    std::map<std::string, int> base;     // register name -> first global qubit
    std::vector<int>           ancillas; // every ancilla qubit, for the resets
    int                        total = 0;
};

std::string bits_to_string(const std::vector<bool>& bits) {
    std::string out;
    out.reserve(bits.size());
    for (const bool bit : bits) out += bit ? '1' : '0';
    return out;
}

struct FqcvName {
    const char*                      name;
    spbdd::code::fqcv_method         method;
};

constexpr FqcvName kFqcv[] = {
    {"bruteforce", spbdd::code::fqcv_method::bruteforce},
    {"table",      spbdd::code::fqcv_method::table},
    {"image",      spbdd::code::fqcv_method::image},
    {"square",     spbdd::code::fqcv_method::square},
};

spbdd::code::fqcv_method fqcv_method_of(const std::string& name) {
    for (const auto& entry : kFqcv) {
        if (name == entry.name) return entry.method;
    }
    throw std::runtime_error("spbdd backend: unknown fqcv method '" + name + "'");
}

// What the final E1E2 queries cost, summed over every one that was actually
// run (a repeat of a state already checked is a cache hit and costs nothing).
struct FqcvStats {
    std::size_t calls = 0;
    double      seconds = 0, max_seconds = 0;

    // The diagrams handed to the query, one per non-empty fault level.
    std::size_t sets = 0;
    std::size_t max_set_nodes = 0, sum_set_nodes = 0;
    double      max_set_size = 0;

    // Largest growth of the process's resident set during a single call.
    std::size_t max_rss_growth = 0;

    // square only: N(S)-S, built once per code, and E*E, built once per set.
    // These are measured on the side, outside the timed call.
    bool        has_nms = false;
    double      nms_seconds = 0;
    std::size_t nms_nodes = 0;
    double      nms_size = 0;
    std::size_t sq_sets = 0, max_sq_nodes = 0, sum_sq_nodes = 0;
    double      sq_seconds = 0, max_sq_seconds = 0;

    // Time spent on measurements like the above, so it can be taken back out
    // of the traversal time.
    double      diagnostics_seconds = 0;
};

class SpbddBackend : public Backend {
public:
    explicit SpbddBackend(SpbddOptions options)
        : options_(std::move(options)), method_(fqcv_method_of(options_.fqcv)) {
        tracking_ = reset_high_water_mark();
    }

    ~SpbddBackend() override { release(); }

    void begin(const fpdl::CodeSpec& code, int tau) override {
        if (code.generators.empty()) {
            throw std::runtime_error(
                "spbdd backend: the protocol declares no stabilizer generators");
        }
        release();
        data_qubits_ = code.n;
        tau_         = tau;
        generators_  = code.generators;

        // Levels (r, s): input weight r, s circuit faults, r + s <= tau; in
        // order of r + s, then r. A fault moves (r, s) to (r, s + 1).
        levels_.clear();
        for (int k = 0; k <= tau_; ++k) {
            for (int r = 0; r <= (options_.input_errors ? k : 0); ++r) levels_.push_back({r, k - r});
        }
        succ_.assign(levels_.size(), -1);
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            for (std::size_t j = 0; j < levels_.size(); ++j) {
                if (levels_[j].first == levels_[i].first && levels_[j].second == levels_[i].second + 1) {
                    succ_[i] = static_cast<int>(j);
                }
            }
        }

        // Dynamic reordering stays off: it never paid on any protocol large
        // enough to time.
        spbdd::config config;
        config.dynamic_reordering = false;
        // BuDDy's operation cache defaults to 2^16 entries in spbdd. That is
        // far too small once the Cond 3 bound within(s) grows past a few
        // hundred thousand nodes: building within(2) for CB18 [[45,1,5]]
        // (2.8 million nodes) did not finish in 25 minutes with 2^16 entries,
        // and takes 103 s with 2^20 (77 s with 2^22, at twice the memory).
        config.cache_size = 1 << 20;
        ctx_.emplace(data_qubits_, 0, config);
        allocated_ = data_qubits_;

        // spbdd::code does the one-off linear algebra and says so clearly if
        // the generators are not a stabilizer group.
        rebuild_code();

        intern(fresh_state());
    }

    StateId initial_state() override { return 0; }

    std::vector<std::pair<Outcome, StateId>> step(StateId id,
                                                  const CircuitRef& circuit) override {
        ++step_calls_;
        // Two different measurement records often leave the same set of errors
        // behind -- the record is what the decoder saw, not what survived --
        // and stepping depends only on the set. States are interned, so equal
        // sets have equal ids and this key is exact.
        const CacheKey key{id, circuit.qasm.string()};
        if (const auto found = step_cache_.find(key); found != step_cache_.end()) {
            ++step_hits_;
            return found->second;
        }

        const QasmProgram& program = program_for(circuit);
        const Layout       layout  = layout_for(program, circuit);
        const Records&     records = records_for(circuit, program);
        grow_to(layout.total);

        auto branches = simulate(reset_ancillas(states_[id], layout), program, layout, succ_);

        std::vector<std::pair<Outcome, StateId>> out;
        for (auto& branch : branches) {
            // The ancillas are done; their outcome is recorded, and anything
            // left on them cannot influence the next circuit.
            State cleaned;
            cleaned.reserve(branch.by_t.size());
            for (auto& level : branch.by_t) cleaned.push_back(level.reset(layout.ancillas));
            if (all_empty(cleaned)) continue;

            Outcome outcome;
            outcome.syndrome = read_register(branch.bits, records.syndrome, circuit);
            if (records.flag) outcome.flag = read_register(branch.bits, *records.flag, circuit);
            out.emplace_back(std::move(outcome), intern(std::move(cleaned)));
        }
        step_cache_.emplace(key, out);
        return out;
    }

    std::optional<Failure> check(StateId id) override {
        ++check_calls_;
        if (const auto found = check_cache_.find(id); found != check_cache_.end()) {
            ++check_hits_;
            return found->second;
        }
        const auto answer = check_uncached(id);
        check_cache_.emplace(id, answer);
        return answer;
    }

    TerminalFacts terminal_facts(StateId id) override {
        ++validity_.facts_calls;
        if (const auto found = facts_cache_.find(id); found != facts_cache_.end()) {
            ++validity_.facts_hits;
            return found->second;
        }
        const Measured measured(*this, validity_.facts_seconds);
        const State& state = states_[id];
        TerminalFacts facts;

        pset all = ctx_->empty();
        for (const auto& level : state) all = all | level;

        // Condition 1: no generator splits the residual errors.
        facts.same_syndrome = true;
        for (const auto& g : generators_) {
            const pset commuting = all & ctx_->commute_with(padded(g));
            if (!commuting.is_empty() && commuting != all) {
                facts.same_syndrome   = false;
                facts.syndrome_detail = on_data(commuting.any_element().value_or("")) + " and " +
                                        on_data((all - commuting).any_element().value_or("")) +
                                        " differ on " + g;
                break;
            }
        }

        // Condition 3: (r, s) errors are within weight s of the code space.
        facts.weight_bounded = true;
        for (std::size_t i = 0; i < state.size(); ++i) {
            const int s = levels_[i].second;
            const pset outside = outside_of(state[i], within(s));
            if (outside.is_empty()) continue;
            facts.weight_bounded = false;
            facts.weight_detail = on_data(outside.any_element().value_or("")) + " (input weight " +
                           std::to_string(levels_[i].first) + ", " + std::to_string(s) +
                           " fault(s)) has minimum weight above " + std::to_string(s);
            break;
        }
        facts_cache_.emplace(id, facts);
        return facts;
    }

    std::optional<std::string> flag_condition(const CircuitRef& circuit, int k) override {
        const auto key = std::make_pair(circuit.qasm.string(), k);
        ++validity_.flag_calls;
        if (const auto found = flag_cache_.find(key); found != flag_cache_.end()) {
            ++validity_.flag_hits;
            return found->second;
        }
        const Measured measured(*this, validity_.flag_seconds);

        const QasmProgram& program = program_for(circuit);
        const Layout       layout  = layout_for(program, circuit);
        const Records&     records = records_for(circuit, program);
        grow_to(layout.total);

        // Levels 0..k of faults in this circuit alone, no input error.
        State start{ctx_->identity()};
        std::vector<int> succ;
        for (int s = 1; s <= k; ++s) {
            start.push_back(ctx_->empty());
            succ.push_back(s);
        }
        succ.push_back(-1);

        std::optional<std::string> problem;
        for (auto& branch : simulate(std::move(start), program, layout, succ)) {
            if (records.flag) {
                const auto flags = read_register(branch.bits, *records.flag, circuit);
                if (std::find(flags.begin(), flags.end(), true) != flags.end()) continue;
            }
            for (int s = 1; s <= k && !problem; ++s) {
                pset level = branch.by_t[static_cast<std::size_t>(s)];
                level.reset(layout.ancillas);
                const pset outside = outside_of(level, within(s));
                if (outside.is_empty()) continue;
                problem = std::to_string(s) + " fault(s) raise no flag but leave " +
                          on_data(outside.any_element().value_or("")) +
                          ", of minimum weight above " + std::to_string(s);
            }
            if (problem) break;
        }
        flag_cache_.emplace(key, problem);
        return problem;
    }

    std::string describe(StateId id) const override {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0);
        for (std::size_t i = 0; i < states_[id].size(); ++i) {
            if (i != 0) out << ' ';
            out << 'r' << levels_[i].first << 's' << levels_[i].second << '=' << states_[id][i].size();
        }
        return out.str();
    }

    std::size_t peak_memory_bytes() const override {
        if (!tracking_) return 0;
        return std::max(peak_rss_bytes_, status_kib("VmHWM") * 1024);
    }

    // One "key : value" per line so that tools/bench_fqcv.py can read it back;
    // the labels are part of that contract.
    std::string statistics() const override {
        std::ostringstream out;
        out << std::fixed;
        const auto  seconds = [&](double s) { out << std::setprecision(4) << s << " s"; };
        const auto  count   = [&](double c) { out << std::setprecision(0) << c; };
        const auto  kib     = [&](std::size_t bytes) {
            out << std::setprecision(1) << static_cast<double>(bytes) / 1024.0 << " KiB";
        };

        out << "spbdd backend   : " << step_calls_ << " step(s), " << step_hits_
            << " from cache (" << percent(step_hits_, step_calls_) << "%); " << check_calls_
            << " check(s), " << check_hits_ << " from cache ("
            << percent(check_hits_, check_calls_) << "%)\n"
            << "distinct states : " << states_.size() << " interned, " << check_cache_.size()
            << " checked, " << step_cache_.size() << " stepped\n";

        out << "fqcv method     : " << options_.fqcv << '\n'
            << "fqcv calls      : " << fqcv_.calls << '\n'
            << "fqcv time       : ";
        seconds(fqcv_.seconds);
        out << "\nfqcv max call   : ";
        seconds(fqcv_.max_seconds);
        out << "\nfqcv sets       : " << fqcv_.sets << '\n'
            << "fqcv set nodes  : max " << fqcv_.max_set_nodes << ", sum " << fqcv_.sum_set_nodes
            << '\n'
            << "fqcv set size   : max ";
        count(fqcv_.max_set_size);
        out << "\nfqcv rss growth : ";
        kib(fqcv_.max_rss_growth * 1024);
        out << '\n';

        if (fqcv_.has_nms) {
            out << "nms build time  : ";
            seconds(fqcv_.nms_seconds);
            out << "\nnms nodes       : " << fqcv_.nms_nodes << "\nnms size        : ";
            count(fqcv_.nms_size);
            out << "\nsq sets         : " << fqcv_.sq_sets << "\nsq nodes        : max "
                << fqcv_.max_sq_nodes << ", sum " << fqcv_.sum_sq_nodes
                << "\nsq build time   : total ";
            seconds(fqcv_.sq_seconds);
            out << ", max ";
            seconds(fqcv_.max_sq_seconds);
            out << '\n';
        }

        out << "validity facts  : " << validity_.facts_calls << " call(s), " << validity_.facts_hits
            << " from cache, ";
        seconds(validity_.facts_seconds);
        out << "\nflag condition  : " << validity_.flag_calls << " call(s), " << validity_.flag_hits
            << " from cache, ";
        seconds(validity_.flag_seconds);
        out << "\nvalidity backend: ";
        seconds(validity_.facts_seconds + validity_.flag_seconds);
        out << "\nvalidity rss    : max growth in one call ";
        kib(validity_.max_rss_growth_kib * 1024);
        out << '\n';

        out << "diagnostics     : ";
        seconds(fqcv_.diagnostics_seconds);
        out << '\n';
        if (ctx_) {
            out << "diagram         : " << ctx_->live_nodes() << " live node(s), peak "
                << ctx_->peak_nodes() << ", table " << ctx_->table_size() << ", "
                << ctx_->num_qubits() << " qubits";
        }
        return out.str();
    }

private:
    using State    = std::vector<pset>;
    using CacheKey = std::pair<StateId, std::string>;

    // Protocol validity work in the backend (Cond 1 / Cond 3 and the flag
    // condition), timed apart from correctability, with its memory growth
    // measured the way run_fqcv measures the final query.
    struct ValidityStats {
        std::size_t facts_calls = 0, facts_hits = 0, flag_calls = 0, flag_hits = 0;
        double      facts_seconds = 0, flag_seconds = 0;
        std::size_t max_rss_growth_kib = 0;
    };

    class Measured {
    public:
        Measured(SpbddBackend& backend, double& seconds) : backend_(backend), seconds_(seconds) {
            if (backend_.tracking_) {
                backend_.peak_rss_bytes_ =
                    std::max(backend_.peak_rss_bytes_, status_kib("VmHWM") * 1024);
                reset_high_water_mark();
            }
            rss_before_ = status_kib("VmRSS");
            start_      = Clock::now();
        }
        ~Measured() {
            seconds_ += seconds_since(start_);
            const std::size_t high = status_kib("VmHWM");
            if (backend_.tracking_) {
                backend_.peak_rss_bytes_ = std::max(backend_.peak_rss_bytes_, high * 1024);
            }
            auto& growth = backend_.validity_.max_rss_growth_kib;
            growth = std::max(growth, high > rss_before_ ? high - rss_before_ : 0);
        }
        Measured(const Measured&)            = delete;
        Measured& operator=(const Measured&) = delete;

    private:
        SpbddBackend&     backend_;
        double&           seconds_;
        std::size_t       rss_before_ = 0;
        Clock::time_point start_;
    };

    // One classical outcome so far, and the error sets that can accompany it.
    struct Branch {
        State                                    by_t;
        std::map<std::string, std::vector<bool>> bits;
    };

    static int percent(std::size_t part, std::size_t whole) {
        return whole == 0 ? 0 : static_cast<int>(100.0 * static_cast<double>(part) /
                                                 static_cast<double>(whole));
    }

    void release() {
        step_cache_.clear();
        check_cache_.clear();
        facts_cache_.clear();
        flag_cache_.clear();
        ws_.clear();
        buckets_.clear();
        programs_.clear();
        records_.clear();
        states_.clear();
        code_.reset();
        ctx_.reset();
        allocated_ = 0;
    }

    // The code is stated over the data qubits and the sets live over the data
    // qubits *and* the ancillas. spbdd::code takes the generators on the data
    // qubits and treats the ancillas as free; every set this is ever asked
    // about has its ancillas back at identity, so what the query decides is
    // exactly the data-qubit question. The code snapshots the qubit count, so
    // it is rebuilt whenever the context grows.
    void rebuild_code() {
        code_ = std::make_unique<spbdd::code>(*ctx_, generators_);
        nms_measured_ = false;
        ws_.clear();
    }

    std::string padded(const std::string& pauli) const {
        return pauli + std::string(static_cast<std::size_t>(ctx_->num_qubits()) - pauli.size(), 'I');
    }

    std::string on_data(const std::string& pauli) const {
        return pauli.substr(0, static_cast<std::size_t>(data_qubits_));
    }

    // The errors of `set` that are not in `bound`. Written as set - (set & bound)
    // rather than set - bound: bound (within(s)) is far larger than set, and a
    // plain difference has to build the complement of bound's subgraphs every
    // time set reaches its 1-leaf. With BuDDy's 2^16-entry operation cache
    // that is recomputed over and over: on CB18 [[35,1,5]] the first Cond 3
    // check (4,395 nodes minus 387,079) did not finish in 18 hours. The
    // intersection only walks set, and the difference is then between two
    // small diagrams.
    static pset outside_of(const pset& set, const pset& bound) { return set - (set & bound); }

    // Errors within weight s of the code space: (weight <= s on the data) * S.
    const pset& within(int s) {
        while (static_cast<int>(ws_.size()) <= s) {
            std::vector<int> data(static_cast<std::size_t>(data_qubits_));
            for (int q = 0; q < data_qubits_; ++q) data[static_cast<std::size_t>(q)] = q;
            ws_.push_back(ctx_->weight_at_most(static_cast<int>(ws_.size()), data) * code_->S());
        }
        return ws_[static_cast<std::size_t>(s)];
    }

    // Level (r, 0) holds every data error of weight at most r (only the data
    // qubits exist yet); fault levels start empty.
    State fresh_state() const {
        State state;
        state.reserve(levels_.size());
        for (const auto& [r, s] : levels_) {
            if (s != 0) state.push_back(ctx_->empty());
            else state.push_back(r == 0 ? ctx_->identity() : ctx_->weight_at_most(r));
        }
        return state;
    }

    static bool all_empty(const State& state) {
        for (const auto& level : state) {
            if (!level.is_empty()) return false;
        }
        return true;
    }

    // States are interned: the same family of sets always gets the same id,
    // which is what keys the step and check caches. pset compares in constant
    // time (the diagrams are canonical), and pset::hash() is the root node. It used to be the number of elements of
    // each level instead, but those collide massively -- 155k states fell into
    // 759 buckets on Du24 [[17,1,5]] -- and the bucket scans took up to 60% of
    // the traversal.
    StateId intern(State state) {
        std::size_t key = 0;
        for (const auto& level : state) {
            key = key * 1000003u ^ level.hash();
        }
        auto& bucket = buckets_[key];
        for (const StateId id : bucket) {
            if (same(states_[id], state)) return id;
        }
        states_.push_back(std::move(state));
        bucket.push_back(states_.size() - 1);
        return states_.size() - 1;
    }

    static bool same(const State& a, const State& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != b[i]) return false;
        }
        return true;
    }

    void grow_to(int qubits) {
        if (qubits <= allocated_) return;
        ctx_->grow_num_qubits_to(qubits);
        // Growing leaves the new variables unconstrained, so every set would
        // silently gain 4^added configurations on them. Pin them at once.
        // Pinning does not merge or split states, so ids and caches stay valid.
        std::vector<int> added;
        for (int q = allocated_; q < qubits; ++q) added.push_back(q);
        for (auto& state : states_) {
            for (auto& level : state) level.reset(added);
        }
        allocated_ = qubits;
        // The generators have to be restated over the wider space.
        rebuild_code();
    }

    const QasmProgram& program_for(const CircuitRef& circuit) {
        const std::string key = circuit.qasm.string();
        if (const auto found = programs_.find(key); found != programs_.end()) {
            return found->second;
        }
        return programs_.emplace(key, parse_qasm_file(circuit.qasm)).first->second;
    }

    Layout layout_for(const QasmProgram& program, const CircuitRef& circuit) const {
        const auto width = [&](const std::string& name, const char* role) {
            if (!program.has_qubit_register(name)) {
                throw std::runtime_error("spbdd backend: " + circuit.qasm.string() + " has no " +
                                         role + " register '" + name + "'");
            }
            return static_cast<int>(program.qubit_width(name));
        };

        Layout layout;
        const int data = width(circuit.data_qubits, "qd");
        if (data != data_qubits_) {
            throw std::runtime_error("spbdd backend: " + circuit.qasm.string() + " declares " +
                                     std::to_string(data) + " data qubits but the code has " +
                                     std::to_string(data_qubits_));
        }
        layout.base[circuit.data_qubits] = 0;
        int next = data_qubits_;

        const auto place = [&](const std::string& name, const char* role) {
            const int n = width(name, role);
            layout.base[name] = next;
            for (int i = 0; i < n; ++i) layout.ancillas.push_back(next + i);
            next += n;
        };
        place(circuit.syndrome_qubits, "qm");
        if (circuit.flag_qubits) place(*circuit.flag_qubits, "qf");

        layout.total = next;
        return layout;
    }

    static int qubit_of(const QubitRef& ref, const Layout& layout) {
        const auto found = layout.base.find(ref.reg);
        if (found == layout.base.end()) {
            throw std::runtime_error("spbdd backend: register '" + ref.reg +
                                     "' has no role in this circuit; the protocol names only "
                                     "qd, qm and qf");
        }
        return found->second + static_cast<int>(ref.index);
    }

    static State reset_ancillas(const State& state, const Layout& layout) {
        State out;
        out.reserve(state.size());
        for (auto level : state) out.push_back(level.reset(layout.ancillas));
        return out;
    }

    // Run one circuit from `start`. One branch per distinct classical outcome
    // so far; branches whose bits agree are the same observation and are
    // unioned, which is the same merge the driver does between nodes, applied
    // within a circuit. `succ[i]` is the level a fault moves level i to.
    std::vector<Branch> simulate(State start, const QasmProgram& program, const Layout& layout,
                                 const std::vector<int>& succ) const {
        std::vector<Branch> branches;
        branches.push_back(Branch{std::move(start), {}});
        for (const auto& reg : program.bits) {
            branches.front().bits.emplace(reg.name, std::vector<bool>(reg.width, false));
        }
        for (const auto& instruction : program.instructions) {
            switch (instruction.kind) {
                case QasmInstruction::Kind::Barrier:
                    break;
                case QasmInstruction::Kind::Gate:
                    for (auto& branch : branches) apply_gate(branch.by_t, instruction, layout, succ);
                    break;
                case QasmInstruction::Kind::Reset: {
                    const std::vector<int> target{qubit_of(instruction.qubits[0], layout)};
                    for (auto& branch : branches) {
                        for (auto& level : branch.by_t) level.reset(target);
                    }
                    break;
                }
                case QasmInstruction::Kind::Measure:
                    branches = split_on_measurement(branches, instruction, layout);
                    break;
            }
        }
        return branches;
    }

    void apply_gate(State& by_t, const QasmInstruction& instruction,
                    const Layout& layout, const std::vector<int>& succ) const {
        const std::string& gate = instruction.gate;
        if (gate == "i" || gate == "id") return;

        if (instruction.qubits.size() == 1) {
            const int q = qubit_of(instruction.qubits[0], layout);
            for (auto& level : by_t) {
                // x, y and z conjugate to themselves up to a phase, so they
                // leave the error set alone; the library says so itself.
                if (gate == "x") level.x(q);
                else if (gate == "y") level.y(q);
                else if (gate == "z") level.z(q);
                else if (gate == "h") level.h(q);
                else if (gate == "s" || gate == "sdg") level.s(q);
                else throw std::runtime_error("spbdd backend: unsupported gate '" + gate + "'");
            }
            return;
        }

        const int control = qubit_of(instruction.qubits[0], layout);
        const int target  = qubit_of(instruction.qubits[1], layout);
        for (auto& level : by_t) {
            if (gate == "cx") level.cx(control, target);
            else if (gate == "cy") level.cy(control, target);
            else if (gate == "cz") level.cz(control, target);
            else throw std::runtime_error("spbdd backend: unsupported gate '" + gate + "'");
        }

        // Every two-qubit gate is a fault location. Single-qubit gates are
        // taken to be fault free, which is the model the protocols assume.
        //
        // All spawns read the post-gate state before any of them is merged in,
        // so one gate adds at most one fault to any lineage; feeding the merged
        // result back would let a single location fault twice.
        const std::vector<int> location{control, target};
        std::vector<std::pair<int, pset>> spawn;
        for (std::size_t i = 0; i < by_t.size(); ++i) {
            if (succ[i] >= 0 && !by_t[i].is_empty()) spawn.emplace_back(succ[i], by_t[i].fault_inject(location));
        }
        for (auto& [to, set] : spawn) {
            by_t[static_cast<std::size_t>(to)] = by_t[static_cast<std::size_t>(to)] | set;
        }
    }

    // Measuring splits the set rather than changing it, and which half an
    // error lands in is the outcome the decoder reads. An empty half is an
    // outcome that cannot occur under these errors.
    std::vector<Branch> split_on_measurement(std::vector<Branch>& branches,
                                             const QasmInstruction& instruction,
                                             const Layout& layout) const {
        const int q = qubit_of(instruction.qubits[0], layout);

        // Keyed by the classical bits so far: two branches that agree on
        // everything measured are indistinguishable and must be unioned.
        std::map<std::string, Branch> merged;
        for (auto& branch : branches) {
            Branch outcome[2] = {branch, branch};
            bool   empty[2]   = {true, true};
            for (std::size_t t = 0; t < branch.by_t.size(); ++t) {
                const auto split = branch.by_t[t].measure_z(q);
                outcome[0].by_t[t] = split.meas_0;
                outcome[1].by_t[t] = split.meas_1;
                empty[0] = empty[0] && split.meas_0.is_empty();
                empty[1] = empty[1] && split.meas_1.is_empty();
            }

            for (const int bit : {0, 1}) {
                if (empty[bit]) continue;
                Branch& next = outcome[bit];
                next.bits[instruction.target.reg][instruction.target.index] = bit != 0;

                const std::string key = signature(next.bits);
                if (const auto found = merged.find(key); found != merged.end()) {
                    for (std::size_t t = 0; t < next.by_t.size(); ++t) {
                        found->second.by_t[t] = found->second.by_t[t] | next.by_t[t];
                    }
                } else {
                    merged.emplace(key, std::move(next));
                }
            }
        }

        std::vector<Branch> out;
        out.reserve(merged.size());
        for (auto& [key, branch] : merged) out.push_back(std::move(branch));
        return out;
    }

    static std::string signature(const std::map<std::string, std::vector<bool>>& bits) {
        std::string key;
        for (const auto& [name, values] : bits) {
            key += name;
            key += ':';
            for (const bool bit : values) key += bit ? '1' : '0';
            key += ';';
        }
        return key;
    }

    // N(S)-S, built the way the library builds it for the "square" method
    // (which builds it once per code and then reuses it). Measured once per
    // code, on the side, with a fresh build rather than the cached copy.
    void measure_nms() {
        if (nms_measured_) return;
        const auto start = Clock::now();
        const pset nms   = code_->N() - code_->S();
        const double sec = seconds_since(start);
        fqcv_.has_nms     = true;
        fqcv_.nms_seconds = sec;
        fqcv_.nms_nodes   = nms.node_count();
        fqcv_.nms_size    = nms.size();
        fqcv_.diagnostics_seconds += sec;
        nms_measured_ = true;
    }

    // The final E1E2 query on one fault level, timed, with its memory growth
    // and the size of what it was given recorded. Everything that is only
    // measurement happens outside the timed region.
    spbdd::code::violation run_fqcv(const pset& errors) {
        const auto side_start = Clock::now();
        ++fqcv_.sets;
        const std::size_t nodes = errors.node_count();
        fqcv_.max_set_nodes = std::max(fqcv_.max_set_nodes, nodes);
        fqcv_.sum_set_nodes += nodes;
        fqcv_.max_set_size = std::max(fqcv_.max_set_size, errors.size());
        fqcv_.diagnostics_seconds += seconds_since(side_start);

        if (tracking_) {
            peak_rss_bytes_ = std::max(peak_rss_bytes_, status_kib("VmHWM") * 1024);
            reset_high_water_mark();
        }
        const std::size_t rss_before = status_kib("VmRSS");

        const auto start = Clock::now();
        auto       found = code_->find_qec_cond_violation(errors, method_);
        const double sec = seconds_since(start);

        const std::size_t high = status_kib("VmHWM");
        if (tracking_) peak_rss_bytes_ = std::max(peak_rss_bytes_, high * 1024);
        fqcv_.max_rss_growth = std::max(fqcv_.max_rss_growth, high > rss_before ? high - rss_before : 0);

        ++fqcv_.calls;
        fqcv_.seconds += sec;
        fqcv_.max_seconds = std::max(fqcv_.max_seconds, sec);

        // square only: the sizes of N(S)-S and E*E, measured after the timed
        // call so that they do not warm BuDDy's caches for it.
        if (method_ == spbdd::code::fqcv_method::square) {
            const auto diag_start = Clock::now();
            measure_nms();
            const auto sq_start = Clock::now();
            const pset square   = errors * errors;
            const double sq_sec = seconds_since(sq_start);
            const std::size_t n = square.node_count();
            ++fqcv_.sq_sets;
            fqcv_.max_sq_nodes = std::max(fqcv_.max_sq_nodes, n);
            fqcv_.sum_sq_nodes += n;
            fqcv_.sq_seconds += sq_sec;
            fqcv_.max_sq_seconds = std::max(fqcv_.max_sq_seconds, sq_sec);
            fqcv_.diagnostics_seconds += seconds_since(diag_start);
        }
        return found;
    }

    // The residual errors of every execution with at most k faults in all,
    // input weight included: E_m of Def. 3.8 for fault bound k.
    std::vector<pset> unions_by_budget(const State& state) const {
        std::vector<pset> u(static_cast<std::size_t>(tau_) + 1, ctx_->empty());
        for (std::size_t i = 0; i < state.size(); ++i) {
            const int k = levels_[i].first + levels_[i].second;
            for (int j = k; j <= tau_; ++j) u[static_cast<std::size_t>(j)] = u[static_cast<std::size_t>(j)] | state[i];
        }
        return u;
    }

    std::optional<Failure> check_uncached(StateId id) {
        const auto unions = unions_by_budget(states_[id]);
        // The full budget first; only on a violation look for the smallest one.
        if (unions.back().is_empty() || !run_fqcv(unions.back())) return std::nullopt;
        for (std::size_t t = 0; t < unions.size(); ++t) {
            if (unions[t].is_empty()) continue;
            const auto pair = run_fqcv(unions[t]);
            if (!pair) continue;

            // The witnesses run over the whole space; their ancilla half is
            // identity by construction, so only the data prefix says anything.
            std::ostringstream detail;
            detail << on_data(pair->first) << " and " << on_data(pair->second)
                   << " share syndrome " << bits_to_string(code_->syndrome_of(pair->first))
                   << " but differ logically ("
                   << bits_to_string(code_->logical_component_of(pair->first)) << " vs "
                   << bits_to_string(code_->logical_component_of(pair->second)) << ")";
            return Failure{static_cast<int>(t), detail.str()};
        }
        return std::nullopt;
    }

    // Which classical register each role's outcome lands in. The protocol does
    // not say -- and does not need to: a measurement statement already binds a
    // classical register to the qubit it reads, so naming the syndrome and flag
    // ancillas fixes the answer.
    struct Records {
        std::string                syndrome;
        std::optional<std::string> flag;
    };

    const Records& records_for(const CircuitRef& circuit, const QasmProgram& program) {
        const std::string key = circuit.qasm.string();
        if (const auto found = records_.find(key); found != records_.end()) return found->second;

        const auto target_of = [&](const std::string& qubits, const char* role) {
            std::optional<std::string> found;
            for (const auto& instruction : program.instructions) {
                if (instruction.kind != QasmInstruction::Kind::Measure) continue;
                if (instruction.qubits.at(0).reg != qubits) continue;
                if (found && *found != instruction.target.reg) {
                    throw std::runtime_error("spbdd backend: " + circuit.qasm.string() +
                                             " measures " + role + " register '" + qubits +
                                             "' into both '" + *found + "' and '" +
                                             instruction.target.reg +
                                             "'; a role must land in one register");
                }
                found = instruction.target.reg;
            }
            return found;
        };

        Records records;
        const auto syndrome = target_of(circuit.syndrome_qubits, "qm");
        if (!syndrome) {
            throw std::runtime_error("spbdd backend: " + circuit.qasm.string() +
                                     " never measures its qm register '" +
                                     circuit.syndrome_qubits + "', so there is no syndrome");
        }
        records.syndrome = *syndrome;
        if (circuit.flag_qubits) {
            records.flag = target_of(*circuit.flag_qubits, "qf");
            if (!records.flag) {
                throw std::runtime_error("spbdd backend: " + circuit.qasm.string() +
                                         " never measures its qf register '" +
                                         *circuit.flag_qubits + "', so there is no flag");
            }
        }
        return records_.emplace(key, std::move(records)).first->second;
    }

    static std::vector<bool> read_register(
        const std::map<std::string, std::vector<bool>>& bits, const std::string& name,
        const CircuitRef& circuit) {
        const auto found = bits.find(name);
        if (found == bits.end()) {
            throw std::runtime_error("spbdd backend: " + circuit.qasm.string() +
                                     " declares no classical register '" + name + "'");
        }
        return found->second;
    }

    SpbddOptions             options_;
    spbdd::code::fqcv_method method_;
    int                      data_qubits_ = 0;
    int                      tau_         = 0;
    int                      allocated_   = 0;

    std::map<CacheKey, std::vector<std::pair<Outcome, StateId>>> step_cache_;
    std::map<StateId, std::optional<Failure>>                    check_cache_;
    std::size_t step_calls_ = 0, step_hits_ = 0, check_calls_ = 0, check_hits_ = 0;

    FqcvStats     fqcv_;
    ValidityStats validity_;
    bool        nms_measured_   = false;
    bool        tracking_       = false;
    std::size_t peak_rss_bytes_ = 0;

    std::vector<std::pair<int, int>>               levels_;   // (r, s) per level
    std::vector<int>                               succ_;     // level after one more fault, or -1
    std::vector<pset>                              ws_;       // ws_[s] = (weight <= s on data) * S
    std::map<StateId, TerminalFacts>               facts_cache_;
    std::map<std::pair<std::string, int>, std::optional<std::string>> flag_cache_;
    std::vector<std::string>                       generators_;
    std::optional<spbdd::context>                  ctx_;
    std::unique_ptr<spbdd::code>                   code_;
    std::map<std::string, QasmProgram>             programs_;
    std::map<std::string, Records>                 records_;
    std::vector<State>                             states_;
    std::unordered_map<std::size_t, std::vector<StateId>> buckets_;
};

} // namespace

std::vector<std::string> spbdd_fqcv_methods() {
    std::vector<std::string> names;
    for (const auto& entry : kFqcv) names.emplace_back(entry.name);
    return names;
}

bool is_spbdd_fqcv_method(const std::string& name) {
    for (const auto& entry : kFqcv) {
        if (name == entry.name) return true;
    }
    return false;
}

std::unique_ptr<Backend> make_spbdd_backend(const SpbddOptions& options) {
    return std::make_unique<SpbddBackend>(options);
}

} // namespace ftec
