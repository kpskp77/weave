// Weave C++ bookkeeping self-check: verifies shield's asyncio.shield semantics on a real SystemC
// kernel (weave_smoke_sc). Synchronisation is fully event-driven (pure delta cycles) and does not
// depend on a real clock.
// Each case prints [PASS]/[FAIL] through run_case, and report summarises at the end (see
// case_runner.hpp).

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

WEAVE_TEST(shield_semantics) {
  // case 1: a wait_for timeout cancels only the proxy, inner keeps running
  run_case(__func__, "case1", [&] {
    Event go;
    auto inner = create_task("s1_inner", [&] { go.wait(); });
    auto proxy = shield(inner);
    try {
      wait_for(proxy, 1_ms);
      throw std::logic_error("case1: wait_for should timeout");
    } catch (const Timeout&) {
      // Expected path
    }
    if (!proxy.cancelled()) throw std::logic_error("case1: proxy should be cancelled");
    if (inner.done()) throw std::logic_error("case1: shield must protect inner from timeout");
    go.set();
    inner.await();
    if (!inner.done() || inner.cancelled())
      throw std::logic_error("case1: inner should finish on its own");
  });

  // case 2: gather is cancelled → the cascade stops at the proxy layer and inner survives
  run_case(__func__, "case2", [&] {
    Event block, go, grp_ready;
    auto inner = create_task("s2_inner", [&] { go.wait(); });
    auto proxy = shield(inner);
    auto other = create_task("s2_other", [&] { block.wait(); });
    auto grp = create_task("s2_grp", [&] {
      grp_ready.set();
      try {
        gather(proxy, other);
        throw std::logic_error("case2: unreachable");
      } catch (const Cancelled&) {
        // Expected path
      }
    });
    grp_ready.wait();  // grp is now blocked inside gather (the hand-off itself synchronises)
    grp.cancel();
    grp.await();
    // After cancelling siblings, gather does not wait for them to land: proxy/other's cancellation
    // slices and main's wakeup share a slot, and their order is up to the backend — so await the
    // terminal states explicitly before asserting (determinism).
    try {
      proxy.await();
      throw std::logic_error("case2: proxy should throw Cancelled");
    } catch (const Cancelled&) {
      // Expected path
    }
    try {
      other.await();
      throw std::logic_error("case2: other should throw Cancelled");
    } catch (const Cancelled&) {
      // Expected path
    }
    if (!proxy.cancelled()) throw std::logic_error("case2: proxy should be cancelled");
    if (!other.cancelled()) throw std::logic_error("case2: unshielded sibling cancelled");
    if (inner.done()) throw std::logic_error("case2: shield must protect inner from gather");
    go.set();
    inner.await();
    if (!inner.done() || inner.cancelled())
      throw std::logic_error("case2: inner should finish on its own");
  });

  // case 3: the proxy mirrors inner's failure
  run_case(__func__, "case3", [&] {
    Event go;
    auto inner = create_task("s3_inner", [&] {
      go.wait();
      throw std::runtime_error("s3_inner_fail");
    });
    auto proxy = shield(inner);
    go.set();
    try {
      proxy.await();
      throw std::logic_error("case3: proxy should rethrow inner failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "s3_inner_fail")
        throw std::logic_error("case3: proxy must mirror inner's exception");
    }
    try {
      inner.await();
      throw std::logic_error("case3: inner should rethrow its failure");
    } catch (const std::runtime_error& e) {
      if (std::string(e.what()) != "s3_inner_fail") throw;
    }
  });

  // case 4: inner was already terminal before shield → the proxy mirrors the completion directly
  run_case(__func__, "case4", [&] {
    auto inner = create_task("s4_inner", [] {});
    inner.await();  // inner is already DONE
    auto proxy = shield(inner);
    proxy.await();
    if (!proxy.done() || proxy.cancelled())
      throw std::logic_error("case4: proxy should mirror pre-completed inner");
  });

  report(__func__);
}
