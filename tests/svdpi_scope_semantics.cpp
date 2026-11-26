// SVDPI backend self-check: weave fixes the DPI scope itself, so a consumer needs no svSetScope.
//
// A consumer's testbench process imports its own package (and usually not weave_pkg), so when C++
// is reached through the consumer's own DPI import, the current DPI scope has no weave_sv_* in it
// and VCS 2018.09 aborts the whole run with
//   Error-[DPI-DXFNF] … 'weave_sv_current_task' … is not defined or visible
// on the first suspension or identity query. src/backend/svdpi.cpp therefore switches the scope to
// weave_pkg around every import it makes (and restores the caller's on the way out). This file pins
// that by switching the scope to a module that deliberately imports nothing (the
// weave_foreign_scope instance in tests/weave_tb.sv) and calling the primitives from there.
//
// SVDPI-only: the SystemC payload compiles without svdpi.h, so this assertion body is linked into
// weave_smoke_svdpi only — the mirror image of scope_sc_thread_semantics.cpp, which is SC-only
// because it needs a raw sc_spawn.

#include <svdpi.h>

#include <stdexcept>
#include <weave/weave.hpp>

#include "case_runner.hpp"

using namespace weave;
using namespace weave::literals;
using weave_test::report;
using weave_test::run_case;

namespace {

// Plays the consumer's import entry: while it is alive, the current DPI scope is a module that has
// not imported weave_pkg, exactly as when C++ is entered through a consumer's own DPI import.
class ForeignScope {
public:
  explicit ForeignScope(svScope scope) : prev_(svGetScope()) { svSetScope(scope); }
  ~ForeignScope() {
    if (prev_ != nullptr) svSetScope(prev_);
  }
  ForeignScope(const ForeignScope&) = delete;
  ForeignScope& operator=(const ForeignScope&) = delete;

private:
  svScope prev_;
};

svScope foreign_scope() {
  const svScope s = svGetScopeFromName("weave_tb.u_foreign_scope");
  if (s == nullptr) throw std::logic_error("tb: weave_tb.u_foreign_scope not found");
  return s;
}

}  // namespace

WEAVE_TEST(svdpi_scope_semantics) {
  // case 1: the non-suspending queries resolve through a foreign scope, and the caller's scope is
  // restored on the way out (the consumer still needs it for its own exports)
  run_case(__func__, "queries", [&] {
    const svScope scope = foreign_scope();
    ForeignScope foreign(scope);
    if (now_ticks() < 0) throw std::logic_error("case1: now_ticks reported a negative clock");
    if (!current_task().has_value()) throw std::logic_error("case1: task identity lost");
    if (svGetScope() != scope) throw std::logic_error("case1: weave did not restore the DPI scope");
  });

  // case 2: a real suspension, plus the event/spawn machinery around it, under a foreign scope
  run_case(__func__, "suspension", [&] {
    const svScope scope = foreign_scope();
    ForeignScope foreign(scope);
    Event ev;
    auto setter = create_task("scope_setter", [&] { ev.set(); });
    ev.wait();  // the only blocking point, reached with the foreign scope current
    setter.await();
    if (!setter.done()) throw std::logic_error("case2: setter task did not finish");
    if (svGetScope() != scope) throw std::logic_error("case2: weave did not restore the DPI scope");
  });

  report(__func__);
}
