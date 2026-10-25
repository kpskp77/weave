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
//   waiter registered on that id and rings that waiter's private wakeup event, and the waiter
//   itself suspends in `if (w_state[wd] == 0) @(w_evt[wd])`. No fork, no disable fork, no
//   dependence on which delta forked legs would have run in, and no observation of the sticky
//   flag. The durable w_state flag is what keeps the edge from being lost: the trigger writes it
//   *before* ringing, so a waiter that has not reached its `if` yet sees it and never blocks, and
//   nothing can run between that check and the `@` (a suspension is the only scheduling point).
//   Why the wakeup edge is a per-waiter event instead of `wait (w_state[wd] != 0)` on the registry
//   (the form this backend used before): that wait references an associative array, and every
//   registration writes w_state, which makes the kernel re-evaluate the condition of *all*
//   suspended waiters referencing the array. N concurrently waiting tasks therefore cost O(N²) in
//   a zero-time phase — $time frozen, simv pegged at 100% CPU, i.e. indistinguishable from a hang
//   (measured on VCS: 10⁴ tasks 8.6 s, 2·10⁴ tasks 45.8 s). An event element is observed in O(1),
//   keeping the same phase at 0.34 s for 10⁴ and 2.3 s for 10⁵ tasks.
//   Timeouts, the only part that needs a #delay, are owned by the persistent timer service below:
//   a #delay inside the waiter would have to be forked, and a join_none child reads its captured
//   ids only after the parent frame may already be gone.
//   `->ev` stays for the two users that genuinely want the level flag: the root-done handshake and
//   the same-slot cancel precedence test.

