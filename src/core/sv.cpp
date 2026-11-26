// Named cross-language edges (include/weave/sv.hpp). Backend-agnostic: ids come from the port
// contract, and the name table is plain storage that the SystemVerilog testbench can publish into.

#include <map>
#include <string>
#include <weave/sv.hpp>

#include "core_internal.hpp"
#include "port.hpp"

namespace weave {
namespace sv {
namespace detail {

struct Entry {
  int id = -1;
  bool bound = false;  // A SystemVerilog weave_edge object exists for this name
};

namespace {

// One name is one event for the whole run, so the table owns the ids and outlives every Edge
// object — an Edge is a bare pointer to its entry (include/weave/sv.hpp). std::map rather than
// unordered_map (or any flat container): entries are never moved once created and never erased,
// which is exactly what keeps those pointers valid for the run, and what lets bound() answer
// without looking the name up at all.
std::map<std::string, Entry>& registry() {
  static std::map<std::string, Entry> table;
  return table;
}

Entry& entry_for(const std::string& name) {
  return registry()[name];  // Created on first mention; id stays -1 until one side makes it
}

}  // namespace
}  // namespace detail

Edge::Edge(const char* name) : entry_(&detail::entry_for(name != nullptr ? name : "")) {
  if (entry_->id < 0) entry_->id = port::create_event();  // Just a fresh table slot until a waiter
}

void Edge::fire() {
  port::trigger(entry_->id);
}

void Edge::wait() {
  // Qualified: an unqualified detail:: here would mean weave::sv::detail, not the core one.
  // No timeout leg: fire or cancellation only. A leaf wait, so an ownerless caller (a testbench
  // process) is allowed: it has no task record to cancel and no wait-graph node to publish.
  ::weave::detail::backend_wait(entry_->id, -1, ::weave::detail::Ownerless::Allow);
}

bool Edge::bound() const noexcept {
  return entry_->bound;
}

int lookup_edge(const char* name) noexcept {
  if (name == nullptr) return -1;
  try {
    const auto& table = detail::registry();
    const auto it = table.find(name);
    return it == table.end() ? -1 : it->second.id;
  } catch (...) {
    return -1;  // Simulation code cannot handle an exception
  }
}

void claim_edge(const char* name, int ev_id) noexcept {
  if (name == nullptr || ev_id < 0) return;
  try {
    detail::Entry& entry = detail::entry_for(name);
    if (entry.id < 0) {
      entry.id = ev_id;  // The testbench created it first: adopt
    } else if (entry.id != ev_id) {
      port::destroy_event(ev_id);  // Both sides made one: keep the older id, drop the duplicate
    }
    entry.bound = true;
  } catch (...) {
    // Swallowed for the same reason: a lost claim only leaves bound() false
  }
}

}  // namespace sv
}  // namespace weave
