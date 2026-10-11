#pragma once

// 后端选择：编译期宏，二选一；构建系统通过 `xmake f --backend=...` 定义对应宏。
//   WEAVE_BACKEND_SVDPI    SystemVerilog DPI 后端（默认；VCS/IES/Xcelium）
//   WEAVE_BACKEND_SYSTEMC   SystemC 后端（sc_spawn/sc_event；入口 weave::sc::run()）
//
// 手写构建时不定义任何宏，本头文件默认启用 SVDPI。

#if defined(WEAVE_BACKEND_SVDPI) && defined(WEAVE_BACKEND_SYSTEMC)
#error "define at most one of WEAVE_BACKEND_SVDPI / WEAVE_BACKEND_SYSTEMC"
#endif

#if !defined(WEAVE_BACKEND_SVDPI) && !defined(WEAVE_BACKEND_SYSTEMC)
#define WEAVE_BACKEND_SVDPI 1
#endif
