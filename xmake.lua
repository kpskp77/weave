-- Weave: a behaviour orchestration layer between C++ test logic and EDA simulation kernels
-- The build produces the shared libraries the simulator loads (or self-check binaries) plus the
-- backend protocol
--
-- Target grouping (keep same-kind targets adjacent, do not interleave):
--   weave           core library (backend chosen by --backend)
--   smoke payloads  weave_smoke_sc / weave_smoke_svdpi (+ weave_smoke_svdpi_vcs phony runner)
--   examples        weave_examples_sc (systemc) / weave_examples_svdpi (svdpi, + phony runner)
-- Regression entries live in the justfile (just smoke / just examples); this file
-- defines build targets only, and the two regressions are independent.

set_project("weave")
set_version("1.0.3")

add_rules("mode.debug", "mode.release")
set_languages("c++17")

-- Backend selection: svdpi (default) / systemc.
-- The matching macros WEAVE_BACKEND_SVDPI / WEAVE_BACKEND_SYSTEMC are defined by the targets;
-- with no macro defined, src/core/config.hpp defaults to SVDPI.
option("backend", function()
  set_showmenu(true)
  set_default("svdpi")
  set_values("svdpi", "systemc")
  set_description("Simulation backend")
end)

-- Add dependencies
add_requires("simlog", { configs = { shared = true, backend = get_config("backend") } })
if get_config("backend") == "systemc" then
  add_requires("systemc", { configs = { shared = true } })
else
  add_requires("svdpi")
end

-- Warning policy: every target uses -Wall -Wextra with warnings as errors (set_warnings' error
-- level maps to -Werror). Third-party package headers are pulled in via -isystem by add_packages
-- and are unaffected.

-- ── core library ─────────────────────────────────────────────────────────────

-- src/core backend-agnostic bookkeeping + the backend adapter selected by macro in src/backend
-- The simlog package is public: libweave.so's core sources carry the debug/trace instrumentation
-- (src/core/trace.hpp), so consumers that link the library inherit the simlog link (the examples
-- link libweave.so and load it into a simulator via -sv_lib).
target("weave", function()
  set_kind("shared")
  set_warnings("all", "extra", "error")
  add_headerfiles("include/(weave/*.hpp)")
  add_includedirs("include", { public = true })
  add_files("src/**.cpp")
  add_packages("simlog", { public = true })
  if get_config("backend") == "systemc" then
    add_packages("systemc")
    add_defines("WEAVE_BACKEND_SYSTEMC")
  else
    add_packages("svdpi")
    add_defines("WEAVE_BACKEND_SVDPI")
  end
end)

-- ── smoke payloads (tests/; run by just smoke) ───────────────────────────────

-- SystemC self-check: runs the core semantic assertions on a real SystemC kernel.
-- SystemC comes from xmake-repo (add_requires("systemc"), installed automatically before building
-- any target).
--   xmake build weave_smoke_sc && xmake run weave_smoke_sc   -- must go through xmake run (injects
--                                                            the package runtime environment)
-- Examples are not compiled into this payload: they are built the way a user would and link
-- libweave, see the examples payloads below.
target("weave_smoke_sc", function()
  set_kind("binary")
  set_default(false)
  set_warnings("all", "extra", "error")
  add_includedirs("include")
  add_defines("WEAVE_BACKEND_SYSTEMC")
  add_files(
    "src/**.cpp",
    "tests/as_completed_semantics.cpp",
    "tests/barrier_semantics.cpp",
    "tests/condition_semantics.cpp",
    "tests/duration_semantics.cpp",
    "tests/event_semantics.cpp",
    "tests/gather_semantics.cpp",
    "tests/lock_semantics.cpp",
    "tests/queue_semantics.cpp",
    "tests/semaphore_semantics.cpp",
    "tests/shield_semantics.cpp",
    "tests/task_semantics.cpp",
    "tests/taskgroup_semantics.cpp",
    "tests/wait_for_semantics.cpp",
    "tests/wait_semantics.cpp",
    "tests/performance_semantics.cpp",
    "tests/sc_main.cpp"
  )
  add_packages("systemc", "simlog")
end)

