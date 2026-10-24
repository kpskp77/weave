// ── Weave SystemVerilog backend ──────────────────────────────────────────
// DPI contract with src/backend/svdpi.cpp:
//   export (C++→SV): spawn/trigger/event table/ticks/generic wait, forming the only blocking
//                    mechanism
//   import (SV→C++): weave_start bootstrap, weave_task_main task body, weave_test_status
//
// Convention: this package's timeunit determines the conversion base for #() and the 1s literal;
// the C++ side derives the fs→tick conversion from it (rounding up). A task suspension is an SV
// process suspension, and the C++ call stack freezes with it.
//
// Suspension model — why weave_sv_wait contains no fork:
//   The port contract (src/core/port.hpp, guarantee 4) asks for edge semantics: a wait may report
//   "event hit" only for a trigger that happens after it started waiting. SystemC gets that for
//   free — sc_core::wait(e | ab) registers and yields in one kernel action, and
//   sc_event::triggered() is delta-scoped. SystemVerilog has no such primitive, and the fork
//   …join_any approximation leaks on both halves:
//     · the legs are scheduled in a later delta, so registration is not atomic: a trigger can land
//       in between, and a `disable fork` issued by another wait kills legs that never got to run;
//     · the only observation primitive, wait (ev.triggered), is sticky for the whole time slot, so
//       a trigger older than the wait reads as a fresh hit. The core legitimately reuses one raw
//       event id across consecutive waits (as_completed's wake_ev, wait's wake_ev, TaskGroup's
//       wake_ev/drain_ev); with sticky observation those waits never block and the C++ side spins
//       in a zero-delay loop ($time frozen, simv at 100% CPU).
//   Therefore wakeups are delivered by a synchronous registry write instead: weave_sv_trigger()
//   — reached from the trigger path, i.e. synchronously, like sc_event::notify() — marks every
//   waiter registered on that id, and the waiter itself suspends in a single
//   `wait (w_state[wd] != 0)`, which registers and yields in one action. No fork, no
//   disable fork, no dependence on which delta forked legs would have run in, and no observation of
//   the sticky flag. Timeouts, the only part that needs a #delay, are owned by the persistent timer
//   service below: a #delay inside the waiter would have to be forked, and a join_none child reads
//   its captured ids only after the parent frame may already be gone.
//   `->ev` stays for the two users that genuinely want the level flag: the root-done handshake and
//   the same-slot cancel precedence test.

