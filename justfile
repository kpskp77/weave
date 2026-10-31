# Regression entry points (see AGENTS.md "Build and verify"). The build targets live in xmake.lua;
# the compile & run orchestration for tests and examples lives here:
#   just smoke     — semantic self-checks, once per backend (systemc binary + svdpi VCS script run)
#   just examples  — example regression, both backends in one go
#   just headers   — public-header baseline: each include/weave/*.hpp compiled standalone under
#                    C++17/20/23 with -Wall -Wextra -Werror
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

# A header that is not clean fails the recipe at the offending compile. An old host compiler
# that does not know a standard reports SKIP for it (same convention as the SVDPI run without
# VCS). CXX wins over the default c++ driver, mirroring the build.
headers:
    #!/usr/bin/env bash
    set -euo pipefail
    cc="${CXX:-c++}"
    tmpdir=$(mktemp -d)
    trap 'rm -rf "$tmpdir"' EXIT
    checked=0
    for std in c++17 c++20 c++23; do
      if ! echo | "$cc" -std="$std" -fsyntax-only -x c++ - >/dev/null 2>&1; then
        echo "==> -std=$std SKIP (not supported by $cc)"
        continue
      fi
      echo "==> -std=$std"
      for header in include/weave/*.hpp; do
        name=$(basename "$header")
        tu="$tmpdir/${name%.hpp}.cpp"
        echo "#include <weave/$name>" >"$tu"
        "$cc" -std="$std" -Wall -Wextra -Werror -fsyntax-only -Iinclude "$tu"
        checked=$((checked + 1))
      done
    done
    echo "$checked header x standard combinations clean"
