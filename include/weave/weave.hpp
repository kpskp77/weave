#pragma once

// Weave: a behaviour orchestration layer between C++ test logic and EDA simulation kernels.
// The API is modelled after Python asyncio and does not use C++20 coroutines.
// Blocking semantics are carried entirely by the backend: each task runs in a simulation
// process context, and sleep/wait freeze the whole C++ call stack via a backend suspension
// point.
//
// This header is the umbrella — depending on <weave/weave.hpp> alone yields the whole public
// API. Header layering (dependencies point downwards):
//   config.hpp   backend selection (compile-time macro, SVDPI by default)
//   time.hpp     time representation (int64 femtoseconds + literals)
//   task.hpp     task primitives (Task / create_task / sleep / Cancelled)
//   event.hpp    event primitive (Event)
//   locks.hpp    communication primitives (Lock / LockGuard / Condition)
//   semaphore.hpp communication primitives (Semaphore / SemaphoreGuard / BoundedSemaphore /
//                  BoundedSemaphoreGuard)
//   sync.hpp     combinators (gather / wait_for / wait / shield / as_completed / TaskGroup)
//   test.hpp     test registration (WEAVE_TEST)

#include <weave/config.hpp>
#include <weave/event.hpp>
#include <weave/locks.hpp>
#include <weave/semaphore.hpp>
#include <weave/sync.hpp>
#include <weave/task.hpp>
#include <weave/test.hpp>
#include <weave/time.hpp>
