-- Weave: C++ 测试逻辑与 EDA 仿真内核之间的行为编排层
-- 构建产物为仿真器加载的共享库（或自检二进制）+ 后端协议
--
-- 目标分组（同类相邻，勿交错）：
--   weave                                     核心库（按 --backend 选择后端）
--   smoke 载荷  weave_sc_smoke / weave_svdpi_smokelib / weave_svdpi_smoke
--   examples 载荷 weave_examples_sc（systemc）/ weave_examples_svdpi（svdpi）
-- 回归入口：xmake smoke（tests/ 语义自检）、xmake examples（examples/ 示例），两者独立。

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

-- 编译告警策略：所有目标统一 -Wall -Wextra 且警告即错误（set_warnings 的 error
-- 级别映射为 -Werror）。第三方包头文件由 add_packages 以 -isystem 引入，不受影响。

-- ── 核心库 ────────────────────────────────────────────────────────────

-- src/core 后端无关簿记 + src/backend 宏选择的后端适配
target("weave")
    set_kind("shared")
    set_warnings("all", "extra", "error")
    -- 公共头里定义、由库抛出、由调用方 catch 的异常类型（Cancelled/Timeout）其 typeinfo
    -- 与 vtable 需跨库对齐；libc++ 按指针比较 type_info，两侧须导出同一份。故保持
    -- 默认可见性（不继承依赖包可能带入的 -fvisibility=hidden）。
    add_cxflags("-fvisibility=default")
    add_includedirs("include", {public = true})
    add_files("src/core/*.cpp", "src/backend/*.cpp")
    if get_config("backend") == "systemc" then
        add_packages("systemc")
        add_defines("WEAVE_BACKEND_SYSTEMC")
    else
        add_packages("svdpi")
        add_defines("WEAVE_BACKEND_SVDPI")
    end

-- ── smoke 载荷（tests/，xmake smoke 运行）──────────────────────────────

-- SystemC 自检：在真实 SystemC 内核上运行核心语义断言。
-- SystemC 由 xmake-repo 提供（add_requires("systemc")，构建任意目标前会自动安装）。
--   xmake build weave_sc_smoke && ./build/<plat>/<arch>/<mode>/weave_sc_smoke
-- 示例不编入本载荷：示例按用户用法单独编译并链接 libweave，见下方 examples 载荷。
target("weave_sc_smoke")
    set_warnings("all", "extra", "error")
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
    set_warnings("all", "extra", "error")
    set_kind("shared")
    add_includedirs("include")
    add_defines("WEAVE_BACKEND_SVDPI")
    add_files("src/core/*.cpp", "src/backend/*.cpp",
              "tests/as_completed_semantics.cpp", "tests/event_semantics.cpp",
              "tests/gather_semantics.cpp", "tests/shield_semantics.cpp",
              "tests/task_semantics.cpp", "tests/taskgroup_semantics.cpp",
              "tests/wait_for_semantics.cpp", "tests/wait_semantics.cpp")
    add_packages("svdpi")

-- SVDPI VCS 驱动（tests/svdpi_driver.cpp）被编成多个目标，只差载荷参数；
-- 都定位 VCS → 编译 sv/ 骨架 → simv 加载 -sv_lib 载荷 → 以输出标记判 Pass，
-- 无 VCS/IES 时打印 SKIP 并以 0 退出（见 AGENTS.md）。
--
-- 本目标为「自检」载荷（binary 且 *_smoke 结尾，自动纳入 xmake smoke）。
-- links = false：只保留构建顺序依赖，不把载荷库链进驱动。否则动态加载器在
-- main() 运行前就会因库内未定义的 DPI 符号（svGetScope 等，本应由 VCS 的
-- simv 运行时提供）拒绝加载，SKIP 分支根本到不了。
target("weave_svdpi_smoke")
    set_warnings("all", "extra", "error")
    set_kind("binary")
    add_deps("weave_svdpi_smokelib", {links = false})
    add_files("tests/svdpi_driver.cpp")
    add_defines("WEAVE_SVDPI_SV_DIR=\"$(projectdir)/sv\"",
                "WEAVE_SVDPI_LIB=\"libweave_svdpi_smokelib\"",
                "WEAVE_SVDPI_WORK=\"svdpi_smoke_work\"")

-- ── examples 载荷（examples/，xmake examples 运行）─────────────────────

