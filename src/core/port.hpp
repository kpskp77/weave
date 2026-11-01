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
//      ev_id < 0 means no event leg; timeout_ticks < 0 means no timeout leg.
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

// The only suspension point (see semantics 4 above).
void wait(int ev_id, std::int64_t timeout_ticks, int abort_id, int* status);

}  // namespace port
}  // namespace weave