package weave_pkg;
  timeunit 1ps; timeprecision 1ps;

  // Event table: the sole owner of SV events; the C++ side only holds int ids.
  class weave_event;
    event ev;
  endclass

  weave_event ev_table  [int];
  int ev_next_id = 0;
  int root_done_ev = -1;

  // ── waiter registry ─────────────────────────────────────────────────────
  // One entry per suspended waiter, keyed by a never-recycled wait id (so a late deadline can never
  // be mistaken for a newer wait). w_state is the only thing a waiter blocks on:
  // 0 = waiting, 1 = woken by an event / abort trigger, 2 = woken by the timer service.
  int w_state [int];
  int w_ev    [int];    // event id this waiter blocks on (-1 = no event leg)
  int w_ab    [int];    // abort id this waiter blocks on (-1 = no cancel leg)
  longint w_due [int];  // absolute deadline in ticks (present only while a timeout is armed)
  int w_next = 0;

  // Timer service rendezvous: a level counter, not an event, so a deadline registered while the
  // service is between "scan the deadlines" and "go to sleep" cannot be lost.
  int timer_poke_gen = 0;
  bit timer_started = 0;

  import "DPI-C" context function void weave_start();
  import "DPI-C" context task weave_task_main(input int task_id);
  import "DPI-C" function int weave_test_status();

  export "DPI-C" function weave_sv_spawn;
  export "DPI-C" function weave_sv_trigger;
  export "DPI-C" function weave_sv_create_event;
  export "DPI-C" function weave_sv_destroy_event;
  export "DPI-C" function weave_sv_ticks_per_sec;
  export "DPI-C" function weave_sv_set_root_done_ev;
  export "DPI-C" task weave_sv_wait;
  export "DPI-C" task weave_sv_wait_done;

  // A fork...join_none child does not start until the parent blocks — consistent with asyncio
  // create_task's "scheduled, not run immediately" semantics.
  function automatic int weave_sv_spawn(input int task_id);
    fork
      begin
        automatic int id = task_id;
        weave_task_main(id);
      end
    join_none
    return 0;
  endfunction

  // Synchronous wakeup (≡ sc_event::notify): the event table trigger keeps wait (ev.triggered)
  // users working, and the registry walk makes exactly the waiters registered on this id runnable.
  // Writing w_state only schedules those processes; it never runs them inline, so the core's "at
  // most one process executes C++ bookkeeping" invariant still holds.
  function automatic void weave_sv_trigger(input int ev_id);
    ->ev_table[ev_id].ev;
    foreach (w_state[wd])
      if (w_state[wd] == 0 && (w_ev[wd] == ev_id || w_ab[wd] == ev_id)) w_state[wd] = 1;
  endfunction

  function automatic int weave_sv_create_event();
    int id = ev_next_id++;
    ev_table[id] = new();
    return id;
  endfunction

  function automatic void weave_sv_destroy_event(input int ev_id);
    ev_table.delete(ev_id);
  endfunction

  // The 1s time literal converts to a tick count using this package's timeunit
  // (timeunit=1ps → 10^12)
  function automatic longint weave_sv_ticks_per_sec();
    return 1s;
  endfunction

  function automatic void weave_sv_set_root_done_ev(input int ev_id);
    root_done_ev = ev_id;
  endfunction

  // ── timer service ───────────────────────────────────────────────────────
  // The only #delay in the backend. Persistent process: it sleeps until the earliest deadline, then
  // fires every expired one (w_state = 2). A deadline registered earlier than the one it is
  // currently sleeping on wakes it early through timer_poke_gen; both branches are level-sensitive,
  // so a poke that arrives just before the service blocks is never lost.
  task automatic weave_timer_service();
    forever begin
      int seen = timer_poke_gen;
      longint now = longint'($time);
      longint next_due = 0;
      bit have = 0;
      int expired[$];
      foreach (w_due[wd]) begin
        if (!have || w_due[wd] < next_due) begin
          next_due = w_due[wd];
          have = 1;
        end
      end
      if (!have) begin
        wait (timer_poke_gen != seen);  // nothing armed: sleep until a deadline shows up
      end else if (next_due > now) begin
        fork
          begin : tm_sleep
            #(next_due - now);
          end
          begin : tm_poke
            wait (timer_poke_gen != seen);
          end
        join_any
        // Service-local legs only: this process is started from a task that is not inside a DPI
        // call, so disabling its forks cannot reach another waiter's frame.
        disable fork;
      end
      now = longint'($time);
      // Collect first: the deadline table must not be modified while it is being iterated
      foreach (w_due[wd])
        if (w_due[wd] <= now) expired.push_back(wd);
      foreach (expired[i]) begin
        if (w_state.exists(expired[i]) && w_state[expired[i]] == 0) w_state[expired[i]] = 2;
        w_due.delete(expired[i]);
      end
    end
  endtask

  task automatic weave_timer_start_once();
    if (!timer_started) begin
      timer_started = 1;
      fork
        weave_timer_service();
      join_none
    end
  endtask

  // The only blocking primitive. Three legs: event / timeout / cancel.
  //   ev_id < 0           → no event leg (pure sleep)
  //   timeout_ticks < 0   → no timeout leg (wait forever)
  // status: 0=event hit  1=timeout  2=cancelled (cancel wins same-slot conflicts)
  task automatic weave_sv_wait(input int ev_id, input longint timeout_ticks, input int abort_id,
                               output int status);
    int wd;
    weave_timer_start_once();
    wd = w_next++;
    w_state[wd] = 0;
    w_ev[wd] = ev_id;
    w_ab[wd] = abort_id;
    if (timeout_ticks >= 0) begin
      w_due[wd] = longint'($time) + timeout_ticks;
      timer_poke_gen++;  // let the service shorten its sleep if this deadline comes first
    end
    // The only suspension: the registration above and the yield are one kernel action, so no
    // trigger can slip in between (the SV analogue of sc_core::wait(e | ab)).
    wait (w_state[wd] != 0);
    // Same decision order as src/backend/systemc.cpp after its wait: the cancel leg wins a
    // same-slot conflict, then the event leg (compared via the time-slot sticky flag, which is
    // exactly the "triggered in this slot" question), and only otherwise was it the timeout leg.
    if (abort_id >= 0 && ev_table.exists(abort_id) && ev_table[abort_id].ev.triggered)
      status = 2;
    else if (ev_id >= 0 && ev_table.exists(ev_id) && ev_table[ev_id].ev.triggered)
      status = 0;
    else status = 1;
    w_state.delete(wd);
    w_ev.delete(wd);
    w_ab.delete(wd);
    if (w_due.exists(wd)) begin
      w_due.delete(wd);
      timer_poke_gen++;  // the service may be sleeping on a deadline that just disappeared
    end
  endtask

  // Test wait chain: the harness suspends here until the root task finishes
  task automatic weave_sv_wait_done();
    wait (ev_table[root_done_ev].ev.triggered);
  endtask

  task automatic weave_run(output int status);
    // weave_start is the only entrypoint of C++ code, calling inside weave_pkg,
    // so there is no need to switch sv scope in C++
    weave_start();  // Bootstrap C++: create and spawn the root task
    weave_sv_wait_done();  // Suspend this process until the root task is terminal
    status = weave_test_status();
  endtask

endpackage
