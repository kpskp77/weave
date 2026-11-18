// Cross-language event handles: the raw backend id surface (include/weave/sv.hpp). Backend-agnostic
// — make/fire/destroy/wait go straight through the port contract, and the hand-over table is just
// storage the SystemVerilog testbench fills at bootstrap.

#include <stdexcept>
#include <weave/sv.hpp>

#include "core_internal.hpp"
#include "port.hpp"

namespace weave {
namespace sv {
namespace {

// Tags are handed in before the run starts, from simulation code that cannot handle an exception,
// so this table is plain storage with no allocation and no validation of the ids themselves.
int g_handed[kHandOverSlots];
bool g_handed_set[kHandOverSlots] = {};

void require(EventHandle h) {
  if (!h) throw std::logic_error("weave::sv: invalid event handle");
}

}  // namespace

EventHandle make_event() {
  return EventHandle{port::create_event()};
}

void fire(EventHandle h) {
  require(h);
  port::trigger(h.id);
}

void destroy(EventHandle h) {
  require(h);
  port::destroy_event(h.id);
}

void wait(EventHandle h) {
  require(h);
  detail::backend_wait(h.id, -1);  // No timeout leg: only a fire or a cancellation ends this
}

void hand_off(int tag, int ev_id) noexcept {
  if (tag < 0 || tag >= kHandOverSlots) return;
  g_handed[tag] = ev_id;
  g_handed_set[tag] = true;
}

EventHandle handed(int tag) noexcept {
  if (tag < 0 || tag >= kHandOverSlots || !g_handed_set[tag]) return EventHandle{};
  return EventHandle{g_handed[tag]};
}

}  // namespace sv
}  // namespace weave
