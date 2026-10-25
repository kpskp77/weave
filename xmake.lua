-- Weave: a behaviour orchestration layer between C++ test logic and EDA simulation kernels
-- The build produces the shared libraries the simulator loads (or self-check binaries) plus the
-- backend protocol
--
-- Target grouping (keep same-kind targets adjacent, do not interleave):
--   weave           core library (backend chosen by --backend)
--   smoke payloads  weave_smoke_sc / weave_smoke_svdpi / weave_smoke_svdpi_driver
--   examples        weave_examples_sc (systemc) / weave_examples_svdpi (svdpi)
-- Regression entries: xmake smoke (tests/ semantic self-checks), xmake examples (examples/
-- programs); the two are independent.

set_project("weave")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_languages("c++17")

-- SystemC backend dependency (an xmake-repo package; for a local install, replace this with
-- add_includedirs/add_linkdirs/add_links pointing at the installation)
add_requires("systemc", { configs = { shared = true } })
add_requires("svdpi")

-- Backend selection: svdpi (default) / systemc.
-- The matching macros WEAVE_BACKEND_SVDPI / WEAVE_BACKEND_SYSTEMC are defined by the targets;
-- with no macro defined, include/weave/config.hpp defaults to SVDPI.
option("backend", function()
  set_showmenu(true)
  set_default("svdpi")
  set_values("svdpi", "systemc")
  set_description("Simulation backend")
end)

-- Warning policy: every target uses -Wall -Wextra with warnings as errors (set_warnings' error
-- level maps to -Werror). Third-party package headers are pulled in via -isystem by add_packages
-- and are unaffected.

-- ── core library ─────────────────────────────────────────────────────────────

-- src/core backend-agnostic bookkeeping + the backend adapter selected by macro in src/backend
target("weave", function()
  set_kind("shared")
  set_warnings("all", "extra", "error")
  add_includedirs("include", { public = true })
  add_files("src/**.cpp")
  if get_config("backend") == "systemc" then
    add_packages("systemc")
    add_defines("WEAVE_BACKEND_SYSTEMC")
  else
    add_packages("svdpi")
    add_defines("WEAVE_BACKEND_SVDPI")
  end
end)

-- ── smoke payloads (tests/; run by xmake smoke) ──────────────────────────────

-- SystemC self-check: runs the core semantic assertions on a real SystemC kernel.
-- SystemC comes from xmake-repo (add_requires("systemc"), installed automatically before building
-- any target).
--   xmake build weave_smoke_sc && xmake run weave_smoke_sc   -- must go through xmake run (injects
--                                                            the package runtime environment)
-- Examples are not compiled into this payload: they are built the way a user would and link
-- libweave, see the examples payloads below.
target("weave_smoke_sc", function()
  set_warnings("all", "extra", "error")
  set_kind("binary")
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
  add_packages("systemc")
end)

