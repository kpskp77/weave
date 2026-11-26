#pragma once

// Named edges shared with SystemVerilog: one edge both languages fire and wait on, bound by name.
// A weave::Event allocates a fresh wake event per waiter (that is what makes it safe for many
// waiters), so it has no stable id to hand out; sv::Edge is the stable-id counterpart. Its
// semantics are the port's edge semantics — nothing is latched, so a fire() that happens before the
// peer waits is lost, exactly like `->e` on a SystemVerilog event:
//
//   C++                                SystemVerilog (sv/weave_pkg.sv, class weave_edge)
//   sv::Edge e{"clk"}                  weave_edge e = new("clk")
//   e.fire()                           weave_sv_trigger(e.id)  /  ->ev_table[e.id]
//   e.wait()                           e.wait_edge()  /  weave_sv_wait(e.id, -1, -1, st)
//
// The name is the binding: one name is one backend event for the whole run, whichever side creates
// it first. C++ registers the id in its constructor; SystemVerilog asks for it by name
// (weave_sv_edge_id) and publishes its own when C++ was not there yet (weave_sv_edge_bind). Nothing
// is destroyed — the lifetime is the run's, so the "no waiter may outlive its event" discipline of
// a raw handle does not apply here.
//
// Under the SystemC backend an Edge is a plain sc_event and no testbench can claim it (bound()
// stays false), so a payload that must work on both backends takes its cross-language branch on
// bound().
//
// An Edge is a handle, not the event: the per-name entry lives in the name table for the whole run,
// and Edge holds one pointer to it (8 bytes, trivially copyable, no allocation of its own). Copies
// are free and — exactly like the backend id they share — two Edges built from one name stay one
// edge, so copying an Edge into a task body is the natural gesture (see case2 in
// tests/sv_edge_semantics.cpp). Note what this is *not*: the Event::Impl pattern, which gives each
// object a private impl; here the impl is per *name*, because sharing the backend event is the
// documented semantics.

namespace weave {
namespace sv {

namespace detail {
struct Entry;  // The per-name entry the table owns (src/core/sv.cpp); never erased, never moved
}

class Edge {
public:
  // Registers name, creating the backend event if neither side did yet. Never suspends.
  explicit Edge(const char* name);

  // Wake every waiter registered on this edge right now, in either language. Never suspends.
  void fire();

  // Suspend the current task until this edge is fired by the other language or by fire(). Edge
  // semantics: a fire that already happened does not wake a later wait. There is no timeout leg —
  // wrap with weave::timeout(body, d) when a bound is needed — and the wait takes part in
  // cancellation like every other suspension.
  void wait();

  // True once a SystemVerilog weave_edge object exists for this name: always false under the
  // SystemC backend, and false under SVDPI until the testbench claims it.
  [[nodiscard]] bool bound() const noexcept;

private:
  // Points into the name table: stable for the run (the table is a std::map and nothing is ever
  // erased), so fire()/wait()/bound() never look the name up again.
  detail::Entry* entry_;
};

// ── called from SystemVerilog ─────────────────────────────────────────────
// The id registered for name, or -1 when neither side has created it yet; and the inverse, where
// the testbench publishes the id it created and marks the name as claimed. Exported as
// weave_sv_edge_id / weave_sv_edge_bind, i.e. they run in simulation code that cannot handle an
// exception: failures are swallowed (a lost claim only leaves bound() false).

int lookup_edge(const char* name) noexcept;
void claim_edge(const char* name, int ev_id) noexcept;

}  // namespace sv
}  // namespace weave
