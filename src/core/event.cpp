// Event 实现。edge 语义载体：set() 快照当前等待者的一次性唤醒事件并逐个触发。

#include <algorithm>
#include <utility>
#include <vector>
#include <weave/event.hpp>

#include "core_internal.hpp"

namespace weave {

struct Event::Impl {
  bool flag = false;
  std::vector<int> wake_evs;  // 当前等待者的一次性唤醒事件（edge 语义载体）
};

Event::Event() : impl_(std::make_shared<Impl>()) {}

bool Event::is_set() const {
  return impl_->flag;
}

void Event::set() {
  impl_->flag = true;
  const auto pending = std::move(impl_->wake_evs);  // 快照当前等待者
  impl_->wake_evs.clear();
  for (int w : pending)
    port::trigger(w);
}

void Event::clear() {
  impl_->flag = false;
}

void Event::wait() {
  if (impl_->flag) return;  // 已置位：立即通过
  const int w = port::create_event();
  impl_->wake_evs.push_back(w);
  try {
    detail::backend_wait(w, -1, detail::current());
  } catch (...) {
    // 取消路径：唤醒事件未被 set() 消费，自行摘除并销毁
    auto& v = impl_->wake_evs;
    v.erase(std::remove(v.begin(), v.end(), w), v.end());
    port::destroy_event(w);
    throw;
  }
  // edge 语义：被 set() 快照触发即放行，不复查 flag。
  // 成功路径同样要摘除再销毁：set() 之后仍可能被再次调用
  // （如 gather 首个失败后，晚到的兄弟完成再次置位），
  // 否则会对已销毁的事件 id 触发唤醒。
  auto& v = impl_->wake_evs;
  v.erase(std::remove(v.begin(), v.end(), w), v.end());
  port::destroy_event(w);
}

}  // namespace weave
