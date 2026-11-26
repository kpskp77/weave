#pragma once

// Internal entries used by the backend bootstrap. Not a public API; only src/backend may use it.

namespace weave {
namespace detail {

// Creates and spawns the root task (the first registered test).
// Returns the root task's done event id; -1 when no test is registered.
int run_registered_test();

// Root task state: 0=DONE 1=FAILED 2=CANCELLED -1=not finished
int root_status();

}  // namespace detail
}  // namespace weave
