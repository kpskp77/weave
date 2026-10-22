// Weave test skeleton: bootstraps the C++ registered tests and waits for the root task to finish.
// Usage: compile this module together with your DUT/testbench top; the simulator loads the test
// library via -sv_lib.

module weave_tb;
  timeunit 1ps; timeprecision 1ps;
  import weave_pkg::*;

  initial begin
    weave_run();  // Bootstrap C++: create and spawn the root task
    weave_sv_wait_done();  // Suspend this process until the root task is terminal
    if (weave_test_status() != 0) $fatal(1, "WEAVE TEST FAILED (status=%0d)", weave_test_status());
    $display("WEAVE TEST PASSED");
    $finish;
  end

endmodule