-- SVDPI self-check library: shares the same set of assertion bodies (tests/*_semantics.cpp) with
-- weave_smoke_sc, built as a shared library the simulator loads via -sv_lib. The backend macro is
-- pinned to SVDPI explicitly, independently of the --backend configuration, so the DPI path is
-- regressed under both configurations.
target("weave_smoke_svdpi", function()
  set_warnings("all", "extra", "error")
  set_kind("shared")
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
  add_packages("svdpi")
end)

-- The SVDPI VCS driver (tests/svdpi_driver.cpp) is compiled into several targets that differ only
-- in their payload parameters; each locates VCS → compiles the sv/ skeleton → runs simv with the
-- -sv_lib payload → judges Pass by the output marker, and prints SKIP and exits 0 when VCS/IES is
-- unavailable (see AGENTS.md).
--
-- This target is the "self-check" payload (a binary whose name starts with weave_smoke_, so xmake
-- smoke picks it up). No dependency on the payload is declared: once the payload library is linked
-- into the driver, the dynamic loader refuses to load it before main() runs, because of the
-- undefined DPI symbols inside it (svGetScope and friends, which only VCS's simv is supposed to
-- provide) — the SKIP branch would be unreachable. At run time the driver looks for the payload
-- next to itself (WEAVE_SVDPI_LIB); the build order is guaranteed by xmake smoke's build list.
target("weave_smoke_svdpi_driver", function()
  set_warnings("all", "extra", "error")
  set_kind("binary")
  add_files("tests/svdpi_driver.cpp")
  add_defines(
    'WEAVE_SVDPI_SV_DIR="$(projectdir)/sv"',
    'WEAVE_SVDPI_LIB="libweave_smoke_svdpi"',
    'WEAVE_SVDPI_WORK="svdpi_smoke_work"'
  )
end)

-- ── examples payloads (examples/; run by xmake examples) ─────────────────────

-- The example regression is built the way a **user** would: only examples/*.cpp are compiled, the
-- example code may use nothing but the public API in include/weave/, and it links libweave.so via
-- add_deps("weave") (never inlining core sources), so it genuinely crosses the DSO boundary. Both
-- forms are always defined (not gated on --backend): xmake examples switches configurations within
-- a single invocation and runs each of them, so the task process must be able to look up both
-- targets.
--   weave_examples_sc    systemc → an executable, run locally
--   weave_examples_svdpi svdpi   → a shared library, loaded into VCS via -sv_lib

target("weave_examples_sc", function()
  set_warnings("all", "extra", "error")
  set_kind("binary")
  add_deps("weave")
  add_rpathdirs("$ORIGIN")
  add_files("examples/*.cpp", "tests/sc_main.cpp")
  add_packages("systemc")
end)

-- SVDPI shared-library form: loaded into VCS together with libweave.so via -sv_lib, bootstrapped
-- by weave_run/weave_sv_wait_done in sv/weave_tb.sv. This is also the payload for the VCS-side
-- example regression, and the very library the manual VCS run in the docs loads.
target("weave_examples_svdpi", function()
  set_warnings("all", "extra", "error")
  set_kind("shared")
  add_deps("weave")
  add_rpathdirs("$ORIGIN")
  add_files("examples/*.cpp")
end)

-- The VCS driver for the examples: the same tests/svdpi_driver.cpp as weave_smoke_svdpi_driver,
-- with the payload swapped for libweave_examples_svdpi. It is not named weave_smoke_* — the
-- example regression is independent of xmake smoke.
-- **No dependency is declared**: {links = false} / {order = true} would both drag in libweave.so's
-- link dependencies through the payload (which itself has add_deps("weave")), and libweave.so's
-- DPI symbols can only be provided by VCS's simv — the driver would fail at load time with
-- "cannot locate symbol svGetScope", never reaching the SKIP branch. The driver only looks for the
-- payload by path at run time, so no build-time dependency is needed; xmake examples explicitly
-- builds the payload before the driver.
target("weave_examples_svdpi_driver", function()
  set_warnings("all", "extra", "error")
  set_kind("binary")
  add_files("tests/svdpi_driver.cpp")
  add_defines(
    'WEAVE_SVDPI_SV_DIR="$(projectdir)/sv"',
    'WEAVE_SVDPI_LIB="libweave_examples_svdpi"',
    'WEAVE_SVDPI_WORK="svdpi_examples_work"'
  )
end)

-- ── regression tasks ─────────────────────────────────────────────────────────

-- Regression entry: build the three smoke payloads, then run each weave_smoke_* self-check
-- binary with `xmake run`.
-- A new self-check target is run by this task only if its name starts with weave_smoke_ (and it
-- is a binary), and it must also be added to the build list below (xmake run injects the
-- package's LD_LIBRARY_PATH, since systemc has no rpath).
task("smoke", function()
  set_menu({
    usage = "xmake smoke",
    description = "Build all targets and run all self-check binaries (full regression)",
  })
  on_run(function()
    import("core.project.config")
    import("core.project.project")
    config.load()
    os.execv(
      "xmake",
      { "build", "weave_smoke_sc", "weave_smoke_svdpi", "weave_smoke_svdpi_driver" }
    )
    for _, target in pairs(project.targets()) do
      local name = target:name()
      if target:get("kind") == "binary" and name:match("^weave_smoke_") then
        print("==> " .. name)
        local code = os.execv("xmake", { "run", name })
        if code ~= 0 then os.raise(name .. " failed (exit " .. tostring(code) .. ")") end
      end
    end
    cprint("${bright green}All smoke tests passed.${clear}")
  end)
end)

-- Example regression entry (independent of xmake smoke; payloads described above).
task("examples", function()
  set_menu({
    usage = "xmake examples",
    description = "Build and run examples on both backends (independent of smoke)",
  })
  on_run(function()
    import("core.project.config")
    import("core.project.project")
    config.load()
    -- Run both backends: the example targets are always defined, and switching --backend only
    -- makes the `weave` library (the one weave_examples_* links) rebuild for the chosen backend.
    -- The configuration is restored afterwards.
    local saved = config.get("backend")

    -- SystemC: an executable, run locally on a real kernel.
    os.execv("xmake", { "f", "--backend=systemc" })
    os.execv("xmake", { "build", "weave_examples_sc" })
    print("==> [systemc] weave_examples_sc")
    local code = os.execv("xmake", { "run", "weave_examples_sc" })
    if code ~= 0 then
      os.execv("xmake", { "f", "--backend=" .. saved })
      os.raise("weave_examples_sc failed (exit " .. tostring(code) .. ")")
    end

    -- SVDPI: shared library + VCS driver. The driver itself prints SKIP and returns 0 when VCS is
    --     unavailable, so without SVDPI support this step amounts to a compile-only check.
    os.execv("xmake", { "f", "--backend=svdpi" })
    os.execv("xmake", { "build", "weave_examples_svdpi", "weave_examples_svdpi_driver" })
    print("==> [svdpi] weave_examples_svdpi + driver")
    local vcs_code = os.execv("xmake", { "run", "weave_examples_svdpi_driver" })
    os.execv("xmake", { "f", "--backend=" .. saved })
    if vcs_code ~= 0 then
      os.raise("weave_examples_svdpi failed (exit " .. tostring(vcs_code) .. ")")
    end
    cprint("${bright green}Examples done (systemc + svdpi).${clear}")
  end)
end)

-- Public-header baseline check: every header under include/weave/ is compiled as its own
-- translation unit (a file whose only content is `#include <weave/X.hpp>`) under
-- -std=c++17 / c++20 / c++23 with the project's warning policy and -fsyntax-only, so "each public
-- header compiles standalone under every supported standard" is a re-runnable check rather than a
-- claim (README.md, "Language standard"). sc.hpp needs neither the SystemC include path nor
-- WEAVE_BACKEND_SYSTEMC: it only declares weave::sc::run().
task("headers", function()
  set_menu({
    usage = "xmake headers",
    description = "Compile each public header standalone under C++17/20/23",
  })
  on_run(function()
    import("core.project.config")
    import("core.project.project")
    config.load()
    -- Same compiler as the build (the target toolchain knows it); CXX wins when set explicitly.
    local cc = os.getenv("CXX")
    if not cc then cc = project.target("weave_smoke_sc"):tool("cxx") end
    local headers = os.files("include/weave/*.hpp")
    table.sort(headers)
    local tmpdir = path.join(os.tmpdir(), "weave_headers")
    os.tryrm(tmpdir)
    os.mkdir(tmpdir)
    local checked = 0
    for _, std in ipairs({ "c++17", "c++20", "c++23" }) do
      -- An old host compiler may not know the standard at all: report SKIP instead of a failure
      -- (same convention as the SVDPI driver without VCS).
      local probe = path.join(tmpdir, "probe.cpp")
      io.writefile(probe, "")
      if os.execv(cc, { "-std=" .. std, "-fsyntax-only", probe }) ~= 0 then
        print("==> -std=" .. std .. " SKIP (not supported by " .. cc .. ")")
        goto continue
      end
      print("==> -std=" .. std)
      for _, header in ipairs(headers) do
        local tu = path.join(tmpdir, path.basename(header) .. ".cpp")
        io.writefile(tu, "#include <weave/" .. path.filename(header) .. ">\n")
        local code = os.execv(cc, {
          "-std=" .. std,
          "-Wall",
          "-Wextra",
          "-Werror",
          "-fsyntax-only",
          "-Iinclude",
          tu,
        })
        if code ~= 0 then
          os.raise(
            header .. " is not clean under -std=" .. std .. " (exit " .. tostring(code) .. ")"
          )
        end
        checked = checked + 1
      end
      ::continue::
    end
    os.tryrm(tmpdir)
    cprint("${bright green}" .. checked .. " header x standard combinations clean${clear}")
  end)
end)