package weave_pkg;
  timeunit 1ps; timeprecision 1ps;

  // Event table: the sole owner of SV events; the C++ side only holds int ids. Events are stored
  // directly (no class wrapper) — an associative-array element materialises on first use, so
  // create_event only hands out a fresh id and the event object comes into being on its first
  // trigger / .triggered check. This avoids a class allocation per event (the class-wrapped form
  // scales super-linearly in VCS once events number in the hundreds of thousands).
  event ev_table [int];
  int ev_next_id = 0;
  int root_done_ev = -1;

  // ── waiter registry ─────────────────────────────────────────────────────
  // One entry per suspended waiter, keyed by a never-recycled wait id (so a late deadline can never
  // be mistaken for a newer wait, and a stale wakeup can never be taken for a fresh one).
  // w_state is the durable decision — 0 = waiting, 1 = woken by an event / abort trigger,
  // 2 = woken by the timer service — and w_evt is the private edge that actually resumes the
  // waiter (see the suspension model above: observing w_state level-sensitively is O(N²)).
  int w_state   [int];
  longint w_due [int];  // absolute deadline in ticks (present only while a timeout is armed)
  event w_evt   [int];  // this waiter's wakeup edge; never observed through .triggered
  int w_next = 0;
  // Reverse index event/abort id → the wait ids registered on it, so a trigger rings exactly those
  // waiters instead of walking the whole registry (that walk is O(waiters) per trigger, i.e. also
  // quadratic for a workload that keeps N waits alive while triggering N times). Every entry is
  // unlinked in the same step of weave_sv_wait that deletes w_state.
  int w_idx [int][int];

  // Timer service rendezvous: a level counter, not an event, so a deadline registered while the
  // service is between "scan the deadlines" and "go to sleep" cannot be lost.
  int timer_poke_gen = 0;
  bit timer_started = 0;

  // ── timer deadline heap ─────────────────────────────────────────────────
  // A binary min-heap over the armed deadlines, so the service finds the earliest deadline and
  // drains the expired ones in O(log N) each. Entries are never removed out-of-band: a deadline
  // whose waiter was woken/cancelled leaves a stale entry that is discarded lazily when it
  // surfaces (heap_valid). Wait ids are never re-armed, so (due, wd) identifies an entry uniquely.
  longint t_due [$];
  int t_wd    [$];

  function automatic int heap_valid(input int i);
    return i < t_due.size() && w_due.exists(t_wd[i]) && w_due[t_wd[i]] == t_due[i];
  endfunction

  function automatic void heap_push(input longint due, input int wd);
    int i;
    int p;
    longint pd;
    int pw;
    t_due.push_back(due);
    t_wd.push_back(wd);
    i = t_due.size() - 1;
    while (i > 0) begin
      p = (i - 1) / 2;
      if (t_due[p] <= t_due[i]) break;
      pd = t_due[p]; t_due[p] = t_due[i]; t_due[i] = pd;
      pw = t_wd[p]; t_wd[p] = t_wd[i]; t_wd[i] = pw;
      i = p;
    end
  endfunction

  function automatic void heap_pop();
    int n = t_due.size();
    int i;
    int l;
    int r;
    int m;
    longint td;
    int tw;
    if (n == 0) return;
    t_due[0] = t_due[n - 1];
    t_wd[0] = t_wd[n - 1];
    t_due.pop_back();
    t_wd.pop_back();
    n = t_due.size();
    i = 0;
    while (1) begin
      l = 2 * i + 1;
      r = 2 * i + 2;
      m = i;
      if (l < n && t_due[l] < t_due[m]) m = l;
      if (r < n && t_due[r] < t_due[m]) m = r;
      if (m == i) break;
      td = t_due[i]; t_due[i] = t_due[m]; t_due[m] = td;
      tw = t_wd[i]; t_wd[i] = t_wd[m]; t_wd[m] = tw;
      i = m;
    end
  endfunction

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
  // users working, and the reverse index makes exactly the waiters registered on this id runnable.
  // Writing w_state / ringing w_evt only schedules those processes; it never runs them inline, so
  // the core's "at most one process executes C++ bookkeeping" invariant still holds.
  function automatic void weave_sv_trigger(input int ev_id);
    ->ev_table[ev_id];
    if (w_idx.exists(ev_id))
      foreach (w_idx[ev_id][wd])
        if (w_state[wd] == 0) begin
          // Durable state first, then the edge: a waiter that has not reached its @ yet sees the
          // flag and never blocks, the one already blocked is resumed by the event.
          w_state[wd] = 1;
          ->w_evt[wd];
        end
  endfunction

  function automatic int weave_sv_create_event();
    int id = ev_next_id++;
    return id;  // ev_table[id] materialises lazily on first trigger / .triggered check
  endfunction

  function automatic void weave_sv_destroy_event(input int ev_id);
    ev_table.delete(ev_id);
    w_idx.delete(ev_id);  // no waiter may survive its event's destruction (port guarantee 3)
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
  //
  // Deadlines live in the min-heap above, so "find the earliest" and "drain the expired" are each
  // O(log N). The naive two-scan over the deadline table this replaced is O(armed) per wake —
  // quadratic over a workload that staggers N deadlines, and the biggest remaining cost after the
  // wakeup-edge fix.
  task automatic weave_timer_service();
    forever begin
      int seen;
      longint now;
      longint next_due;
      int wd;
      seen = timer_poke_gen;
      now = longint'($time);
      // Drop stale tops (a deadline already consumed by a woken/cancelled waiter stays in the heap
      // until it surfaces here).
      while (t_due.size() > 0 && !heap_valid(0)) heap_pop();
      if (t_due.size() == 0) begin
        wait (timer_poke_gen != seen);  // nothing armed: sleep until a deadline shows up
        continue;
      end
      next_due = t_due[0];
      if (next_due > now) begin
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
      // Drain every expired deadline. Pop first, then act, so w_due can be mutated inline (the
      // heap, not the deadline table, is the structure being walked).
      while (t_due.size() > 0) begin
        if (!heap_valid(0)) begin
          heap_pop();
          continue;
        end
        if (t_due[0] > now) break;
        wd = t_wd[0];
        heap_pop();
        if (w_state[wd] == 0) begin
          w_state[wd] = 2;  // same durable-then-ring order as the trigger path
          ->w_evt[wd];
        end
        w_due.delete(wd);
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
    if (ev_id >= 0) w_idx[ev_id][wd] = 1;
    if (abort_id >= 0) w_idx[abort_id][wd] = 1;
    if (timeout_ticks >= 0) begin
      w_due[wd] = longint'($time) + timeout_ticks;
      heap_push(w_due[wd], wd);
      timer_poke_gen++;  // let the service shorten its sleep if this deadline comes first
    end
    // The only suspension: the registration above, the state check and the yield are one kernel
    // action, so no trigger can slip in between (the SV analogue of sc_core::wait(e | ab)).
    if (w_state[wd] == 0) @(w_evt[wd]);
    // Same decision order as src/backend/systemc.cpp after its wait: the cancel leg wins a
    // same-slot conflict, then the event leg (compared via the time-slot sticky flag, which is
    // exactly the "triggered in this slot" question), and only otherwise was it the timeout leg.
    if (abort_id >= 0 && ev_table.exists(abort_id) && ev_table[abort_id].triggered)
      status = 2;
    else if (ev_id >= 0 && ev_table.exists(ev_id) && ev_table[ev_id].triggered)
      status = 0;
    else status = 1;
    w_state.delete(wd);
    w_evt.delete(wd);
    if (ev_id >= 0 && w_idx.exists(ev_id)) begin
      w_idx[ev_id].delete(wd);
      if (w_idx[ev_id].num() == 0) w_idx.delete(ev_id);
    end
    if (abort_id >= 0 && w_idx.exists(abort_id)) begin
      w_idx[abort_id].delete(wd);
      if (w_idx[abort_id].num() == 0) w_idx.delete(abort_id);
    end
    if (w_due.exists(wd)) begin
      w_due.delete(wd);
      timer_poke_gen++;  // the service may be sleeping on a deadline that just disappeared
    end
  endtask

  // Test wait chain: the harness suspends here until the root task finishes
  task automatic weave_sv_wait_done();
    wait (ev_table[root_done_ev].triggered);
  endtask

  task automatic weave_run(output int status);
    // weave_start is the only entrypoint of C++ code, calling inside weave_pkg,
    // so there is no need to switch sv scope in C++
    weave_start();  // Bootstrap C++: create and spawn the root task
    weave_sv_wait_done();  // Suspend this process until the root task is terminal
    status = weave_test_status();
  endtask

endpackage
