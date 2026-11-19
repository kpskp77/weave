# Weave backends

A backend is an implementation of the port contract (see [architecture.md](architecture.md)). This
document is for people who need to switch, integrate, or add a backend.

## Backend overview

| Macro | Backend | Bootstrap entry | Status |
|---|---|---|---|
| `WEAVE_BACKEND_SVDPI` (default) | SystemVerilog DPI (VCS/IES/Xcelium) | `extern "C" void weave_start()`, called from the `weave_run` task in sv/weave_pkg.sv | Verified on a real VCS kernel (weave_smoke_svdpi_vcs; auto-SKIPs without VCS) |
| `WEAVE_BACKEND_SYSTEMC` | SystemC (xmake-repo 3.0.2) | `int weave::sc::run()`, called from sc_main | Verified on a real kernel (weave_smoke_sc) |

Switching:

```bash
xmake f --backend=systemc && xmake   # default is svdpi
```

Manual builds: just define the corresponding macro; with no macro defined,
`src/core/config.hpp` defaults to SVDPI.

## The SVDPI backend

The DPI boundary (`src/backend/svdpi.cpp` + `sv/weave_pkg.sv`):

- **imports (provided by SV)**: `weave_sv_spawn` (creates a process via fork...join_none),
  `weave_sv_trigger` (one-shot wakeup: fires the event and marks the waiters registered on that id),
  `weave_sv_create_event` / `destroy_event` (event table), `weave_sv_ticks_per_sec`,
  `weave_sv_now_ticks` (current `$time` in ticks — a plain query, no suspension),
  `weave_sv_current_task` / `weave_sv_set_owner` (the task identity of the calling process, see
  below), `weave_sv_set_root_done_ev`, `weave_sv_wait` (three-legged wait, the only blocking primitive:
  register in the package's waiter registry, suspend on the waiter's own wakeup event
  `if (w_state[wd] == 0) @(w_evt[wd])` — a durable flag plus a per-waiter edge, because a
  level-sensitive `wait` on the registry array costs O(N²) on VCS (see the `sv/weave_pkg.sv`
  header); the timeout leg is covered by the persistent timer service, whose deadlines sit in a
  min-heap so "find the earliest" and "drain the expired" are O(log N) each rather than the
  O(armed) two-scan — never `fork … join_any`, see the "Pitfalls" section of
  [../AGENTS.md](../AGENTS.md)).
- **exports (provided by C++)**: `weave_task_main` (task dispatch, reachable from SV via
  weave_sv_spawn), `weave_start` / `weave_test_status` (tb bootstrap; the package's `weave_run` task
  calls them, so the DPI scope is already `weave_pkg` and no `svSetScope` is needed),
  `weave_sv_edge_id` / `weave_sv_edge_bind` (a named `sv::Edge` and the testbench's `weave_edge`
  meet on one id, see below).

### Named edges shared with SystemVerilog

A `weave::Event` allocates a fresh wake event per waiter (that is what makes it safe for many
waiters), so it has no stable id to show a testbench. `weave::sv::Edge` (include/weave/sv.hpp) is
the stable-id counterpart: one name is one backend event for the whole run, and both languages fire
and wait on it with `Pulse` semantics — nothing is latched, so a fire nobody waits for is lost,
which is exactly what `->e` / `@e` do on a SystemVerilog `event`.

| C++ | SystemVerilog (`class weave_edge`) |
|---|---|
| `sv::Edge e{"clk"}` | `weave_edge e = new("clk")` |
| `e.fire()` | `weave_sv_trigger(e.id)` / `->ev_table[e.id]` |
| `e.wait()` | `e.wait_edge()` / `weave_sv_wait(e.id, -1, -1, st)` / `@(ev_table[e.id])` |

