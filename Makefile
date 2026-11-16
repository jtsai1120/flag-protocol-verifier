# Flag protocol verifier, on top of SPBDD (https://github.com/fmlab-iis/spbdd).
#
#   make            fetch and build SPBDD, then build ftec-verify
#   make check      also build and run the tests
#   make site       build the website's WebAssembly playground (needs Emscripten)
#   make check-spbdd-playground  check the (unpublished) SPBDD playground's
#                   examples against g++ (needs Emscripten and Node)
#   make clean      remove build/ (keeps the fetched SPBDD)
#   make distclean  also remove third_party/
#
# SPBDD is fetched at SPBDD_REF and built with its own Makefile, which in turn
# fetches BuDDy and bundles it into libspbdd.a. Only two things are kept from
# that build: the archive and the public headers, copied to
#
#   third_party/spbdd/lib/libspbdd.a
#   third_party/spbdd/include/spbdd/*.{hpp,h}
#
# To build against a different SPBDD:
#   make SPBDD_REF=<branch, tag or commit>        (then `make distclean` first)
#   make SPBDD_SRC=/path/to/your/spbdd/checkout   (no fetch; uses it as is)

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic

SPBDD_REPO ?= https://github.com/fmlab-iis/spbdd.git
SPBDD_REF  ?= 672a9a01968af6e2befc4b585f0f6926af5f221e
# What SPBDD itself is compiled with (BuDDy's C files use SPBDD's own flags).
SPBDD_CXXFLAGS ?= -std=c++17 -O3 -DNDEBUG

THIRD_PARTY := third_party
SPBDD_SRC   ?= $(THIRD_PARTY)/spbdd-src
SPBDD_DIR   := $(THIRD_PARTY)/spbdd
SPBDD_LIB   := $(SPBDD_DIR)/lib/libspbdd.a
SPBDD_STAMP := $(SPBDD_DIR)/.stamp

BUILD := build
BIN   := $(BUILD)/ftec-verify

CORE_SRC := src/fpdl/parser.cpp src/ftec/qasm.cpp src/ftec/dag.cpp src/ftec/verify.cpp \
            src/ftec/spbdd_backend.cpp src/ftec/se_check.cpp
CORE_OBJ := $(CORE_SRC:src/%.cpp=$(BUILD)/obj/%.o)
MAIN_OBJ := $(BUILD)/obj/ftec_verify_main.o
TESTS    := $(BUILD)/parser_tests $(BUILD)/qasm_tests $(BUILD)/se_check_tests $(BUILD)/fqcv_tests

CPPFLAGS := -Iinclude -I$(SPBDD_DIR)/include -MMD -MP
LDLIBS   := $(SPBDD_LIB) -lm

.PHONY: all spbdd check site check-spbdd-playground clean distclean
.DELETE_ON_ERROR:
.SECONDARY:

all: $(BIN)

spbdd: $(SPBDD_STAMP)

# --- SPBDD: fetch, build, keep lib + include ---------------------------------

$(THIRD_PARTY)/spbdd-src/.git/HEAD:
	@mkdir -p $(THIRD_PARTY)
	rm -rf $(THIRD_PARTY)/spbdd-src
	git init -q $(THIRD_PARTY)/spbdd-src
	git -C $(THIRD_PARTY)/spbdd-src fetch -q --depth 1 $(SPBDD_REPO) $(SPBDD_REF)
	git -C $(THIRD_PARTY)/spbdd-src checkout -q FETCH_HEAD