-- 示例回归按**用户用法**编译：只编 examples/*.cpp，示例代码只能用 include/weave/ 的
-- 公共 API，并通过 add_deps("weave") 链接 libweave.so（不内联核心源码），因此真实跨过
-- DSO 边界。两种形态常驻定义（不按 --backend 门控）：xmake examples 会在同一次调用里
-- 切换配置把两者各跑一遍，任务进程需要能查到两个目标。
--   weave_examples_sc    systemc → 可执行文件，本地运行
--   weave_examples_svdpi svdpi   → 共享库，-sv_lib 进 VCS 运行

target("weave_examples_sc")
    set_warnings("all", "extra", "error")
    set_kind("binary")
    add_includedirs("include")
    add_deps("weave")
    add_rpathdirs("$ORIGIN")
    add_files("examples/*.cpp", "tests/sc_main.cpp")
    -- 可执行文件形态下公共异常类型会再生成一份 RTTI/vtable，libc++ 按指针比较
    -- type_info，需经动态符号解析到同一份 → 加 -rdynamic（--export-dynamic）。
    -- 共享库形态（weave_examples_svdpi）动态链接器自行统合，无需此设置。
    add_cxflags("-fvisibility=default")
    add_ldflags("-rdynamic")
    add_packages("systemc")

-- SVDPI 共享库形态：与 libweave.so 一起经 -sv_lib 加载进 VCS，由 sv/weave_tb.sv
-- 的 weave_run/weave_sv_wait_done 引导。这也是 VCS 侧示例回归的载体，
-- 同时是文档里 VCS 手工运行示例所加载的那份库。
target("weave_examples_svdpi")
    set_warnings("all", "extra", "error")
    set_kind("shared")
    add_includedirs("include")
    add_deps("weave")
    add_rpathdirs("$ORIGIN")
    add_files("examples/*.cpp")

-- 示例的 VCS 驱动：与 weave_svdpi_smoke 同一份 tests/svdpi_driver.cpp，只是载荷
-- 换成 libweave_examples_svdpi。不叫 *_smoke —— 示例回归独立于 xmake smoke。
-- **不建立任何依赖**：{links = false} / {order = true} 都会经载荷把 libweave.so 的
-- 链接依赖带进来（载荷本身 add_deps("weave")），而 libweave.so 的 DPI 符号只有 VCS
-- 的 simv 能提供 —— 驱动会在加载阶段就以 "cannot locate symbol svGetScope" 失败，
-- SKIP 分支根本到不了。驱动只在运行时按路径找载荷，故无需构建期依赖；
-- xmake examples 会显式先建载荷再建驱动。
target("weave_examples_svdpi_driver")
    set_warnings("all", "extra", "error")
    set_kind("binary")
    add_files("tests/svdpi_driver.cpp")
    add_defines("WEAVE_SVDPI_SV_DIR=\"$(projectdir)/sv\"",
                "WEAVE_SVDPI_LIB=\"libweave_examples_svdpi\"",
                "WEAVE_SVDPI_WORK=\"svdpi_examples_work\"")

-- ── 回归任务 ──────────────────────────────────────────────────────────

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

-- 示例回归入口（与 xmake smoke 相互独立，载荷见上）。
task("examples")
    set_menu {
        usage = "xmake examples",
        description = "Build and run examples on both backends (independent of smoke)"
    }
    on_run(function ()
        import("core.project.config")
        import("core.project.project")
        config.load()
        -- 双后端各跑一遍：示例目标常驻定义，切换 --backend 只是让 `weave` 库
        -- （以及 weave_examples_* 链接的那份）按对应后端重建。结束后还原配置。
        local saved = config.get("backend")

        -- ① SystemC：可执行文件，本地真实内核运行。
        os.execv("xmake", {"f", "--backend=systemc"})
        os.execv("xmake", {"build", "weave_examples_sc"})
        print("==> [systemc] weave_examples_sc")
        local code = os.execv(project.target("weave_examples_sc"):targetfile())
        if code ~= 0 then
            os.execv("xmake", {"f", "--backend=" .. saved})
            os.raise("weave_examples_sc failed (exit " .. tostring(code) .. ")")
        end

        -- ② SVDPI：共享库 + VCS 驱动。驱动自身会在无 VCS 时打印 SKIP 并返回 0，
        --    因此"不支持 SVDPI"时本步等价于只做编译验证。
        os.execv("xmake", {"f", "--backend=svdpi"})
        os.execv("xmake", {"build", "weave_examples_svdpi"})  -- 先出载荷
        os.execv("xmake", {"build", "weave_examples_svdpi_driver"})
        print("==> [svdpi] weave_examples_svdpi + driver")
        local vcs_code = os.execv(project.target("weave_examples_svdpi_driver"):targetfile())
        os.execv("xmake", {"f", "--backend=" .. saved})
        if vcs_code ~= 0 then
            os.raise("weave_examples_svdpi failed (exit " .. tostring(vcs_code) .. ")")
        end
        cprint("${bright green}Examples done (systemc + svdpi).${clear}")
    end)
