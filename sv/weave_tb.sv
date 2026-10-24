// Weave test skeleton: bootstraps the C++ registered tests and waits for the root task to finish.
// Usage: compile this module together with your DUT/testbench top; the simulator loads the test
// library via -sv_lib.

module weave_tb;
  timeunit 1ps; timeprecision 1ps;
  import weave_pkg::*;

  initial begin
    automatic int status;
    weave_run(status);  // run C++ test and wait until the root task is terminal
    if (status != 0) $fatal(1, "WEAVE TEST FAILED (status=%0d)", status);
    $display("WEAVE TEST PASSED");
    $finish;
  end

endmodule
