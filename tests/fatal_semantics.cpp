// Fatal-path self-check: an unrecoverable contract violation must end the run, not throw.
//
// This body cannot live in the shared *_semantics payloads (tests/weave_smoke_sc.cpp and friends):
// it kills the process, so every assertion after it would be unreachable. It is compiled into its
// own pair of payloads (weave_fatal_sc / weave_fatal_svdpi), which the regression runs through
// tests/expect_fatal.sh and tests/svdpi/run.sh --expect-fatal — with **inverted** judgement:
//
//   · the output must carry the core's headline "weave: fatal:" (so the violation was diagnosed,
//     with its site and the identity that hit it),
//   · the pass marker must be absent (a violation that some catch swallowed would end up printing
//     "WEAVE TEST PASSED" — the exact silent-green failure this payload exists to prevent),
//   · the process must not exit 0.
//
// Only one violation can be exercised per process, and this is the one a test is most likely to
// hit: a task awaiting itself. The other four sites (an ownerless suspension, a suspension after
// the body returned, a TaskGroup child waiting on its own group, shield() on the running task) go
// through the very same detail::fatal_violation path, so they are not repeated here.
//
// Assertion bodies stay in tests/ (see AGENTS.md); the backend entry points are sc_main.cpp and
// sv/weave_pkg.sv + tests/weave_tb.sv.

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using weave::current_task;
using weave::Task;

WEAVE_TEST(fatal_semantics) {
  std::printf("[fatal] about to await myself: the run must end here\n");
  std::fflush(stdout);
  const auto self = current_task();
  if (!self)
    throw std::logic_error("case1: no task context (a test body always runs inside a task)");
  Task self_task = *self;
  self_task.await();
  // Unreachable when the contract holds: a diagnosed wait cycle terminates the simulation instead
  // of raising something a caller could catch. Reaching this line means the violation was turned
  // back into an ordinary failure (or swallowed), which the runner reports as a failed run.
  throw std::logic_error("case1: the self-await returned instead of ending the run");
}
