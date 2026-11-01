// Event implementation. The carrier of edge semantics: set() snapshots the current waiters'
// one-shot wake events and triggers each of them.

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
  detail::backend_wait(w, -1);
}

}  // namespace weave
