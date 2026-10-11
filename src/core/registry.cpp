// 测试注册表与后端引导入口（运行注册测试 / 查询根任务状态）。

#include <map>
#include <utility>
#include <weave/test.hpp>

#include "core_internal.hpp"

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
  // 所有注册测试按名字序串行运行在根任务内：每个测试是一个子任务，
  // 根任务 await 其完成再跑下一个；任一测试 FAILED → 根任务 FAILED（整体失败）。
  // 串行 + 确定性（delta-cycle），顺序无关的并行留作扩展。
  Task root = create_task("weave_root", [] {
    for (const auto& entry : registry()) {
      Task t = create_task(entry.first, entry.second);
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
