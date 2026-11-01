# Regression entry points (see AGENTS.md "Build and verify"). The build targets live in xmake.lua;
# the compile & run orchestration for tests and examples lives here:
#   just smoke     — semantic self-checks, once per backend (systemc binary + svdpi VCS script run)
#   just examples  — example regression, both backends in one go
# just smoke / just examples each end with the default configuration (--backend=svdpi) restored.
# SystemC payloads always go through `xmake run`: add_packages(systemc) produces no rpath, and
# xmake run is what injects the package LD_LIBRARY_PATH.

_default: smoke examples

smoke:
    xmake f --backend=systemc && xmake run weave_smoke_sc
    xmake f --backend=svdpi && xmake run weave_smoke_svdpi_vcs

examples:
    xmake f --backend=systemc && xmake run weave_examples_sc
    xmake f --backend=svdpi && xmake run weave_examples_svdpi_vcs

