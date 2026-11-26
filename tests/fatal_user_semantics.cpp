// Fatal-path self-check for the public entry: a caller that decides the run cannot continue ends it
// through weave::fatal() — not by throwing, and not through a backend-specific $fatal /
// SC_REPORT_FATAL.
//
// Separate payload pair from tests/fatal_semantics.cpp on purpose: that file pins the core's
// contract-violation sites, a process can only die once, and both must be judged with inverted
// expectation (weave_fatal_user_sc / weave_fatal_user_svdpi, tests/expect_fatal.sh and
// tests/run-eda.sh --expect-fatal).
//
// What the regression pins, given the caller's message "caller gave up: the retry loop never
// converged":
//   · the headline carries the caller's wording and the identity that declared it
//     (task 2 'fatal_user_semantics') — so the report path was taken, not some other death,
//   · the pass marker is absent (a fatal that some catch swallowed would end up printing
//     "WEAVE TEST PASSED" — the silent-green failure this payload exists to prevent),
//   · the process does not exit 0.
//
// Assertion bodies stay in tests/ (see AGENTS.md); the backend entry points are sc_main.cpp and
// sv/weave_pkg.sv + tests/weave_tb.sv.

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using weave::current_task;

WEAVE_TEST(fatal_user_semantics) {
  if (!current_task())
    throw std::logic_error("case1: no task context (a test body always runs inside a task)");
  std::printf("[fatal-user] about to end the run through weave::fatal: it must not return\n");
  std::fflush(stdout);
  weave::fatal("caller gave up: the retry loop never converged");
  // Unreachable when the contract holds: fatal ends the simulation instead of unwinding, so a
  // caller cannot catch it. Reaching this line means the entry degraded into an ordinary
  // exception, which the runner reports as a failed run.
  throw std::logic_error("case1: weave::fatal returned instead of ending the run");
}
