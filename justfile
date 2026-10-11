# 手工开发入口：demo 示例的 VCS 编译/运行捷径。
# 回归唯一入口是 xmake smoke（weave_svdpi_smoke 会自动驱动 VCS），勿在此加测试逻辑。

smoke:
    xmake smoke

# 编译 SV 骨架 + demo 库。仅 sv/ 改动才需要重跑本步骤；
# 只改 C++ 时 simv 在运行时才解析 -sv_lib，无需重跑 vcs。
build:
    #!/usr/bin/env bash
    set -euo pipefail
    xmake build weave_demo
    mkdir -p build/vcs
    cd build/vcs
    vcs -full64 -sverilog -LDFLAGS "-Wl,--export-dynamic" \
        ../../sv/weave_pkg.sv ../../sv/weave_tb.sv

# 运行 demo：按当前 xmake 配置（plat/arch/mode）定位共享库目录，自动重编 weave_demo
run:
    #!/usr/bin/env bash
    set -euo pipefail
    xmake build weave_demo
    libdir=$(xmake show -t weave_demo | sed 's/\x1b\[[0-9;]*m//g' \
        | awk -F': ' '/targetfile:/ {print $2}' | xargs dirname | xargs realpath)
    cd build/vcs
    ./simv -sv_root "$libdir/" -sv_lib libweave_demo
