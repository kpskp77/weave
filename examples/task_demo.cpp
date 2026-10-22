// Common task-primitive usage: create_task's scheduling timing, await / done / cancelled /
// get_name, sleep and sleep(0), cancel(msg), and the cooperative semantics of "swallowing
// Cancelled to refuse cancellation".
// Isolated usage of each primitive is spread across this directory by domain; a typical
// end-to-end test lives in demo_test.cpp.

#include <cstdio>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(task_demo) {
  // ── create_task: schedules only, does not run immediately ─────────────
  // A child task is scheduled only after the parent hits its next blocking point (or finishes).
  Event started, gate;
  auto worker = create_task("worker", [&] {
    started.set();
    gate.wait();
  });
  if (started.is_set()) throw std::logic_error("task_demo: create_task must not run body eagerly");
  started.wait();  // The parent suspends here → worker runs and sets started
  if (worker.done()) throw std::logic_error("task_demo: worker should still be running");
  if (worker.get_name() != "worker") throw std::logic_error("task_demo: get_name mismatch");
  gate.set();
  worker.await();  // Waits for completion; if worker FAILED, its exception is rethrown here
  if (!worker.done() || worker.cancelled()) throw std::logic_error("task_demo: worker state");

  // ── sleep: waits for simulation time; sleep(0) is a zero-time yield ───
  Event ticked;
  auto timer = create_task("timer", [&] {
    sleep(100_ns);
    sleep(0_fs);  // Does not advance simulation time, just yields once (≡ asyncio.sleep(0))
    ticked.set();
  });
  ticked.wait();
  timer.await();

  // ── variable delays: duration supports + - * /, so a delay can be
  // computed from runtime variables ─────────────────────────────────────
  // Overflow/division by zero throw, and non-positive durations clamp to 0 ticks (see time.hpp);
  // here a fixed step is accumulated in segments:
  Event paced_done;
  const int rounds = 3;
  const duration step = 20_ns;
  auto paced = create_task("paced", [&] {
    for (int i = 0; i < rounds; ++i) sleep(step);  // rounds * step split into three parts
    paced_done.set();
  });
  paced_done.wait();
  paced.await();

  // ── cancel(msg): injects Cancelled(msg) at the target's next blocking point ──
  Event ready, caught;
  auto victim = create_task("victim", [&] {
    ready.set();
    try {
      Event never;
      never.wait();
    } catch (const Cancelled& e) {
      // msg() is an optional<string_view>: nullopt when the canceller passed no message
      if (!e.msg() || *e.msg() != "shutdown")
        throw std::logic_error("task_demo: cancel msg missing at injection point");
      caught.set();
      throw;  // Not swallowed → the task's terminal state is CANCELLED
    }
  });
  ready.wait();  // victim is now blocked in never.wait() (the hand-off itself synchronises)
  victim.cancel("shutdown");
  caught.wait();
  try {
    victim.await();  // Awaiting a CANCELLED task rethrows the same Cancelled
    throw std::logic_error("task_demo: await must throw for a cancelled task");
  } catch (const Cancelled& e) {
    if (!e.msg() || *e.msg() != "shutdown")
      throw std::logic_error("task_demo: await must rethrow with the original msg");
  }
  if (!victim.cancelled()) throw std::logic_error("task_demo: victim should be CANCELLED");

  // ── refusing cancellation: swallowing Cancelled and returning normally
  // → the task is DONE (the process is never killed) ────────────────────
  Event ready2, finished;
  auto stubborn = create_task("stubborn", [&] {
    ready2.set();
    try {
      Event never;
      never.wait();
    } catch (const Cancelled&) {
      // Ignore the cancellation and carry on (locals and RAII unwind normally throughout)
    }
    finished.set();
  });
  ready2.wait();
  stubborn.cancel();
  finished.wait();
  stubborn.await();  // DONE: does not throw
  if (!stubborn.done() || stubborn.cancelled())
    throw std::logic_error("task_demo: swallowed cancel must end DONE");

  std::printf("[task_demo] create_task / await / sleep / cancel ok\n");
}
