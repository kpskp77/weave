#!/usr/bin/env bash
# Expected-death check for the fatal path (src/core/task.cpp detail::fatal_report →
# src/core/port.hpp port::fatal → SystemC SC_REPORT_FATAL + abort / SVDPI $fatal).
#
# Usage:
#   tests/expect_fatal.sh [--expect <text>]... <command...>
#
# The command must die with the diagnosis headline "weave: fatal:" on its output and must never
# reach the pass marker. This is the inverse of the normal judgement, so it can only be used with a
# payload whose body deliberately ends the run (tests/fatal_semantics.cpp for a core contract
# violation, tests/fatal_user_semantics.cpp for a caller's own weave::fatal verdict). A violation
# that is caught somewhere, downgraded into an ordinary failure or silently swallowed turns this
# script into a failure — which is exactly the regression it encodes.
#
# Each --expect <text> adds a fixed-string match the output must also satisfy — for the payload's
# own wording and the identity that declared it, so the report path cannot pass on the generic
# headline alone.
#
# Note: the log is captured and replayed rather than streamed, so the checks work on both the
# SystemC binary's stdout/stderr and a simulator log.
set -uo pipefail

expects=()
while [[ $# -gt 0 ]]; do
  case "$1" in
  --expect)
    if [[ $# -lt 2 ]]; then
      echo "[expect-fatal] --expect needs a text argument" >&2
      exit 2
    fi
    expects+=("$2")
    shift 2
    ;;
  --)
    shift
    break
    ;;
  -*)
    echo "[expect-fatal] unknown option: $1" >&2
    exit 2
    ;;
  *)
    break
    ;;
  esac
done

if [[ $# -eq 0 ]]; then
  echo "[expect-fatal] usage: expect_fatal.sh [--expect <text>]... <command...>" >&2
  exit 2
fi

log=$(mktemp)
trap 'rm -f -- "$log"' EXIT

# The command's own stderr goes to the log (inner redirect wins); the group's stderr is discarded on
# purpose — a run that dies of SIGABRT (the SystemC path) makes bash print a "Aborted (core dumped)"
# job notice of its own, which is noise next to the diagnosis we are about to print.
{ "$@" >"$log" 2>&1; } 2>/dev/null
status=$?
cat "$log"
if grep -q "WEAVE TEST PASSED" "$log"; then
  echo "[expect-fatal] FAIL: the run reached the pass marker instead of dying" >&2
  exit 1
fi
if ! grep -q "weave: fatal:" "$log"; then
  echo "[expect-fatal] FAIL: no 'weave: fatal:' headline in the output" >&2
  exit 1
fi
for want in ${expects[@]+"${expects[@]}"}; do
  if ! grep -qF -- "$want" "$log"; then
    echo "[expect-fatal] FAIL: the output lacks the expected text: $want" >&2
    exit 1
  fi
done
if [[ $status -eq 0 ]]; then
  echo "[expect-fatal] FAIL: the run exited 0 (the fatal path must not return)" >&2
  exit 1
fi

echo "[expect-fatal] ok: the run terminated on the fatal path (exit $status)"
