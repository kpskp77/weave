// Weave 测试骨架：引导 C++ 注册测试并等待根任务完成。
// 用法：将此模块与你的 DUT/testbench 顶层一起编译，仿真器通过 -sv_lib 加载测试库。

module weave_tb;
  timeunit 1ps; timeprecision 1ps;
  import weave_pkg::*;

  initial begin
    weave_run();  // 引导 C++：创建并 spawn 根任务
    weave_sv_wait_done();  // 挂起本进程直至根任务终态
    if (weave_test_status() != 0) $fatal(1, "WEAVE TEST FAILED (status=%0d)", weave_test_status());
    $display("WEAVE TEST PASSED");
    $finish;
  end

endmodule
