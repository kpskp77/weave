// Weave testbench top for the VCS regression — NOT library source: the DPI protocol lives in
// sv/weave_pkg.sv, which must always be compiled together with this module. This file only
// bootstraps the C++ tests, drives the named-edge probe, and waits for the root task to finish, and
// lives next to the driver that compiles it (tests/svdpi_driver.cpp).
// Usage: compile this module together with your DUT/testbench top; the simulator loads the test
// library via -sv_lib.

// Consumer-scope stand-in for tests/svdpi_scope_semantics.cpp: this module deliberately imports
// nothing, so a process whose DPI scope is this instance cannot see weave_pkg's exports. The test
// switches the DPI scope here to reproduce "C++ reached through a consumer's own DPI import"; the
// backend has to fix the scope itself (src/backend/svdpi.cpp).
module weave_foreign_scope;
  timeunit 1ps; timeprecision 1ps;
  bit alive = 1'b1;  // keeps the instance from being elided
endmodule

module weave_tb;
  timeunit 1ps; timeprecision 1ps;
  import weave_pkg::*;

  weave_foreign_scope u_foreign_scope();

  // ── named-edge probe (include/weave/sv.hpp) ──────────────────────────────
  // Three edges shared with C++ by name. cl_request / cl_ack are created here before the run starts
  // (SystemVerilog-first: C++ adopts these ids), with their responders registered in the first
  // delta cycle the run suspends in, so the C++ side can fire at any moment of the run and find the
  // other language already listening. cpp_first is created only after C++ has asked for it
  // (C++-first: this side must adopt the id C++ registered) and is echoed back on the same edge.
  // Inert for payloads that never touch these names (the examples payload does not): the processes
  // simply stay pending until $finish.
  weave_edge cl_request;  // Answered 10ns after a fire
  weave_edge cl_ack;  // Answered immediately
  weave_edge probe_gate;  // Opened by C++ right after it registers "cpp_first"

  initial begin
    automatic int status;
    cl_request = new("cl_request");
    cl_ack = new("cl_ack");
    probe_gate = new("probe_gate");
    // Forked before the run starts, so the legs are registered in the first delta cycle the run
    // suspends in.
    fork
      begin : cl_phase_request
        cl_request.wait_edge();
        #10ns;
        cl_request.fire();
      end
      begin : cl_phase_ack
        cl_ack.wait_edge();
        cl_ack.fire();
      end
      begin : cpp_first_probe
        automatic weave_edge adopted;
        probe_gate.wait_edge();
        if (weave_sv_edge_id("cpp_first") < 0) $fatal(1, "C++ did not register cpp_first first");
        adopted = new("cpp_first");  // Adopts, does not create
        adopted.fire();  // Echo on the same edge: the id is shared, not a copy
      end
    join_none
    weave_run(status);  // run C++ test and wait until the root task is terminal
    if (status != 0) $fatal(1, "WEAVE TEST FAILED (status=%0d)", status);
    $display("WEAVE TEST PASSED");
    $finish;
  end

endmodule