Only one direction of id exchange is cheap — a testbench cannot read a C++ variable, and C++ holds
no symbol for a testbench function — so the *name* carries the binding and the id follows: a C++
`sv::Edge` constructor registers the id it creates, `weave_edge::new` asks for it by name
(`weave_sv_edge_id`) and publishes its own only when C++ was not there yet (`weave_sv_edge_bind`).
Either creation order therefore works, and `bound()` reports whether the testbench has claimed the
name — always false under the SystemC backend, where an `Edge` is a plain `sc_event`, which is how a
backend-agnostic payload decides whether to take its cross-language branch (cases1 and 4 in
tests/sv_edge_semantics.cpp, driven by tests/weave_tb.sv). Nothing is destroyed: the lifetime is the
run's, so the "no waiter may outlive its event" rule of the raw primitive does not apply here.

How to run it by hand (for manual debugging; `just examples` / `just smoke` perform the same steps
automatically):

```bash
vcs -full64 -sverilog sv/weave_pkg.sv tests/weave_tb.sv \
    -sv_lib build/linux/x86_64/release/weave \
    -sv_lib build/linux/x86_64/release/weave_examples_svdpi
./simv   # expected output: "WEAVE TEST PASSED"
```

Automated regression: `tests/svdpi/run.sh` is the only VCS runner (a bash script in the simlog
arrangement), executed by two phony xmake targets that differ only in payload:

| Target | `-sv_lib` payload | Contents | Entry |
|---|---|---|---|
| `weave_smoke_svdpi_vcs` | `libweave_smoke_svdpi` | `tests/` assertion bodies | `just smoke` |
| `weave_examples_svdpi_vcs` | `libweave_examples_svdpi` | `examples/` programs | `just examples` |

Being phony, the targets carry no link step, so each declares its payload as a normal
`add_deps` (unlike the former C++ driver binary, which could not link the payload because of its
undefined DPI symbols). The script locates vcs (`VCS_HOME`/`PATH`), compiles `sv/weave_pkg.sv` plus
`tests/weave_tb.sv`, runs simv with the payload, and judges Pass by the output marker
`WEAVE TEST PASSED`; when vcs cannot be found it prints SKIP and exits with 0.

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

**ABI notes (exception types across DSOs)**: `CancelledError` / `TimeoutError` / `DeadlockError` are
thrown by the library and caught by user code. On Linux/Android, libc++abi follows the Itanium ABI and assumes type
information is globally unique (`type_info` is compared by pointer), so both sides must share **the
same** typeinfo object. Writing inline definitions directly in a public header makes GCC/Clang treat
them as vague linkage: every TU that uses the type emits a weak definition; and since, for instance,
xmake `mode.release` sets `symbols = hidden` by default on all non-shared targets
(`-fvisibility=hidden -fvisibility-inlines-hidden`), the copy inside the consumer executable becomes
a **local symbol** — different from the library's copy, so every `catch` silently misses (no error at
link or start-up; the exception just flies out to the outermost level).