# SPBDD's own Makefile: `build/libspbdd.a` builds BuDDy and SPBDD into one
# archive (its `all` target would first wipe and re-clone everything).
$(SPBDD_STAMP): $(if $(filter $(THIRD_PARTY)/spbdd-src,$(SPBDD_SRC)),$(THIRD_PARTY)/spbdd-src/.git/HEAD)
	$(MAKE) -C $(SPBDD_SRC) build/libspbdd.a CXXFLAGS='$(SPBDD_CXXFLAGS)'
	rm -rf $(SPBDD_DIR)
	mkdir -p $(SPBDD_DIR)/lib $(SPBDD_DIR)/include/spbdd
	cp $(SPBDD_SRC)/build/libspbdd.a $(SPBDD_LIB)
	cp $(SPBDD_SRC)/include/spbdd/*.hpp $(SPBDD_SRC)/include/spbdd/*.h $(SPBDD_DIR)/include/spbdd/
	git -C $(SPBDD_SRC) rev-parse HEAD > $@ 2>/dev/null || echo local > $@

$(SPBDD_LIB): $(SPBDD_STAMP)

# --- the verifier -------------------------------------------------------------

$(BUILD)/obj/%.o: src/%.cpp $(SPBDD_STAMP)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILD)/obj/tests/%.o: tests/%.cpp $(SPBDD_STAMP)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

$(BIN): $(MAIN_OBJ) $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

$(BUILD)/%_tests: $(BUILD)/obj/tests/%_tests.o $(CORE_OBJ)
	$(CXX) $(CXXFLAGS) $^ $(LDLIBS) -o $@

# --- tests --------------------------------------------------------------------

# Protocol directories have brackets in their names, which $(wildcard) would
# read as patterns, so the shell lists them. The end-to-end test runs only the
# distance-3 protocols; the larger ones take minutes to hours each and belong in
# tools/bench_fqcv.py.
FPDL_ALL = $(shell find protocols -mindepth 2 -maxdepth 2 -name '*.fpdl' | sort)
QASM_ALL = $(shell find protocols -mindepth 2 -maxdepth 2 -name '*.qasm' | sort)
# Every protocol except [[49,1,7]], whose symbolic expansion alone takes
# minutes. Four protocols are known to have circuits that do not measure their
# declared generators -- some syndrome bits come out random, or land in the
# wrong position -- so the check must fail on them: LL25 [[5,1,3]] [2,2] and
# [2,2]^T, and both Du24 [[17,1,5]] protocols.
FPDL_SE_BAD = $(shell find protocols -mindepth 2 -maxdepth 2 -name '*.fpdl' \
                \( -path '*LL25_\[\[5,1,3\]\]_\[2,2\]*' -o -path '*Du24_\[\[17,1,5\]\]*' \) | sort)
FPDL_SE  = $(filter-out $(FPDL_SE_BAD),$(shell find protocols -mindepth 2 -maxdepth 2 -name '*.fpdl' ! -path '*49,1,7*' | sort))
FPDL_D3  = $(shell find protocols -mindepth 2 -maxdepth 2 -name '*.fpdl' -path '*,3]]*' | sort)

check: $(BIN) $(TESTS)
	$(BUILD)/parser_tests $(FPDL_ALL)
	$(BUILD)/qasm_tests $(QASM_ALL)
	$(BUILD)/se_check_tests $(FPDL_SE) $(addprefix --bad ,$(FPDL_SE_BAD))
	$(BUILD)/fqcv_tests tests/expected.txt $(FPDL_D3)

# The website's WebAssembly builds (needs Emscripten); see web/Makefile.
site: $(SPBDD_STAMP)
	$(MAKE) -f web/Makefile SPBDD_SRC=$(SPBDD_SRC)

# The SPBDD playground (web/spbdd-playground/, not published for now): every
# example, built natively with g++ and run by the
# playground's interpreter under Node, must print the same thing.
SPBDD_EXAMPLES = $(sort $(wildcard web/spbdd-playground/examples/*.cpp))
NODE ?= node --no-experimental-fetch

check-spbdd-playground: $(SPBDD_STAMP)
	$(MAKE) -f web/Makefile SPBDD_SRC=$(SPBDD_SRC) spbdd-playground
	@mkdir -p $(BUILD)/examples
	@for f in $(SPBDD_EXAMPLES); do \
	    b=$(BUILD)/examples/$$(basename $$f .cpp); \
	    $(CXX) -std=c++17 -I$(SPBDD_DIR)/include $$f $(SPBDD_LIB) -o $$b && $$b > $$b.native && \
	    $(NODE) web/run_playground_cpp.js $$f > $$b.playground && \
	    cmp -s $$b.native $$b.playground && echo "same output: $$f" || { echo "DIFFERENT: $$f"; exit 1; }; \
	done

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(THIRD_PARTY)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
