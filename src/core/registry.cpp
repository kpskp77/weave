// Test registry and backend bootstrap entry (run registered tests / query the root task state).

#include <map>
#include <utility>
#include <weave/test.hpp>

#include "core_internal.hpp"
#include "trace.hpp"

namespace weave {

namespace {
std::map<std::string, test_fn>& registry() {
  static std::map<std::string, test_fn> r;
  return r;
}
}  // namespace

bool register_test(std::string name, test_fn fn) {
  registry().emplace(std::move(name), fn);
  return true;
}

namespace detail {

int run_registered_test() {
  if (registry().empty()) return -1;
  // All registered tests run serially inside the root task: each test is a child task and the root
  // awaits its completion before running the next one; any FAILED test → the root task FAILs (the
  // whole run fails). Serial + deterministic (delta-cycle); order-independent parallelism is left
  // as a future extension.
  Task root = create_task("weave_root", [] {
    for (const auto& [name, fn] : registry()) {
      detail::trace_registry()->debug("test '{}' start", name);
      Task t = create_task(name, fn);
      t.await();
    }
  });
  g_root = root.id_;
  return record(g_root).done_ev;
}

int root_status() {
  if (g_root < 0) return -1;
  switch (record(g_root).state) {
    case TaskState::DONE:
      return 0;
    case TaskState::FAILED:
      return 1;
    case TaskState::CANCELLED:
      return 2;
    default:
      return -1;
  }
}

}  // namespace detail
}  // namespace weave
