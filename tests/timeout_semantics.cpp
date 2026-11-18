// timeout semantic self-check (asyncio.timeout): a body that finishes in time returns normally; a
// body that overruns is cancelled and landed before TimeoutError is thrown; a body that fails
// rethrows its own failure (not TimeoutError); a zero limit still cancels and lands the body; and a
// cancelled caller takes the body down with it. Ordering stays fully event-driven — the ready
// hand-offs guarantee the body is blocked before the mechanism under test acts. Each case prints
// [PASS]/[FAIL] through run_case, and report summarises at the end (see case_runner.hpp). It shares
// the assertion-body style with wait_for_semantics.cpp and is regressed by both the weave_smoke_sc
// and weave_smoke_svdpi backends.

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(timeout_semantics) {
  // case1: the body finishes in time → returns normally
  run_case(__func__, "case1", [&] {
    Event done;
    timeout(1_ms, [&] { done.set(); });
    if (!done.is_set()) throw std::logic_error("case1: the body must have run to completion");
  });

  // case2: the body overruns → cancelled, landed, then TimeoutError
  run_case(__func__, "case2", [&] {
    Event ready;
    bool body_cancelled = false;
    bool timed_out = false;
    try {
      timeout(1_ms, [&] {
        ready.set();
        Event never;
        try {
          never.wait();
        } catch (const CancelledError&) {
          body_cancelled = true;
          throw;
        }
      });
    } catch (const TimeoutError&) {
      timed_out = true;
    }
    if (!timed_out) throw std::logic_error("case2: an overrunning body must throw TimeoutError");
    if (!body_cancelled) throw std::logic_error("case2: the body must be cancelled, not detached");
  });

  // case3: a failing body rethrows its own failure — not TimeoutError
  run_case(__func__, "case3", [&] {
    bool saw_failure = false;
    try {
      timeout(1_ms, [] { throw std::runtime_error("boom"); });
    } catch (const TimeoutError&) {
      throw std::logic_error("case3: a failing body must not be reported as a timeout");
    } catch (const std::runtime_error& e) {
      saw_failure = std::string(e.what()) == "boom";
    }
    if (!saw_failure) throw std::logic_error("case3: the body's own exception must propagate");
  });

  // case4: a zero limit is well defined — the body is started and immediately bounded, so a body
  // that has to suspend always times out (and is still cancelled and landed)
  run_case(__func__, "case4", [&] {
    bool body_cancelled = false;
    bool timed_out = false;
    try {
      timeout(duration{}, [&] {
        Event never;
        try {
          never.wait();
        } catch (const CancelledError&) {
          body_cancelled = true;
          throw;
        }
      });
    } catch (const TimeoutError&) {
      timed_out = true;
    }
    if (!timed_out) throw std::logic_error("case4: a zero limit must throw TimeoutError");
    if (!body_cancelled) throw std::logic_error("case4: the body must be cancelled and landed");
  });

  // case5: the caller is cancelled → CancelledError (not TimeoutError) and the body goes with it
  run_case(__func__, "case5", [&] {
    Event body_ready;
    bool body_cancelled = false;
    auto caller = create_task("timeout_caller", [&] {
      timeout(100_ms, [&] {
        body_ready.set();
        Event never;
        try {
          never.wait();
        } catch (const CancelledError&) {
          body_cancelled = true;
          throw;
        }
      });
    });
    body_ready.wait();  // the body is blocked inside the bounded block
    caller.cancel("stop the block");

    bool cancelled = false;
    try {
      caller.await();
    } catch (const CancelledError&) {
      cancelled = true;
    } catch (const TimeoutError&) {
      throw std::logic_error("case5: a cancelled caller must not be reported as a timeout");
    }
    if (!cancelled) throw std::logic_error("case5: the caller must propagate CancelledError");
    if (!body_cancelled)
      throw std::logic_error("case5: the body must be cancelled with the caller");
  });

  report(__func__);
}