The fix lives in the **type definitions**, not in build flags: `what()` for both types is **declared
only in the public header and defined out-of-line in the library** (`src/core/sync.cpp` /
`src/core/task.cpp`); `DeadlockError` has no own `what()` (it inherits `std::logic_error`'s) and
instead gets its key function from an out-of-line destructor. An out-of-line, non-inline virtual function acts as the **key function**, so the
vtable and typeinfo are emitted exactly once, in that library TU; consumer TUs keep only undefined
references that the dynamic linker resolves to that same copy at load time. Consequently:

- Consumers need **no** `-fvisibility=default` and **no** `-rdynamic` (the repository's
  `weave_examples_sc` target has neither, and it still works in release mode with
  `-fvisibility=hidden`);
- How to check: `nm -D <exe> | grep _ZTIN5weave` should be all `U` (undefined), with `D`/`V`
  definitions appearing only in `libweave.so`; if `what()` is ever moved back to an inline
  definition, definitions reappear here and `catch` regresses to mismatch;
- The cost: `CancelledError` thereby becomes a polymorphic type (with a virtual destructor, no
  longer trivially copyable); its invariants reduce to "borrowed view, no heap members, nothrow
  copy" (the `static_assert`s in `task.hpp` watch exactly these three things).

Loading the `weave` and example/test libraries via `-sv_lib` (the VCS scenario) works the same way:
the single copy of RTTI inside the library is shared through the dynamic linker.

### Task identity across the DPI boundary

`port::owner_task()` — "which task body is this *process* executing, 0 if none" — is the one piece
of state that cannot live in `src/core`: it is per simulation process, while a C++ global is per
process-wide. The two are not the same across a cross-language callback: when a testbench (or the
harness) calls into a C++ export while another task is frozen inside an export window, the C++ stack
belongs to the *caller*, so "the innermost `weave_task_main` frame" answers the wrong question, and
the backend is the only layer that can tell the two parties apart.

- SVDPI: `int p_owner [process]` in `sv/weave_pkg.sv`, read through
  `weave_sv_current_task()` (must be `function automatic` — `process::self()` is illegal in a static
  function context) and written through `weave_sv_set_owner()` (0 deletes the entry, which is how the
  table stays bounded: `weave_task_main` clears it when the body returns).
- SystemC: the same table keyed by the process object —
  `std::unordered_map<const sc_core::sc_object*, int> g_owner`, read through
  `sc_get_current_process_handle().get_process_object()` (`valid()` is false outside any process,
  i.e. in `sc_main`, which is exactly the ownerless case). Keying by the process, not by a single
  int, is what makes a plain `SC_THREAD` safe: such a thread may call into weave while a task body
  is parked, and it must see *no* identity instead of the parked task's (tests/
  scope_sc_thread_semantics.cpp pins this). A scalar plus save/restore inside `port::wait` would only
  have isolated coroutines that suspend *through* the port, and would leave the parked task's value
  behind for anyone else.
- Ownerless suspension: `port::wait` accepts `abort_id < 0` (no cancel leg) for exactly the leaf
  waits that may run outside a task body (`sleep`, `Event::wait`, `sv::Edge::wait`). SystemC builds
  the wait without the abort leg; SV's `weave_sv_wait` already handled `abort_id < 0` because the
  package's own `weave_edge::wait_edge` uses that form. Everything else — combinators and
  ownership-carrying primitives — still rejects an ownerless caller with
  `std::logic_error("weave: blocking primitive outside task context")`: they hand a `TaskRecord` to a
  queue head or need the cancel leg, so an ownerless waiter would never be handed off.
- Consumption side (SVDPI): every suspension — and `current_task()`, which now asks the backend —
  calls back into the package, so C++ code reached from a **consumer's own** DPI import must have the
  DPI scope set to `weave_pkg` before it touches weave. VCS 2018.09 otherwise aborts the whole run
  with `Error-[DPI-DXFNF] … 'weave_sv_current_task' … is not defined or visible` because the entry
  scope is the consumer's package, not a module that imported ours. Setting it once per import
  (`svGetScopeFromName("weave_pkg")` cached, `svSetScope` re-entrantly restored on exit) is
  sufficient; a testbench that imports `weave_pkg` at module scope needs nothing.

## Adding a backend

1. Add a macro to `src/core/config.hpp` (including the mutual-exclusion check and default
   logic).
2. Create `src/backend/<name>.cpp`, the whole file guarded by
   `#if defined(WEAVE_BACKEND_<NAME>)`, implementing the 6 `weave::port` primitives, the timer/identity
   queries and writes (`ticks_per_sec` / `now_ticks` / `owner_task` / `set_owner_task`) plus a
   bootstrap entry point of your choice (see svdpi.cpp / systemc.cpp).
3. xmake mapping: add a value to the `--backend` option, compare `get_config` inside the target to
   define the macro and the dependencies.
4. Regression: `just smoke` must be fully green (name any new self-check target `weave_smoke_*` and
   add it to the `smoke` recipe in the `justfile`).
