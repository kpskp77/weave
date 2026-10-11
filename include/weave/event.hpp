#pragma once

// 事件原语。语义对齐 asyncio.Event，set/clear 竞态采用 edge 语义：
// set() 时所有当前等待者即被判定唤醒，即使随后立即 clear() 也照样放行。

#include <memory>

namespace weave {

class Event {
public:
  Event();

  void wait();   // 阻塞直到 set()；已置位则立即返回
  void set();    // 置位并唤醒所有当前等待者
  void clear();  // 复位标志（不影响已被 set 判定唤醒的等待者）
  bool is_set() const;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
