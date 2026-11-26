// Event / Pulse implementation. The carrier of edge semantics: set() and fire() snapshot the
// current waiters' one-shot wake events and trigger each of them; Pulse differs only in keeping no
// flag (and counting fires, so a caller can reconcile missed edges itself).

#include <algorithm>
#include <utility>
#include <vector>
#include <weave/event.hpp>

#include "core_internal.hpp"
#include "trace.hpp"

namespace weave {

struct Event::Impl {
  bool flag = false;
  std::vector<int> wake_evs;  // One-shot wake events of the current waiters (edge carrier)
};

Event::Event() : impl_(std::make_shared<Impl>()) {}

bool Event::is_set() const noexcept {
  return impl_->flag;
}

void Event::set() {
  impl_->flag = true;
  const auto pending = std::exchange(impl_->wake_evs, {});  // Snapshot the current waiters
  detail::trace_event()->trace("event set: snapshot has {} waiter(s)", pending.size());
  for (int w : pending) port::trigger(w);
}

void Event::clear() noexcept {
  impl_->flag = false;
}

void Event::wait() {
  if (impl_->flag) {
    detail::trace_event()->trace("event wait: already set, passing through");
    return;
  }
  const int w = port::create_event();
  auto& v = impl_->wake_evs;
  // The cleanup covers both the success and the exception path, and must unlink before
  // destroying: set() may still be called again afterwards (e.g. after gather's first failure, a
  // late sibling completing sets it once more), which would otherwise trigger an already destroyed
  // event id. The guard is declared before push_back: if enqueueing throws, the erase is a no-op
  // and the event is still destroyed.
  auto guard = detail::on_scope_exit([&v, w] {
    v.erase(std::remove(v.begin(), v.end(), w), v.end());
    port::destroy_event(w);
  });
  v.push_back(w);
  detail::trace_event()->trace("event wait: queued as wake ev {} ({} in snapshot)", w, v.size());
  // Edge semantics: being snapshotted by set() is enough to be released, the flag is not rechecked.
  // A leaf wait: allowed outside a task context (a testbench process waiting on a cross-language
  // event has nothing to cancel and no wait-graph node to publish).
  detail::backend_wait(w, -1, detail::Ownerless::Allow);
}

struct Pulse::Impl {
  std::vector<int> wake_evs;     // One-shot wake events of the current waiters (edge carrier)
  std::uint64_t generation = 0;  // Number of fire() calls, for the caller's own reconciliation
};

Pulse::Pulse() : impl_(std::make_shared<Impl>()) {}

std::uint64_t Pulse::generation() const noexcept {
  return impl_->generation;
}

void Pulse::fire() {
  ++impl_->generation;
  const auto pending = std::exchange(impl_->wake_evs, {});  // Snapshot the current waiters
  detail::trace_event()->trace("pulse fire: snapshot has {} waiter(s), generation {}",
                               pending.size(), impl_->generation);
  for (int w : pending) port::trigger(w);
}

void Pulse::wait() {
  const int w = port::create_event();
  auto& v = impl_->wake_evs;
  // Same invariant as Event::wait: the guard covers both the success and the exception path, and
  // unlinks before destroying, because a later fire() must never touch a destroyed wake event id.
  auto guard = detail::on_scope_exit([&v, w] {
    v.erase(std::remove(v.begin(), v.end(), w), v.end());
    port::destroy_event(w);
  });
  v.push_back(w);
  detail::trace_event()->trace("pulse wait: queued as wake ev {} ({} in snapshot)", w, v.size());
  // No state is consulted on the way out: only a fire() that saw this waiter releases it.
  detail::backend_wait(w, -1, detail::Ownerless::Allow);
}

}  // namespace weave