-- SVDPI self-check library: shares the same set of assertion bodies (tests/*_semantics.cpp) with
-- weave_smoke_sc, built as a shared library the simulator loads via -sv_lib. The backend macro is
-- pinned to SVDPI explicitly, independently of the --backend configuration, so the DPI path is
-- regressed under both configurations.
target("weave_smoke_svdpi", function()
  set_kind("shared")
  set_default(false)
  set_warnings("all", "extra", "error")
  add_includedirs("include")
  add_defines("WEAVE_BACKEND_SVDPI")
  add_files(
    "src/**.cpp",
    "tests/as_completed_semantics.cpp",
    "tests/barrier_semantics.cpp",
    "tests/condition_semantics.cpp",
    "tests/duration_semantics.cpp",
    "tests/event_semantics.cpp",
    "tests/gather_semantics.cpp",
    "tests/lock_semantics.cpp",
    "tests/queue_semantics.cpp",
    "tests/semaphore_semantics.cpp",
    "tests/shield_semantics.cpp",
    "tests/task_semantics.cpp",
    "tests/taskgroup_semantics.cpp",
    "tests/wait_for_semantics.cpp",
    "tests/wait_semantics.cpp",
    "tests/performance_semantics.cpp"
  )
  add_packages("svdpi", "simlog")
end)

-- The SVDPI VCS run is a phony target executing tests/svdpi/run.sh (the simlog arrangement):
-- the script locates VCS → compiles sv/weave_pkg.sv + tests/weave_tb.sv → runs simv with the
-- payload via -sv_root/-sv_lib → judges Pass by the output marker, and prints SKIP and exits 0
-- when VCS/IES is unavailable (see AGENTS.md).
--
-- Unlike the former C++ driver binary, a phony target carries no link step, so the dependency on
-- the payload library is declared normally and the build order is guaranteed by xmake itself.
-- The "self-check" name prefix (weave_smoke_) marks the payload as part of the smoke regression;
-- the justfile's smoke recipe runs it through `xmake run` like a binary (a phony target's on_run
-- is invoked by xmake run, which also builds its deps).
target("weave_smoke_svdpi_vcs", function()
  set_kind("phony")
  set_default(false)
  add_deps("weave_smoke_svdpi")
  on_run(function(target)
    os.execv("bash", {
      path.join(os.projectdir(), "tests/svdpi/run.sh"),
      target:dep("weave_smoke_svdpi"):targetfile(),
    })
  end)
end)

-- ── examples payloads (examples/; run by just examples) ─────────────────────

-- The example regression is built the way a **user** would: only examples/*.cpp are compiled, the
-- example code may use nothing but the public API in include/weave/, and it links libweave.so via
-- add_deps("weave") (never inlining core sources), so it genuinely crosses the DSO boundary. Both
-- forms are always defined (not gated on --backend): just examples switches configurations
-- within a single recipe (xmake f between the two runs), so both target definitions must be
-- resolvable regardless of the current --backend.
--   weave_examples_sc    systemc → an executable, run locally
--   weave_examples_svdpi svdpi   → a shared library, loaded into VCS via -sv_lib

target("weave_examples_sc", function()
  set_kind("binary")
  set_default(false)
  set_warnings("all", "extra", "error")
  add_deps("weave")
  add_rpathdirs("$ORIGIN")
  add_files("examples/*.cpp", "tests/sc_main.cpp")
  add_packages("systemc")
end)

-- SVDPI shared-library form: loaded into VCS together with libweave.so via -sv_lib; the bootstrap
-- entry weave_start (imported by sv/weave_pkg.sv, called from its weave_run task, which
-- tests/weave_tb.sv calls) starts the registered tests. This is also the payload for the VCS-side
-- example regression, and the very library the manual VCS run in the docs loads.
target("weave_examples_svdpi", function()
  set_kind("shared")
  set_default(false)
  set_warnings("all", "extra", "error")
  add_deps("weave")
  add_rpathdirs("$ORIGIN")
  add_files("examples/*.cpp")
end)

-- The VCS run for the examples: the same tests/svdpi/run.sh as weave_smoke_svdpi_vcs, with the
-- payload swapped for libweave_examples_svdpi. It is not named weave_smoke_* — the example
-- regression is independent of the smoke regression (just smoke).
target("weave_examples_svdpi_vcs", function()
  set_kind("phony")
  set_default(false)
  add_deps("weave_examples_svdpi")
  on_run(function(target)
    os.execv("bash", {
      path.join(os.projectdir(), "tests/svdpi/run.sh"),
      target:dep("weave_examples_svdpi"):targetfile(),
    })
  end)
end)
