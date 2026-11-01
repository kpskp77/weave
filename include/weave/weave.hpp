#pragma once

// Weave: a behaviour orchestration layer between C++ test logic and EDA simulation kernels.
// The API is modelled after Python asyncio and does not use C++20 coroutines.
// Blocking semantics are carried entirely by the backend: each task runs in a simulation
// process context, and sleep/wait freeze the whole C++ call stack via a backend suspension
// point.
//
// This header is the umbrella — depending on <weave/weave.hpp> alone yields the whole public
// API. Header layering (dependencies point downwards):
//   time.hpp     time representation (int64 femtoseconds + literals)
//   task.hpp     task primitives (Task / create_task / sleep / CancelledError)
//   event.hpp    event primitive (Event)
//   locks.hpp    communication primitives (Lock / LockGuard / Condition)
//   queue.hpp    communication primitive (Queue / QueueEmpty / QueueFull)
//   semaphore.hpp communication primitives (Semaphore / SemaphoreGuard / BoundedSemaphore /
//                  BoundedSemaphoreGuard)
//   barrier.hpp  communication primitive (Barrier / BrokenBarrierError)
//   sync.hpp     combinators (gather / wait_for / wait / shield / as_completed / TaskGroup)
//   test.hpp     test registration (WEAVE_TEST)

#include <weave/barrier.hpp>
#include <weave/event.hpp>
#include <weave/locks.hpp>
#include <weave/queue.hpp>
#include <weave/semaphore.hpp>
#include <weave/sync.hpp>
#include <weave/task.hpp>
#include <weave/test.hpp>
#include <weave/time.hpp>
