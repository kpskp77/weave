// SystemC backend self-check binary entry point (WEAVE_BACKEND_SYSTEMC).
// Runs all semantic assertions in tests/ on a real SystemC kernel.

#include <weave/sc.hpp>

// This xmake-repo SystemC build looks for sc_main with C linkage (sc_main_main.cpp references the
// unmangled symbol), hence the explicit extern "C" here. If the SystemC on your target platform
// uses C++ linkage, drop extern "C".
extern "C" __attribute__((visibility("default"))) int sc_main(int, char**);

extern "C" __attribute__((visibility("default"))) int sc_main(int, char**) {
  return weave::sc::run();
}
