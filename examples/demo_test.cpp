// 端到端示例：把 task / Event / wait_for / cancel / gather 串成一个典型的
// producer-consumer 测试。slow 因 wait_for 超时被取消，demo 正常收尾，仿真 PASSED。
// 各原语的独立用法见同目录：task_demo / event_demo / sync_demo / taskgroup_demo。

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

  // wait_for 注定超时：cancel 生效，取消完成后抛 Timeout
  auto t3 = create_task("slow", slow);
  try {
    wait_for(t3, 1_us);
    throw std::logic_error("wait_for should have timed out");
  } catch (const Timeout&) {
    // 预期路径
  }
  if (!t3.cancelled()) throw std::logic_error("slow should be cancelled");

  // gather 等待剩余任务完成；不含已被消费的 t3
  gather(t1, t2);

  // gather asyncio 语义：首个失败按完成顺序立即重抛，不等慢任务，
  // 也不拖走/取消兄弟任务
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
  sleep(2_us);  // 慢任务应在 gather 失败后继续运行至自然完成
  if (!t5.done() || t5.cancelled())
    throw std::logic_error("peer must keep running after gather failure");

  // gather 自身被取消：未完成的子任务一并取消（asyncio 语义）
  auto t6 = create_task("gc_child", [&] { sleep(10_us); });
  auto t7 = create_task("gatherer", [&] {
    try {
      gather(t6);
      throw std::logic_error("unreachable");
    } catch (const Cancelled&) {
      // 预期路径：外层取消经 gather 传播
    }
  });
  sleep(100_ns);  // 让 gatherer 进入 gather 等待
  t7.cancel();
  sleep(100_ns);  // 让取消流程走完
  if (!t6.cancelled())
    throw std::logic_error("outer cancel of gather must cancel pending children");
}
