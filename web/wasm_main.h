#pragma once
// Compiled for the browser: main() becomes ftec_main(), with C linkage, so the
// page can call it (this Emscripten does not export main itself).
extern "C" int ftec_main(int argc, char** argv);
