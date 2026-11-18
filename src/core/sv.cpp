// Named cross-language edges (include/weave/sv.hpp). Backend-agnostic: ids come from the port
// contract, and the name table is plain storage that the SystemVerilog testbench can publish into.

#include <map>
#include <string>
#include <weave/sv.hpp>

#include "core_internal.hpp"
#include "port.hpp"

namespace weave {
namespace sv {
namespace {

struct Entry {
  int id = -1;
  bool bound = false;  // A SystemVerilog weave_edge object exists for this name
};

// One name is one event for the whole run, so the table owns the ids and outlives every Edge
// object. std::map rather than unordered_map: entries are never moved once created, which is what
// lets bound() look a name up without touching the id.
std::map<std::string, Entry>& registry() {
  static std::map<std::string, Entry> table;
  return table;
}

Entry& entry_for(const std::string& name) {
  return registry()[name];  // Created on first mention; id stays -1 until one side makes it
}

}  // namespace

Edge::Edge(const char* name) : name_(name != nullptr ? name : "") {
  Entry& entry = entry_for(name_);
  if (entry.id < 0) entry.id = port::create_event();  // Just a fresh table slot until someone waits
  id_ = entry.id;
}

void Edge::fire() {
  port::trigger(id_);
}

void Edge::wait() {
  detail::backend_wait(id_, -1);  // No timeout leg: only a fire or a cancellation ends this
}

bool Edge::bound() const noexcept {
  const auto& table = registry();
  const auto it = table.find(name_);
  return it != table.end() && it->second.bound;
}

int lookup_edge(const char* name) noexcept {
  if (name == nullptr) return -1;
  try {
    const auto& table = registry();
    const auto it = table.find(name);
    return it == table.end() ? -1 : it->second.id;
  } catch (...) {
    return -1;  // Simulation code cannot handle an exception
  }
}

void claim_edge(const char* name, int ev_id) noexcept {
  if (name == nullptr || ev_id < 0) return;
  try {
    Entry& entry = entry_for(name);
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
