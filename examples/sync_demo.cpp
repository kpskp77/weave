// 组合原语常见用法：gather（默认 / return_exceptions）、wait_for、wait、
// shield、as_completed。全部建于同一套任务簿记之上——同步完全靠事件传递，
// 不假设任何"睡多久之后应该发生什么"。

#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <weave/weave.hpp>

using namespace weave;
using namespace weave::literals;

namespace {

// 可被外部放行的"工作"任务：置位 started 表示就位，然后等 go。
void worker(Event& started, Event& go) {
  started.set();
  go.wait();
}

}  // namespace

WEAVE_TEST(sync_demo) {
  // ── gather：等一组任务全部完成 ────────────────────────────────────────
  // 默认形态下任一任务失败/被取消会立即按完成顺序重抛，兄弟任务不被取消。
  {
    Event s1, s2, go1, go2;
    auto a = create_task("gather_a", [&] { worker(s1, go1); });
    auto b = create_task("gather_b", [&] { worker(s2, go2); });
    s1.wait();
    s2.wait();  // 两个任务都已就位
    go1.set();
    go2.set();
    gather(a, b);  // 全部成功 → 正常返回
    std::printf("[sync_demo] gather ok\n");
  }

  // ── gather(..., return_exceptions)：等全部落地，按输入顺序取结果 ──────
  //   DONE → nullptr；FAILED → 其异常；CANCELLED → 包装 Cancelled
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
    } catch (const Cancelled& e) {
      if (!e.msg() || *e.msg() != "cleanup")
        throw std::logic_error("sync_demo: CANCELLED keeps the cancel msg");
    }
    canceller.await();
    std::printf("[sync_demo] gather(return_exceptions) ok\n");
  }

  // ── wait_for：成功照常返回；超时则取消任务、等取消完成、再抛 Timeout ──
  {
    Event gate;
    auto fast = create_task("wait_for_fast", [&] { gate.wait(); });
    gate.set();
    wait_for(fast, 1_ms);  // 任务先完成 → 正常返回
    if (!fast.done() || fast.cancelled())
      throw std::logic_error("sync_demo: wait_for success path");

    auto slow = create_task("wait_for_slow", [] { sleep(10_us); });
    try {
      wait_for(slow, 1_ns);
      throw std::logic_error("sync_demo: wait_for must time out");
    } catch (const Timeout&) {
      // 预期路径：超时后任务已被取消，且取消已走完
    }
    if (!slow.cancelled()) throw std::logic_error("sync_demo: timed-out task must be cancelled");
    std::printf("[sync_demo] wait_for ok\n");
  }

  // ── wait：只按当时状态分组返回，不重抛异常、不取消任务 ─────────────────
  {
    Event slow_gate;
    auto quick = create_task("wait_quick", [] {});
    auto slow = create_task("wait_slow", [&] { slow_gate.wait(); });
    auto r = wait({quick, slow}, ReturnWhen::FIRST_COMPLETED);
    if (r.done.size() != 1 || r.pending.size() != 1)
      throw std::logic_error("sync_demo: FIRST_COMPLETED must split done/pending");
    slow_gate.set();
    slow.await();  // 未被取消，放行后自然完成

    // timeout 到点同样只做分组快照
    Event gate2;
    auto t = create_task("wait_timeout", [&] { gate2.wait(); });
    auto snapshot = wait({t}, ReturnWhen::ALL_COMPLETED, 1_ns);
    if (!snapshot.done.empty() || snapshot.pending.size() != 1)
      throw std::logic_error("sync_demo: timeout must return the current grouping");
    gate2.set();
    t.await();
    std::printf("[sync_demo] wait ok\n");
  }

  // ── shield：对代理的取消/超时停在代理层，inner 继续运行 ────────────────
  {
    Event gate;
    auto inner = create_task("shield_inner", [&] { gate.wait(); });
    auto proxy = shield(inner);  // 代理镜像 inner 的终态，但可独立被取消
    try {
      wait_for(proxy, 1_ns);
      throw std::logic_error("sync_demo: shield proxy must time out");
    } catch (const Timeout&) {
      // 预期路径
    }
    if (!proxy.cancelled()) throw std::logic_error("sync_demo: proxy should be cancelled");
    if (inner.done()) throw std::logic_error("sync_demo: shield must protect inner");
    gate.set();
    inner.await();
    if (!inner.done() || inner.cancelled()) throw std::logic_error("sync_demo: inner state");
    std::printf("[sync_demo] shield ok\n");
  }

  // ── as_completed：按完成顺序逐个产出（与创建顺序无关） ──────────────────
  {
    Event g1, g2;
    auto first = create_task("ac_first", [&] { g1.wait(); });
    auto second = create_task("ac_second", [&] { g2.wait(); });
    auto ac = as_completed({first, second});

    g2.set();  // 只放行"后创建"的那个
    if (ac.next().get_name() != "ac_second")
      throw std::logic_error("sync_demo: next() must yield by completion order");
    g1.set();
    if (ac.next().get_name() != "ac_first")
      throw std::logic_error("sync_demo: completion order must be second, first");
    if (ac.pending()) throw std::logic_error("sync_demo: all tasks must be produced");
    std::printf("[sync_demo] as_completed ok\n");
  }
}
