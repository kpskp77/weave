#pragma once

// Backend selection: a compile-time macro, exactly one of the two; the build system defines the
// matching macro via `xmake f --backend=...`.
//   WEAVE_BACKEND_SVDPI    SystemVerilog DPI backend (default; VCS/IES/Xcelium)
//   WEAVE_BACKEND_SYSTEMC  SystemC backend (sc_spawn/sc_event; entry point weave::sc::run())
//
// For hand-written builds, define neither macro and this header enables SVDPI by default.

#if defined(WEAVE_BACKEND_SVDPI) && defined(WEAVE_BACKEND_SYSTEMC)
#error "define at most one of WEAVE_BACKEND_SVDPI / WEAVE_BACKEND_SYSTEMC"
#endif

#if !defined(WEAVE_BACKEND_SVDPI) && !defined(WEAVE_BACKEND_SYSTEMC)
#define WEAVE_BACKEND_SVDPI 1
#endif
