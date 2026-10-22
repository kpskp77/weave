#pragma once

// SystemC backend bootstrap entry point (implemented only in WEAVE_BACKEND_SYSTEMC builds).
// Typical usage (the test side provides sc_main):
//
//   #include <weave/weave.hpp>
//   int sc_main(int, char**) { return weave::sc::run(); }

namespace weave {
namespace sc {

// Call from sc_main:
//   1. spawn the root task (the first registered WEAVE_TEST) and the reporting process;
//   2. sc_start() runs the simulation; the reporting process prints PASSED/FAILED once the
//      root task reaches a terminal state and calls sc_stop();
//   3. return root_status() (0=DONE 1=FAILED 2=CANCELLED) as the process exit code.
int run();

}  // namespace sc
}  // namespace weave
