// ── Weave SystemVerilog 后端 ─────────────────────────────────────────────
// 与 src/weave.cpp 的 DPI 契约：
//   export（C++→SV）：spawn/trigger/event 表/ticks/通用等待，构成唯一阻塞机制
//   import（SV→C++）：weave_run 引导、weave_task_main 任务体、weave_test_status
//
// 约定：本包 timeunit 决定 #() 与 1s 字面量的换算基准；C++ 侧据此建立
// fs→tick 换算（向上取整）。任务挂起即 SV 进程挂起，C++ 调用栈随之冻结。

package weave_pkg;
  timeunit 1ps; timeprecision 1ps;

  // 事件表：SV 事件的唯一所有者，C++ 只持 int id。
  class weave_event;
    event ev;
  endclass

  weave_event ev_table  [int];
  int ev_next_id = 0;
  int root_done_ev = -1;

  import "DPI-C" context function void weave_run();
  import "DPI-C" context task weave_task_main(input int task_id);
  import "DPI-C" context function int weave_test_status();

  export "DPI-C" function weave_sv_spawn;
  export "DPI-C" function weave_sv_trigger;
  export "DPI-C" function weave_sv_create_event;
  export "DPI-C" function weave_sv_destroy_event;
  export "DPI-C" function weave_sv_ticks_per_sec;
  export "DPI-C" function weave_sv_set_root_done_ev;
  export "DPI-C" task weave_sv_wait;
  export "DPI-C" task weave_sv_wait_done;

  // fork...join_none 的子进程要等父进程阻塞后才启动——
  // 与 asyncio create_task 的"调度而非立即执行"语义一致。
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

  // 1s 时间字面量按本包 timeunit 换算为 tick 数（timeunit=1ps → 10^12）
  function automatic longint weave_sv_ticks_per_sec();
    return 1s;
  endfunction

  function automatic void weave_sv_set_root_done_ev(input int ev_id);
    root_done_ev = ev_id;
  endfunction

  // 唯一阻塞原语。三条腿：事件 / 超时 / 取消。
  //   ev_id < 0           → 无事件腿（纯睡眠）
  //   timeout_ticks < 0   → 无超时腿（无限等待）
  // status: 0=事件命中  1=超时  2=被取消（同槽冲突时取消优先）
  task automatic weave_sv_wait(input int ev_id, input longint timeout_ticks, input int abort_id,
                               output int status);
    automatic bit ev_hit = 0;
    automatic bit to_hit = 0;
    automatic bit ab_hit = 0;
    fork
      begin : ev_leg
        if (ev_id >= 0) begin
          wait (ev_table[ev_id].ev.triggered);
          ev_hit = 1;
        end else begin
          wait (1'b0);  // 禁用：永不返回
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
          ab_hit = 1;
        end else begin
          wait (1'b0);
        end
      end
    join_any
    disable fork;
    if (ab_hit) status = 2;
    else if (to_hit) status = 1;
    else status = 0;
  endtask

  // 测试等待链：harness 在此挂起直至根任务结束
  task automatic weave_sv_wait_done();
    wait (ev_table[root_done_ev].ev.triggered);
  endtask

endpackage
