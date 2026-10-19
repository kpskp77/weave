// Event 常见用法：is_set / set / clear 的标志语义，一次 set() 广播唤醒全部
// 当前等待者，以及 edge 语义（set() 快照等待者，随后 clear() 不收回放行）。

#include <cstdio>
#include <stdexcept>
#include <weave/weave.hpp>

using namespace weave;

WEAVE_TEST(event_demo) {
  // ── 标志语义：已置位时 wait() 立即返回，不挂起 ────────────────────────
  Event flag;
  if (flag.is_set()) throw std::logic_error("event_demo: fresh Event must be unset");
  flag.set();
  if (!flag.is_set()) throw std::logic_error("event_demo: is_set must reflect set()");
  flag.wait();  // 已置位 → 立即返回
  flag.clear();
  if (flag.is_set()) throw std::logic_error("event_demo: is_set must reflect clear()");

  // ── 广播：一次 set() 唤醒所有当前等待者 ───────────────────────────────
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
  ready_b.wait();  // 两个等待者都已就位
  go.set();        // 一次 set 同时放行两者
  done_a.wait();
  done_b.wait();
  gather(a, b);

  // ── edge 语义：被 set() 判定放行的等待者，不因随后 clear() 而失效 ─────
  Event ev, ready, proceeded;
  auto w = create_task("edge_waiter", [&] {
    ready.set();
    ev.wait();
    proceeded.set();  // ev 已被 set + clear，仍须走到这里
  });
  ready.wait();
  ev.set();
  ev.clear();  // 标志复位，但 w 已被 set() 的快照判定放行
  proceeded.wait();
  w.await();

  std::printf("[event_demo] set / clear / broadcast / edge ok\n");
}
