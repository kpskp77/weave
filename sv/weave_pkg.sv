// ── Weave SystemVerilog backend ──────────────────────────────────────────
// DPI contract with src/backend/svdpi.cpp:
//   export (C++→SV): spawn/trigger/event table/ticks/generic wait, forming the only blocking
//                    mechanism
//   import (SV→C++): weave_start bootstrap, weave_task_main task body, weave_test_status
//
// Convention: this package's timeunit determines the conversion base for #() and the 1s literal;
// the C++ side derives the fs→tick conversion from it (rounding up). A task suspension is an SV
// process suspension, and the C++ call stack freezes with it.

package weave_pkg;
  timeunit 1ps; timeprecision 1ps;

  // Event table: the sole owner of SV events; the C++ side only holds int ids.
  class weave_event;
    event ev;
  endclass

  weave_event ev_table  [int];
  int ev_next_id = 0;
  int root_done_ev = -1;

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

  function automatic void weave_sv_trigger(input int ev_id);
    ->ev_table[ev_id].ev;
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

  // The only blocking primitive. Three legs: event / timeout / cancel.
  //   ev_id < 0           → no event leg (pure sleep)
  //   timeout_ticks < 0   → no timeout leg (wait forever)
  // status: 0=event hit  1=timeout  2=cancelled (cancel wins same-slot conflicts)
  task automatic weave_sv_wait(input int ev_id, input longint timeout_ticks, input int abort_id,
                               output int status);
    automatic bit to_hit = 0;
    fork
      begin : ev_leg
        if (ev_id >= 0) begin
          wait (ev_table[ev_id].ev.triggered);
        end else begin
          wait (1'b0);  // Disabled: never returns
        end
      end
      begin : to_leg
        if (timeout_ticks >= 0) begin
          #(timeout_ticks);
          to_hit = 1;
        end else begin
          wait (1'b0);
        end
      end
      begin : ab_leg
        if (abort_id >= 0) begin
          wait (ev_table[abort_id].ev.triggered);
        end else begin
          wait (1'b0);
        end
      end
    join_any
    disable fork;
    // On a same-slot conflict the cancel leg wins (same test as ab.triggered() in
    // src/backend/systemc.cpp). Note that the flag set by ab_leg cannot be relied on alone: when
    // two events trigger in the same time slot, join_any may return before ab_leg executes, and
    // disable fork then stops that leg before the flag is set.
    if (abort_id >= 0 && ev_table.exists(abort_id) && ev_table[abort_id].ev.triggered)
      status = 2;
    else if (to_hit) status = 1;
    else status = 0;  // ev_hit: the event leg hit (the only remaining possibility)
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
