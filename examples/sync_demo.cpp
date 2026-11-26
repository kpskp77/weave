// Common combinator usage: gather (default / return_exceptions), wait_for, wait, shield,
// as_completed. All of them are built on the same task bookkeeping — synchronisation relies
// entirely on event passing, and nothing assumes "what should happen after sleeping this long".

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

namespace {

// A "work" task that external code can release: setting started signals readiness, then it waits
// for go.
void worker(Event& started, Event& go) {
  started.set();
  go.wait();
}

}  // namespace

WEAVE_TEST(sync_demo) {
  // ── gather: waits for a group of tasks to finish ──────────────────────
  // In the default form, any task failing/being cancelled is rethrown immediately in completion
  // order, and siblings are not cancelled.
  {
    Event s1, s2, go1, go2;
    auto a = create_task("gather_a", [&] { worker(s1, go1); });
    auto b = create_task("gather_b", [&] { worker(s2, go2); });
    s1.wait();
    s2.wait();  // Both tasks are now in place
    go1.set();
    go2.set();
    gather(a, b);  // All succeeded → returns normally
    std::printf("[sync_demo] gather ok\n");
  }

  // ── gather(..., return_exceptions): waits for everything to land, then
  // returns results in input order ──────────────────────────────────────
  //   DONE → nullptr; FAILED → its exception; CANCELLED → a wrapped CancelledError
  {
    Event block;
    auto ok = create_task("re_ok", [] {});
    auto bad = create_task("re_bad", [] { throw std::runtime_error("boom"); });
    auto pending = create_task("re_pending", [&] { block.wait(); });
    auto canceller = create_task("re_canceller", [&] { pending.cancel("cleanup"); });

    auto results = gather({ok, bad, pending}, return_exceptions);
    if (results.size() != 3) throw std::logic_error("sync_demo: result count mismatch");
    if (results[0]) throw std::logic_error("sync_demo: DONE must map to nullptr");
    try {
      std::rethrow_exception(results[1]);
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "boom") throw;
    }
    try {
      std::rethrow_exception(results[2]);
      throw std::logic_error("sync_demo: unreachable");
    } catch (const CancelledError& e) {
      if (!e.msg() || *e.msg() != "cleanup")
        throw std::logic_error("sync_demo: CANCELLED keeps the cancel msg");
    }
    canceller.await();
    std::printf("[sync_demo] gather(return_exceptions) ok\n");
  }

  // ── wait_for: returns normally on success; on timeout it cancels the
  // task, waits for the cancellation to complete, then throws TimeoutError ───
  {
    Event gate;
    auto fast = create_task("wait_for_fast", [&] { gate.wait(); });
    gate.set();
    wait_for(fast, 1_ms);  // The task finishes first → returns normally
    if (!fast.done() || fast.cancelled())
      throw std::logic_error("sync_demo: wait_for success path");

    auto slow = create_task("wait_for_slow", [] { sleep(10_us); });
    try {
      wait_for(slow, 1_ns);
      throw std::logic_error("sync_demo: wait_for must time out");
    } catch (const TimeoutError&) {
      // Expected path: after the timeout the task is cancelled and the cancellation completed
    }
    if (!slow.cancelled()) throw std::logic_error("sync_demo: timed-out task must be cancelled");
    std::printf("[sync_demo] wait_for ok\n");
  }

  // ── wait: only groups by the state at that moment, never rethrows,
  // never cancels tasks ─────────────────────────────────────────────────
  {
    Event slow_gate;
    auto quick = create_task("wait_quick", [] {});
    auto slow = create_task("wait_slow", [&] { slow_gate.wait(); });
    // Note: it must be written as weave::wait — glibc's <stdlib.h> defines a global
    // `union wait`, and a weave::Task argument triggers ADL, making both candidates for an
    // unqualified call (ambiguity).
    auto r = weave::wait({quick, slow}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1)
      throw std::logic_error("sync_demo: FIRST_COMPLETED must split done/pending");
    slow_gate.set();
    slow.await();  // Not cancelled, it finishes naturally after being released

    // Reaching the timeout also just takes a grouping snapshot
    Event gate2;
    auto t = create_task("wait_timeout", [&] { gate2.wait(); });
    auto snapshot = weave::wait({t}, ReturnWhen::ALL_COMPLETED, 1_ns);
    if (!snapshot.done.empty() || snapshot.pending.size() != 1)
      throw std::logic_error("sync_demo: timeout must return the current grouping");
    gate2.set();
    t.await();
    std::printf("[sync_demo] wait ok\n");
  }

  // ── shield: cancel/timeout of the proxy stops at the proxy layer, inner keeps running ──
  {
    Event gate;
    auto inner = create_task("shield_inner", [&] { gate.wait(); });
    auto proxy = shield(inner);  // The proxy mirrors inner's terminal state but can be cancelled
                                 // independently
    try {
      wait_for(proxy, 1_ns);
      throw std::logic_error("sync_demo: shield proxy must time out");
    } catch (const TimeoutError&) {
      // Expected path
    }
    if (!proxy.cancelled()) throw std::logic_error("sync_demo: proxy should be cancelled");
    if (inner.done()) throw std::logic_error("sync_demo: shield must protect inner");
    gate.set();
    inner.await();
    if (!inner.done() || inner.cancelled()) throw std::logic_error("sync_demo: inner state");
    std::printf("[sync_demo] shield ok\n");
  }

  // ── as_completed: yields one by one in completion order (independent of creation order) ──
  {
    Event g1, g2;
    auto first = create_task("ac_first", [&] { g1.wait(); });
    auto second = create_task("ac_second", [&] { g2.wait(); });
    auto ac = as_completed({first, second});

    g2.set();  // Release only the one created second
    if (ac.next().get_name() != "ac_second")
      throw std::logic_error("sync_demo: next() must yield by completion order");
    g1.set();
    if (ac.next().get_name() != "ac_first")
      throw std::logic_error("sync_demo: completion order must be second, first");
    if (ac.pending()) throw std::logic_error("sync_demo: all tasks must be produced");
    std::printf("[sync_demo] as_completed ok\n");
  }
}
