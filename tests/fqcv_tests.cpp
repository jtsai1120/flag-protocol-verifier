// End-to-end check of the verifier on the small (distance-3) protocols.
//
// Each protocol is verified once per fqcv method. The four methods answer the
// same question, so they must agree with each other on everything the verdict
// is made of, and with the expected summary recorded in tests/expected.txt.
//
// What is *not* compared is Failure::detail: it names a witness pair, and a
// set can hold several, so which one comes back is the library's business. The
// fault count it comes back with is not -- that is the answer.
//
// usage: fqcv_tests <expected.txt> <protocol.fpdl>...
//        fqcv_tests --print <protocol.fpdl>...   (regenerate expected.txt)

#include "ftec/dag.hpp"
#include "ftec/spbdd_backend.hpp"
#include "ftec/verify.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "  FAILED: " << what << '\n';
    ++failures;
}

// The same doubling the CLI does: a protocol that has not finished expanding
// is silently incomplete, so raise the bound until the parser stops truncating.
fpdl::ParseResult expand(const std::filesystem::path& source) {
    fpdl::ParseOptions options;
    options.max_paths = 5000;
    for (std::size_t bound = 16; bound <= (1u << 16); bound *= 2) {
        options.bmc_bound = bound;
        auto parsed = fpdl::Parser::parse_file(source, options);
        bool any_path_bounded = false;
        for (const auto& path : parsed.paths) any_path_bounded |= path.bound_exceeded;
        if (!parsed.truncated && !any_path_bounded) return parsed;
    }
    throw std::runtime_error("symbolic expansion did not converge");
}

// Everything the methods have to agree on, on one line.
std::string summary(const ftec::VerifyResult& result) {
    std::ostringstream out;
    out << "paths=" << result.paths_reached << " records=" << result.records_reached
        << " circuits=" << result.se_applications << " min_t=" << result.min_fault_count
        << " failures=" << result.failures.size();
    for (const auto& failure : result.failures) {
        out << " [path " << failure.path_id << " t=" << failure.failure.fault_count
            << " record=" << failure.record_string() << ']';
    }
    return out.str();
}

// "<protocol directory name>\t<summary>" per line; '#' starts a comment.
std::map<std::string, std::string> read_expected(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) throw std::runtime_error("cannot read " + file.string());
    std::map<std::string, std::string> expected;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        expected[line.substr(0, tab)] = line.substr(tab + 1);
    }
    return expected;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <expected.txt> <protocol.fpdl>...\n"
                  << "       " << argv[0] << " --print <protocol.fpdl>...\n";
        return 2;
    }
    const bool print = std::string(argv[1]) == "--print";
    std::map<std::string, std::string> expected;
    if (!print) expected = read_expected(argv[1]);

    for (int i = 2; i < argc; ++i) {
        const std::filesystem::path source = argv[i];
        const std::string name = source.parent_path().filename().string();
        if (!print) std::cout << name << '\n';

        try {
            const auto parsed = expand(source);
            const auto dag    = ftec::build_dag(parsed, source.parent_path());

            std::string first;
            for (const std::string& method : ftec::spbdd_fqcv_methods()) {
                ftec::SpbddOptions options;
                options.fqcv = method;
                auto backend = ftec::make_spbdd_backend(options);
                const std::string got = summary(ftec::verify(dag, *backend));
                if (print) {
                    if (first.empty()) std::cout << name << '\t' << got << '\n';
                    first = got;
                    continue;
                }
                if (first.empty()) first = got;
                check(got == first, name + ": " + method + " disagrees with " +
                                        ftec::spbdd_fqcv_methods().front() + "\n    got     : " +
                                        got + "\n    expected: " + first);
                const auto it = expected.find(name);
                check(it != expected.end(), name + ": not in the expected file");
                if (it != expected.end()) {
                    check(got == it->second, name + "/" + method + "\n    got     : " + got +
                                                 "\n    expected: " + it->second);
                    if (got == it->second) std::cout << "  ok (" << method << ")\n";
                }
            }
        } catch (const std::exception& error) {
            check(false, name + ": " + error.what());
        }
    }

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    if (!print) std::cout << "all fqcv methods agree with the expected verdicts\n";
    return 0;
}
