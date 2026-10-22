// End-to-end example: strings task / Event / wait_for / cancel / gather into a typical
// producer-consumer test. slow gets cancelled because wait_for times out, demo winds down
// normally, and the simulation reports PASSED.
// Isolated usage of each primitive lives in the same directory: task_demo / event_demo /
// sync_demo / taskgroup_demo.

#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

namespace {

void producer(Event& ev) {
  sleep(100_ns);
  ev.set();
}

void consumer(Event& ev) {
  ev.wait();
  sleep(10_ns);
}

void slow() {
  sleep(10_us);
}

}  // namespace

WEAVE_TEST(demo) {
  Event ev;
  auto t1 = create_task("producer", [&] { producer(ev); });
  auto t2 = create_task("consumer", [&] { consumer(ev); });

  // wait_for is bound to time out: the cancel takes effect and Timeout is thrown once the
  // cancellation completes
  auto t3 = create_task("slow", slow);
  try {
    wait_for(t3, 1_us);
    throw std::logic_error("wait_for should have timed out");
  } catch (const Timeout&) {
    // Expected path
  }
  if (!t3.cancelled()) throw std::logic_error("slow should be cancelled");

  // gather waits for the remaining tasks; t3 is not included, its exception was already consumed
  gather(t1, t2);

  // asyncio gather semantics: the first failure is rethrown immediately in completion order,
  // without waiting for slow tasks and without dragging down / cancelling peers
  auto t4 = create_task("boom", [&] {
    sleep(100_ns);
    throw std::runtime_error("boom");
  });
  auto t5 = create_task("peer", [&] { sleep(1_us); });
  try {
    gather(t4, t5);
    throw std::logic_error("gather should have rethrown boom");
  } catch (const std::runtime_error& e) {
    if (std::string(e.what()) != "boom") throw;
  }
  if (t5.done() || t5.cancelled())
    throw std::logic_error("gather failure must not drag peers to terminal");
  sleep(2_us);  // The slow task should keep running to natural completion after gather fails
  if (!t5.done() || t5.cancelled())
    throw std::logic_error("peer must keep running after gather failure");

  // gather itself is cancelled: unfinished child tasks are cancelled too (asyncio semantics)
  auto t6 = create_task("gc_child", [&] { sleep(10_us); });
  auto t7 = create_task("gatherer", [&] {
    try {
      gather(t6);
      throw std::logic_error("unreachable");
    } catch (const Cancelled&) {
      // Expected path: the outer cancellation propagates through gather
    }
  });
  sleep(100_ns);  // Let gatherer enter the gather wait
  t7.cancel();
  sleep(100_ns);  // Let the cancellation flow finish
  if (!t6.cancelled())
    throw std::logic_error("outer cancel of gather must cancel pending children");
}
