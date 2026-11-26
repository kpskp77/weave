// Common Event usage: the flag semantics of is_set / set / clear, a single set() broadcasting to
// every current waiter, and edge semantics (set() snapshots the waiters, a following clear() does
// not take the release back).

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;

WEAVE_TEST(event_demo) {
  // ── flag semantics: wait() returns immediately when already set, no suspension ──
  Event flag;
  if (flag.is_set()) throw std::logic_error("event_demo: fresh Event must be unset");
  flag.set();
  if (!flag.is_set()) throw std::logic_error("event_demo: is_set must reflect set()");
  flag.wait();  // Already set → returns immediately
  flag.clear();
  if (flag.is_set()) throw std::logic_error("event_demo: is_set must reflect clear()");

  // ── broadcast: one set() wakes all current waiters ────────────────────
  Event go, ready_a, ready_b, done_a, done_b;
  auto a = create_task("waiter_a", [&] {
    ready_a.set();
    go.wait();
    done_a.set();
  });
  auto b = create_task("waiter_b", [&] {
    ready_b.set();
    go.wait();
    done_b.set();
  });
  ready_a.wait();
  ready_b.wait();  // Both waiters are now in place
  go.set();        // A single set releases both
  done_a.wait();
  done_b.wait();
  gather(a, b);

  // ── edge semantics: a waiter released by set() is not undone by a later clear() ──
  Event ev, ready, proceeded;
  auto w = create_task("edge_waiter", [&] {
    ready.set();
    ev.wait();
    proceeded.set();  // ev was set then cleared, yet we must still get here
  });
  ready.wait();
  ev.set();
  ev.clear();  // The flag is reset, but w was already released by set()'s snapshot
  proceeded.wait();
  w.await();

  std::printf("[event_demo] set / clear / broadcast / edge ok\n");
}
