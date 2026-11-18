#pragma once

// Event and Pulse primitives. Event semantics follow asyncio.Event; set/clear races use edge
// semantics: every waiter present at set() time counts as woken, and is released even if clear()
// follows immediately.
//
// Pulse is the edge-only sibling, for sources whose native semantics are "-> ev" (a SystemVerilog
// event): fire() wakes exactly the waiters present at that instant and leaves nothing behind, so a
// caller that starts waiting *after* the pulse has gone by stays blocked instead of consuming an
// edge that never belonged to it. Mapping an SV event onto the sticky Event would turn that into a
// silent false positive; that is why both exist rather than one flag parameter.

#include <cstdint>
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

// Edge-triggered pulse (no asyncio counterpart, hence C++ naming). Copies share the same pulse,
// like Event::Impl does.
class Pulse {
public:
  Pulse();

  // Blocks until the next fire(); a fire() that happened before this call is NOT remembered. Must
  // be called from a task context (it suspends).
  void wait();

  // Wakes every waiter present right now and advances generation(). Leaves no flag behind: a later
  // wait() blocks for a new pulse. Never suspends.
  void fire();

  // Number of fire() calls so far. A caller that was busy between two of its own waits can snapshot
  // this before and compare after, to find out whether it missed an edge (the primitive itself
  // cannot tell it). Never suspends.
  [[nodiscard]] std::uint64_t generation() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace weave
