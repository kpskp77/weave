#pragma once

// Event primitive. Semantics follow asyncio.Event; set/clear races use edge semantics:
// every waiter present at set() time counts as woken, and is released even if clear()
// follows immediately.

#include <memory>

namespace weave {

class Event {
public:
  Event();

  void wait();            // Blocks until set(); returns immediately if already set
  void set();             // Sets the flag and wakes all current waiters
  void clear() noexcept;  // Clears the flag (does not affect waiters already woken by set)
  [[nodiscard]] bool is_set() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
