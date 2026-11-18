// Weave testbench top for the VCS regression — NOT library source: the DPI protocol lives in
// sv/weave_pkg.sv, which must always be compiled together with this module. This file only
// bootstraps the C++ tests and waits for the root task to finish, and lives next to the driver
// that compiles it (tests/svdpi_driver.cpp).
// Usage: compile this module together with your DUT/testbench top; the simulator loads the test
// library via -sv_lib.

module weave_tb;
  timeunit 1ps; timeprecision 1ps;
  import weave_pkg::*;

  // ── cross-language handle probe (include/weave/sv.hpp) ────────────────────
  // Two raw event ids, created here because a testbench cannot read a C++ variable and C++ holds no
  // symbol for a testbench function; they are handed in by tag before the run starts. The responders
  // are both registered at time 0, so the C++ side can fire at any moment and be sure the other
  // language is already listening. Inert for payloads that never claim a tag (the examples payload
  // does), where the two processes simply stay pending until $finish.
  int cl_request = -1;  // Tag 0: C++ fires it, answered 10ns later on the same id
  int cl_ack = -1;      // Tag 1: C++ fires it, answered immediately on the same id

  initial begin
    automatic int status;
    cl_request = weave_sv_create_event();
    cl_ack = weave_sv_create_event();
    weave_sv_hand_off(0, cl_request);
    weave_sv_hand_off(1, cl_ack);
    // Both legs are forked before the run starts and registered in the first delta cycle the run
    // suspends in, so the C++ side can fire at any point of the run and find the other language
    // already listening.
    fork
      begin : cl_phase_request
        @(ev_table[cl_request]);
        #10ns;
        weave_sv_trigger(cl_request);
      end
      begin : cl_phase_ack
        @(ev_table[cl_ack]);
        weave_sv_trigger(cl_ack);
      end
    join_none
    weave_run(status);  // run C++ test and wait until the root task is terminal
    if (status != 0) $fatal(1, "WEAVE TEST FAILED (status=%0d)", status);
    $display("WEAVE TEST PASSED");
    $finish;
  end

endmodule
