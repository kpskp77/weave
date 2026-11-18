#pragma once

// Cross-language event handles: the raw backend event id, spelled as a value both languages can
// hold. A weave::Event allocates a fresh wake event per waiter (that is what makes it safe for many
// waiters), so it has no stable id to hand out; EventHandle is the opposite deal — one id, one
// edge, owned by whoever made it, and both sides of the DPI boundary speak the same integer:
//
//   C++                                    SystemVerilog (sv/weave_pkg.sv)
//   sv::make_event()                       weave_sv_create_event()
//   sv::fire(h)                            weave_sv_trigger(h.id)
//   sv::wait(h)                            @(ev_table[h.id])  /  weave_sv_wait(h.id, …)
//   sv::destroy(h)                         weave_sv_destroy_event(h.id)
//
// The id crosses the boundary as an int: the testbench creates ids and hands them in through the
// fixed table below (weave_sv_hand_off), C++ picks them up with handed(tag). Under the SystemC
// backend the same calls are simply sc_events — nothing here is SV-specific machinery,
// SystemVerilog is just the other language that happens to speak the same ids.
//
// Discipline (the port contract's, in raw form): fire() wakes the waiters registered at that moment
// and leaves no state, so a fire before a wait is lost; nobody may still be waiting when destroy()
// runs; a handle is single-owner (the maker destroys it).

namespace weave {
namespace sv {

struct EventHandle {
  int id = -1;  // Backend event id; -1 = invalid handle

  explicit operator bool() const noexcept { return id >= 0; }
};

// A fresh edge, owned by the caller (destroy it). In SVDPI this is weave_sv_create_event.
EventHandle make_event();

// Wake every waiter registered on this edge right now, in either language. Never suspends.
void fire(EventHandle h);

// Release the id. No waiter may survive its event's destruction (port guarantee 3).
void destroy(EventHandle h);

// Suspend the current task until this edge is fired (by the other language or by fire()). Edge
// semantics: no sticky state, so a fire that already happened does not wake a later wait.
void wait(EventHandle h);

// ── hand-over ────────────────────────────────────────────────────────────
// A SystemVerilog testbench cannot read a C++ variable, and the C++ run cannot call a function
// whose symbol it does not have, so ids cross at bootstrap instead: the testbench creates them and
// fills this table before it starts the run (weave_sv_hand_off(tag, id)); C++ adopts them by tag.
inline constexpr int kHandOverSlots = 8;

// Called from SystemVerilog (exported as weave_sv_hand_off). Out-of-range tags are ignored — the
// testbench is the one place where silently dropping a value is better than throwing into a
// simulator.
void hand_off(int tag, int ev_id) noexcept;

// The handle handed in under tag, or an invalid handle when the slot is empty (no testbench, e.g.
// under the SystemC backend). Never suspends.
EventHandle handed(int tag) noexcept;

}  // namespace sv
}  // namespace weave
