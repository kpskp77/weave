// 任务原语常见用法：create_task 的调度时机、await / done / cancelled / get_name、
// sleep 与 sleep(0)、cancel(msg)，以及"吞掉 Cancelled 拒绝取消"的协作式语义。
// 各原语的独立用法按域分散在本目录；端到端的典型测试见 demo_test.cpp。

#include <cstdio>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

WEAVE_TEST(task_demo) {
  // ── create_task：只调度，不立即执行 ──────────────────────────────────
  // 子任务要等父任务碰到下一个阻塞点（或结束）后才获得调度。
  Event started, gate;
  auto worker = create_task("worker", [&] {
    started.set();
    gate.wait();
  });
  if (started.is_set()) throw std::logic_error("task_demo: create_task must not run body eagerly");
  started.wait();  // 父任务在此挂起 → worker 得以运行并置位 started
  if (worker.done()) throw std::logic_error("task_demo: worker should still be running");
  if (worker.get_name() != "worker") throw std::logic_error("task_demo: get_name mismatch");
  gate.set();
  worker.await();  // 等待完成；若 worker FAILED，其异常在此重抛
  if (!worker.done() || worker.cancelled()) throw std::logic_error("task_demo: worker state");

  // ── sleep：仿真时间等待；sleep(0) 是零时间让出 ────────────────────────
  Event ticked;
  auto timer = create_task("timer", [&] {
    sleep(100_ns);
    sleep(0_fs);  // 不推进仿真时间，只做一次调度让出（≡ asyncio.sleep(0)）
    ticked.set();
  });
  ticked.wait();
  timer.await();

  // ── 变量时延：duration 支持 + - * / ，时延可直接由运行期变量算出 ──────
  // 越界/除零抛异常且非正时长夹到 0 tick（见 time.hpp）；这里用分段的固定步长累加：
  Event paced_done;
  const int rounds = 3;
  const duration step = 20_ns;
  auto paced = create_task("paced", [&] {
    for (int i = 0; i < rounds; ++i) sleep(step);  // rounds * step 拆成三段
    paced_done.set();
  });
  paced_done.wait();
  paced.await();

  // ── cancel(msg)：在目标的下一个阻塞点注入 Cancelled(msg) ──────────────
  Event ready, caught;
  auto victim = create_task("victim", [&] {
    ready.set();
    try {
      Event never;
      never.wait();
    } catch (const Cancelled& e) {
      // msg() 是 optional<string_view>：取消者没给消息时为 nullopt
      if (!e.msg() || *e.msg() != "shutdown")
        throw std::logic_error("task_demo: cancel msg missing at injection point");
      caught.set();
      throw;  // 不吞掉 → 任务终态 CANCELLED
    }
  });
  ready.wait();  // victim 已阻塞在 never.wait()（锁交接即同步）
  victim.cancel("shutdown");
  caught.wait();
  try {
    victim.await();  // CANCELLED 任务在 await 处重抛同一 Cancelled
    throw std::logic_error("task_demo: await must throw for a cancelled task");
  } catch (const Cancelled& e) {
    if (!e.msg() || *e.msg() != "shutdown")
      throw std::logic_error("task_demo: await must rethrow with the original msg");
  }
  if (!victim.cancelled()) throw std::logic_error("task_demo: victim should be CANCELLED");

  // ── 拒绝取消：吞掉 Cancelled 后正常返回 → 任务 DONE（不强杀进程） ─────
  Event ready2, finished;
  auto stubborn = create_task("stubborn", [&] {
    ready2.set();
    try {
      Event never;
      never.wait();
    } catch (const Cancelled&) {
      // 忽略取消，继续收尾（局部对象与 RAII 全程正常展开）
    }
    finished.set();
  });
  ready2.wait();
  stubborn.cancel();
  finished.wait();
  stubborn.await();  // DONE：不抛异常
  if (!stubborn.done() || stubborn.cancelled())
    throw std::logic_error("task_demo: swallowed cancel must end DONE");

  std::printf("[task_demo] create_task / await / sleep / cancel ok\n");
}
