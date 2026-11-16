# The WebAssembly builds behind the website's two playgrounds. Run from the
# repository root as `make site` (it fetches SPBDD first); needs Emscripten
# (emcc/em++/emar on PATH).
#
#   site/playground/ftec.{js,wasm}   the verifier, ftec_main() instead of main()
#   site/playground/protocols.json   every protocol in protocols/, for the picker

EMCXX ?= em++
EMCC  ?= emcc
EMAR  ?= emar

SPBDD_SRC ?= third_party/spbdd-src
WASM_LIB  := $(SPBDD_SRC)/build-wasm/libspbdd.a
OPT       := -O3 -DNDEBUG -fexceptions

COMMON_LD := -fexceptions -sMODULARIZE=1 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB \
             -sENVIRONMENT=web,worker,node

FTEC_SRC := src/ftec_verify_main.cpp src/fpdl/parser.cpp src/ftec/qasm.cpp src/ftec/dag.cpp \
            src/ftec/verify.cpp src/ftec/spbdd_backend.cpp src/ftec/circuit_check.cpp

.PHONY: all spbdd-playground FORCE
all: site/playground/ftec.js site/playground/protocols.json

spbdd-playground: web/spbdd-playground/spbdd.js

# SPBDD's own Makefile again, with Emscripten and a build directory of its own
# so the native build is left alone.
$(WASM_LIB): $(SPBDD_SRC)/include/spbdd/bdd.h
	$(MAKE) -C $(SPBDD_SRC) build-wasm/libspbdd.a BUILD=build-wasm \
	    CC=$(EMCC) CXX=$(EMCXX) AR=$(EMAR) CXXFLAGS='-std=c++17 $(OPT)'

site/playground/ftec.js: $(FTEC_SRC) $(wildcard include/*/*.hpp) web/wasm_main.h $(WASM_LIB)
	@mkdir -p $(@D)
	$(EMCXX) -std=c++20 $(OPT) -include web/wasm_main.h -Dmain=ftec_main \
	    -Iinclude -I$(SPBDD_SRC)/include $(FTEC_SRC) $(WASM_LIB) $(COMMON_LD) \
	    -sEXPORT_NAME=createFtec -sEXPORTED_FUNCTIONS=_ftec_main,_malloc,_free \
	    -sEXPORTED_RUNTIME_METHODS='["FS","stringToUTF8","lengthBytesUTF8"]' -o $@

# The SPBDD playground is parked in web/spbdd-playground/ and not published;
# `make spbdd-playground` still builds it.
web/spbdd-playground/spbdd.js: web/spbdd_bind.cpp $(WASM_LIB)
	@mkdir -p $(@D)
	$(EMCXX) -std=c++17 $(OPT) -I$(SPBDD_SRC)/include $< $(WASM_LIB) --bind $(COMMON_LD) \
	    -sEXPORT_NAME=createSpbdd -o $@

# Rebuilt every time: it is cheap, and protocol directory names contain
# brackets, which make would read as patterns in a prerequisite list.
site/playground/protocols.json: FORCE
	python3 web/bundle_protocols.py protocols tests/expected.txt $@
