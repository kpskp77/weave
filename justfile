# Regression entry points (see AGENTS.md "Build and verify"). The build targets live in xmake.lua;
# the compile & run orchestration for tests and examples lives here:
#   just test-all     — semantic self-checks, once per backend (systemc binary + svdpi VCS script run),
#                       including the fatal-path payloads whose judgement is inverted (a contract
#                       violation, or a caller's own weave::fatal verdict, must end the run with
#                       "weave: fatal:" and no pass marker)
#   just example-all  — example regression, both backends in one go
# just test-all / just example-all each end with the default configuration (--backend=svdpi) restored.
# SystemC payloads always go through `xmake run`: add_packages(systemc) produces no rpath, and
# xmake run is what injects the package LD_LIBRARY_PATH.

_default: test-all example-all

test-sc:
    xmake f --backend=systemc && xmake run weave_smoke_sc
    xmake f --backend=systemc && xmake run weave_smoke_fatal_sc
    xmake f --backend=systemc && xmake run weave_smoke_fatal_user_sc

test-vcs:
    xmake f --backend=svdpi && xmake run weave_smoke_svdpi_vcs
    xmake f --backend=svdpi && xmake run weave_smoke_fatal_svdpi_vcs
    xmake f --backend=svdpi && xmake run weave_smoke_fatal_user_svdpi_vcs

test-all: test-sc test-vcs

example-sc:
    xmake f --backend=systemc && xmake run weave_examples_sc

example-vcs:
    xmake f --backend=svdpi && xmake run weave_examples_svdpi_vcs

example-all: example-sc example-vcs
