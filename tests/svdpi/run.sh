#!/usr/bin/env bash
# Real-simulator (Synopsys VCS) run of an SVDPI payload library. The xmake side is a phony
# target with on_run (see xmake.lua), the same arrangement as the simlog project:
#
#   xmake run weave_smoke_svdpi_vcs      # payload: libweave_smoke_svdpi    (tests/ bodies)
#   xmake run weave_examples_svdpi_vcs   # payload: libweave_examples_svdpi  (examples/ programs)
#
# Usage:
#   tests/svdpi/run.sh <payload.so>
#
# Flow: locate vcs ($VCS_HOME/bin/vcs or PATH) → compile sv/weave_pkg.sv + tests/weave_tb.sv
# in a dedicated work directory next to the payload → run simv with the payload via
# -sv_root/-sv_lib → judge Pass by the output marker "WEAVE TEST PASSED" (the $fatal failure
# path in the tb does not necessarily exit non-zero, so the marker is authoritative).
#
# Skipped (exit 0) when vcs is not on PATH, so the target is safe to run everywhere (without
# a simulator the SVDPI regression degrades to a compile-only check, see AGENTS.md).
set -uo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
lib=${1:-}

# Locate vcs exactly the way the old C++ driver did: $VCS_HOME/bin/vcs first, then PATH.
vcs=""
if [[ -n ${VCS_HOME:-} && -x $VCS_HOME/bin/vcs ]]; then
  vcs=$VCS_HOME/bin/vcs
else
  vcs=$(command -v vcs || true)
fi
if [[ -z $vcs ]]; then
  echo "[svdpi-test] skipped: no vcs on PATH (the SVDPI backend is only verified in a simulator)"
  exit 0
fi
if [[ -z $lib || ! -f $lib ]]; then
  echo "[svdpi-test] usage: run.sh <payload.so>" >&2
  exit 2
fi
lib=$(cd -- "$(dirname -- "$lib")" && pwd)/$(basename -- "$lib")
libdir=$(dirname -- "$lib")
base=$(basename -- "$lib" .so) # -sv_lib base name (keeps the lib prefix, drops .so)

# A fresh compile every run, so a stale simv cannot cause a false positive. The work tree
# lives next to the payload, inside xmake's build directory; VCS drops simv in the CWD, so
# the run happens there too.
work="$libdir/svdpi_work_$base"
rm -rf -- "$work"
mkdir -p -- "$work"

echo "[svdpi-test] compiling $base (vcs: $vcs)"
if ! (cd "$work" &&
  timeout 900 "$vcs" -full64 -sverilog -LDFLAGS "-Wl,--export-dynamic" \
    "$root/sv/weave_pkg.sv" "$root/tests/weave_tb.sv" >compile.log 2>&1 </dev/null); then
  echo "[svdpi-test] FAIL: vcs compile failed, see $work/compile.log" >&2
  tail -n 8 "$work/compile.log" >&2
  exit 2
fi

echo "[svdpi-test] running simv (-sv_lib $base)"
if ! (cd "$work" && timeout 300 ./simv -sv_root "$libdir" -sv_lib "$base" \
  >simv.log 2>&1 </dev/null); then
  cat "$work/simv.log"
  echo "[svdpi-test] FAIL: simv exited non-zero, see $work/simv.log" >&2
  exit 1
fi
cat "$work/simv.log"

if ! grep -q "WEAVE TEST PASSED" "$work/simv.log"; then
  echo "[svdpi-test] FAIL: pass marker missing, see $work/simv.log" >&2
  exit 1
fi
echo "[svdpi-test] ok $base"
