-- Weave: C++ 测试逻辑与 EDA 仿真内核之间的行为编排层
-- 构建产物为仿真器加载的共享库（或自检二进制）+ 后端协议

set_project("weave")
set_version("0.1.0")

add_rules("mode.debug", "mode.release")
set_languages("c++17")

-- SystemC 后端依赖（xmake-repo 包；若使用本地安装请改为
-- add_includedirs/add_linkdirs/add_links 指向安装路径）
add_requires("systemc")
add_requires("svdpi")

-- 后端选择：svdpi（默认）/ systemc。
-- 对应宏 WEAVE_BACKEND_SVDPI / WEAVE_BACKEND_SYSTEMC 由目标定义；
-- 不定义任何宏时 include/weave/config.hpp 默认 SVDPI。
option("backend")
    set_showmenu(true)
    set_default("svdpi")
    set_values("svdpi", "systemc")
    set_description("Simulation backend")

-- Weave 核心（src/core 后端无关簿记 + src/backend 宏选择的后端适配）
target("weave")
    set_kind("shared")
    add_includedirs("include", {public = true})
    add_files("src/core/*.cpp", "src/backend/*.cpp")
    if get_config("backend") == "systemc" then
        add_packages("systemc")
        add_defines("WEAVE_BACKEND_SYSTEMC")
    else
        add_packages("svdpi")
        add_defines("WEAVE_BACKEND_SVDPI")
    end

-- 示例测试（编译进独立共享库，与 weave 一同 -sv_lib 加载）
target("weave_demo")
    set_kind("shared")
    add_deps("weave")
    add_rpathdirs("$ORIGIN")
    add_files("examples/*.cpp")

-- SystemC 自检：在真实 SystemC 内核上运行核心语义断言（gather 的 asyncio 语义等）。
-- SystemC 由 xmake-repo 提供（add_requires("systemc")，构建任意目标前会自动安装）。
--   xmake build weave_sc_smoke && ./build/<plat>/<arch>/<mode>/weave_sc_smoke
target("weave_sc_smoke")
    set_kind("binary")
    add_includedirs("include")
    add_defines("WEAVE_BACKEND_SYSTEMC")
    add_files("src/core/*.cpp", "src/backend/*.cpp",
              "tests/as_completed_semantics.cpp", "tests/event_semantics.cpp",
              "tests/gather_semantics.cpp", "tests/shield_semantics.cpp",
              "tests/task_semantics.cpp", "tests/taskgroup_semantics.cpp",
              "tests/wait_for_semantics.cpp", "tests/wait_semantics.cpp",
              "tests/sc_main.cpp")
    add_packages("systemc")

-- SVDPI 自检库：与 weave_sc_smoke 共用同一组断言体（tests/*_semantics.cpp），
-- 编成仿真器 -sv_lib 加载的共享库。后端宏显式固定为 SVDPI，与 --backend 配置无关，
-- 两种配置下都回归 DPI 路径。
target("weave_svdpi_smokelib")
    set_kind("shared")
    add_includedirs("include")
    add_defines("WEAVE_BACKEND_SVDPI")
    add_files("src/core/*.cpp", "src/backend/*.cpp",
              "tests/as_completed_semantics.cpp", "tests/event_semantics.cpp",
              "tests/gather_semantics.cpp", "tests/shield_semantics.cpp",
              "tests/task_semantics.cpp", "tests/taskgroup_semantics.cpp",
              "tests/wait_for_semantics.cpp", "tests/wait_semantics.cpp")
    add_packages("svdpi")

-- SVDPI 自检驱动（binary 且 *_smoke 结尾，自动纳入 xmake smoke）：
-- 定位 VCS → 编译 sv/ 骨架并链接 smokelib → 运行 simv 并以输出标记判 Pass。
-- 无 VCS/IES 环境时打印 SKIP 并以 0 退出（见 AGENTS.md）。
target("weave_svdpi_smoke")
    set_kind("binary")
    -- links = false：只保留构建顺序依赖，不把 smokelib 链进驱动。
    -- 否则动态加载器在 main() 运行前就会因库内未定义的 DPI 符号
    -- （svGetScope 等，本应由 VCS 的 simv 运行时提供）拒绝加载，
    -- SKIP 分支根本到不了。驱动自身不调用库符号，仅按路径引用它。
    add_deps("weave_svdpi_smokelib", {links = false})
    add_files("tests/svdpi_smoke.cpp")
    add_defines("WEAVE_SVDPI_SV_DIR=\"$(projectdir)/sv\"")

-- 回归入口：构建全部目标并运行全部 *_smoke 自检二进制。
-- 新增自检目标只需命名为 *_smoke 即自动纳入，无需修改本任务。
task("smoke")
    set_menu {
        usage = "xmake smoke",
        description = "Build all targets and run all self-check binaries (full regression)"
    }
    on_run(function ()
        import("core.project.config")
        import("core.project.project")
        config.load()
        os.execv("xmake", {"build"})
        for _, target in pairs(project.targets()) do
            local name = target:name()
            if target:get("kind") == "binary" and name:match("_smoke$") then
                print("==> " .. name)
                local code = os.execv(target:targetfile())
                if code ~= 0 then
                    os.raise(name .. " failed (exit " .. tostring(code) .. ")")
                end
            end
        end
        cprint("${bright green}All smoke tests passed.${clear}")
    end)
