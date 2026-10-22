# Weave backends

A backend is an implementation of the port contract (see [architecture.md](architecture.md)). This
document is for people who need to switch, integrate, or add a backend.

## Backend overview

| Macro | Backend | Bootstrap entry | Status |
|---|---|---|---|
| `WEAVE_BACKEND_SVDPI` (default) | SystemVerilog DPI (VCS/IES/Xcelium) | `extern "C" void weave_run()`, called by sv/weave_tb.sv | Verified on a real VCS kernel (weave_smoke_svdpi_driver; auto-SKIPs without VCS) |
| `WEAVE_BACKEND_SYSTEMC` | SystemC (xmake-repo 3.0.2) | `int weave::sc::run()`, called from sc_main | Verified on a real kernel (weave_smoke_sc) |

Switching:

```bash
xmake f --backend=systemc && xmake   # default is svdpi
```

Manual builds: just define the corresponding macro; with no macro defined,
`include/weave/config.hpp` defaults to SVDPI.

## The SVDPI backend

The DPI boundary (`src/backend/svdpi.cpp` + `sv/weave_pkg.sv`):

- **imports (provided by SV)**: `weave_sv_spawn` (creates a process via fork...join_none),
  `weave_sv_trigger`, `weave_sv_create_event` / `destroy_event` (event table),
  `weave_sv_ticks_per_sec`, `weave_sv_set_root_done_ev`, `weave_sv_wait` (three-legged wait, the only
  blocking primitive).
- **exports (provided by C++)**: `weave_task_main` (task dispatch, reachable from SV via
  weave_sv_spawn), `weave_run` / `weave_test_status` (tb bootstrap).

How to run it by hand (for manual debugging; `xmake examples` / `xmake smoke` perform the same steps
automatically):

```bash
vcs -full64 -sverilog sv/weave_pkg.sv sv/weave_tb.sv \
    -sv_lib build/linux/x86_64/release/weave \
    -sv_lib build/linux/x86_64/release/weave_examples_svdpi
./simv   # expected output: "WEAVE TEST PASSED"
```

Automated regression: `tests/svdpi_driver.cpp` is the only VCS driver, compiled into two targets that
differ only in payload:

| Target | `-sv_lib` payload | Contents | Entry |
|---|---|---|---|
| `weave_smoke_svdpi_driver` | `libweave_smoke_svdpi` | `tests/` assertion bodies | `xmake smoke` |
| `weave_examples_svdpi_driver` | `libweave_examples_svdpi` | `examples/` programs | `xmake examples` |

Both locate vcs themselves, compile the skeleton above, run simv, and judge Pass by the output marker
`WEAVE TEST PASSED`; when vcs cannot be found (`VCS_HOME`/`PATH`) they print SKIP and exit with 0.

## The SystemC backend

`src/backend/systemc.cpp` maps the port onto SystemC primitives:

- `spawn` → an `sc_spawn` dynamic process (the child runs only after the parent suspends, consistent
  with fork...join_none semantics)
- events → `sc_event`; the three-legged `wait` → `sc_event_or_list` + kernel timed wait
- time → `sc_get_time_resolution` determines the fs→tick conversion

Integration (the test side supplies sc_main) — note that xmake-repo's SystemC looks for `sc_main`
with **C linkage**:

```cpp
#include <weave/weave.hpp>
extern "C" int sc_main(int, char**);
extern "C" int sc_main(int, char**) { return weave::sc::run(); }
// run(): spawns the root task and the reporting process → sc_start() until sc_stop()
// → returns root_status() as the process exit code; if the SystemC on your target platform
// uses C++ linkage, drop extern "C"
```

**ABI notes (exception types across DSOs)**: `Cancelled` / `Timeout` are thrown by the library and
caught by user code. On Linux/Android, libc++abi follows the Itanium ABI and assumes type
information is globally unique (`type_info` is compared by pointer), so both sides must share **the
same** typeinfo object. Writing inline definitions directly in a public header makes GCC/Clang treat
them as vague linkage: every TU that uses the type emits a weak definition; and since, for instance,
xmake `mode.release` sets `symbols = hidden` by default on all non-shared targets
(`-fvisibility=hidden -fvisibility-inlines-hidden`), the copy inside the consumer executable becomes
a **local symbol** — different from the library's copy, so every `catch` silently misses (no error at
link or start-up; the exception just flies out to the outermost level).

The fix lives in the **type definitions**, not in build flags: `what()` for both types is **declared
only in the public header and defined out-of-line in the library** (`src/core/sync.cpp` /
`src/core/task.cpp`). An out-of-line, non-inline virtual function acts as the **key function**, so the
vtable and typeinfo are emitted exactly once, in that library TU; consumer TUs keep only undefined
references that the dynamic linker resolves to that same copy at load time. Consequently:

- Consumers need **no** `-fvisibility=default` and **no** `-rdynamic` (the repository's
  `weave_examples_sc` target has neither, and it still works in release mode with
  `-fvisibility=hidden`);
- How to check: `nm -D <exe> | grep _ZTIN5weave` should be all `U` (undefined), with `D`/`V`
  definitions appearing only in `libweave.so`; if `what()` is ever moved back to an inline
  definition, definitions reappear here and `catch` regresses to mismatch;
- The cost: `Cancelled` thereby becomes a polymorphic type (with a virtual destructor, no longer
  trivially copyable); its invariants reduce to "borrowed view, no heap members, nothrow copy" (the
  `static_assert`s in `task.hpp` watch exactly these three things).

Loading the `weave` and example/test libraries via `-sv_lib` (the VCS scenario) works the same way:
the single copy of RTTI inside the library is shared through the dynamic linker.

## Adding a backend

1. Add a macro to `include/weave/config.hpp` (including the mutual-exclusion check and default
   logic).
2. Create `src/backend/<name>.cpp`, the whole file guarded by
   `#if defined(WEAVE_BACKEND_<NAME>)`, implementing the 7 `weave::port` primitives plus a bootstrap
   entry point of your choice (see svdpi.cpp / systemc.cpp).
3. xmake mapping: add a value to the `--backend` option, compare `get_config` inside the target to
   define the macro and the dependencies.
4. Regression: `xmake smoke` must be fully green (name any new self-check target `weave_smoke_*` and
   add it to the build list of the `smoke` task).
