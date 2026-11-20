#pragma once

// Backend port contract: the only backend interface the core bookkeeping layer (src/core)
// depends on. Each backend implements it once under src/backend/, selected at compile time by the
// WEAVE_BACKEND_* macros (see config.hpp in this directory; SVDPI by default).
//
// Semantics every backend must guarantee:
//   1. Process model: spawn(id) creates an independent simulation process that executes
//      weave_task_main(id) (SVDPI: a fork...join_none process; SystemC: an sc_spawn dynamic
//      process). The child is scheduled only after the parent hits its next suspension point
//      (or finishes) — consistent with asyncio create_task's "scheduled, not run
//      immediately".
//   2. Single active process: at any instant at most one process executes C++ bookkeeping
//      (a wait suspension ≡ the process handing over the scheduling right; the core relies on
//      this to maintain global state).
//   3. Events: create/trigger/destroy form a one-shot wakeup mechanism; trigger must never be
//      used on an already destroyed id (the core unlinks it on every wait path).
//   4. wait is the only suspension point: a three-legged race (event / timeout / cancel), with
//      the cancel leg winning same-slot conflicts; status 0=event hit 1=timeout 2=cancelled.
//      ev_id < 0 means no event leg; timeout_ticks < 0 means no timeout leg; abort_id < 0 means no
//      cancel leg (status then never reports 2) — the shape an ownerless suspension uses.
//   5. fatal is the last port call of a run: the core has detected an unrecoverable contract
//      violation (a diagnosed wait cycle, a suspension with no task to suspend). It reports the
//      message through the backend's own failure channel and terminates the simulation on the
//      spot — it must never return and never unwind into the caller, so the run cannot continue
//      past the violation and cannot be mistaken for a pass.
//
// Two queries are part of the contract as well: now_ticks and owner_task, neither of which
// suspends. now_ticks must report the same tick unit ticks_per_sec describes — every absolute
// deadline in the public API (now/wait_until, and the periodic scheduling built on them) is derived
// from it. owner_task is the task identity, and it is per simulation *process* rather than per C++
// stack: that is the only place where the answer exists (see the declaration below).
//
// Backend bootstrap entry points (not part of the port contract, each backend picks its own form):
//   SVDPI  : extern "C" void weave_start()        —— called from systemverilog
//   SystemC : int weave::sc::run()                 —— called from sc_main

#include <cstdint>

extern "C" void weave_task_main(int task_id);  // Task dispatch entry, implemented by the core

namespace weave {
namespace port {

// Creates a simulation process that executes weave_task_main(task_id) (see the process model
// above).
void spawn(int task_id);

// One-shot wakeup mechanism. The value returned by create_event must be kept and eventually
// passed to destroy_event: dropping it leaks a backend event id (a real object allocation on the
// backend side).
void trigger(int ev_id);
[[nodiscard]] int create_event();
void destroy_event(int ev_id);

// Number of backend ticks in one second (determines the precision of the fs→tick conversion).
std::int64_t ticks_per_sec();

// Current backend time in ticks, in the same unit as ticks_per_sec (SVDPI: $time; SystemC:
// sc_time_stamp in resolution units). Non-suspending: it may be called from any context, including
// outside a task, and must not advance time. The core clamps it to be monotonically
// non-decreasing and tolerates a throwing implementation, so a backend need not be able to answer
// before the simulation starts.
std::int64_t now_ticks();

// The task id whose body this process is executing, or 0 for a process that runs no task body (a
// testbench process, a backend bootstrap callback). Non-suspending, never throws, and it must be
// answerable before the simulation starts (0).
//
// Per process, not per C++ stack: a slice of another task suspending in its own process must never
// clobber this one, whereas a nested body on this very stack is handled by the core's
// save/restore of the return value. Across a cross-language callback the two answers differ — the
// C++ stack belongs to the caller, the identity to the process — and only the backend can tell
// them apart, which is why the identity lives here and not in core bookkeeping.
int owner_task();

// Publishes owner_task's value; 0 clears it. Called by the core's dispatch entry (weave_task_main)
// around every task body, and by nothing else.
void set_owner_task(int task_id);

// The only suspension point (see semantics 4 above).
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status);

// Ends the run (see semantics 5 above). msg is NUL-terminated and outlives the call because it is
// the report the backend prints; [[noreturn]] so the core cannot keep running after reporting.
[[noreturn]] void fatal(const char* msg);

}  // namespace port
}  // namespace weave
