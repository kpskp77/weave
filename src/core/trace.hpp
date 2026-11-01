#pragma once

// Trace instrumentation for the core: one cached simlog logger per module, all names under the
// "weave." prefix so SIMLOG_LEVEL addresses the whole tree at once (and a single module can still
// be narrowed):
//
//   SIMLOG_LEVEL="weave*=debug"       every core module at debug
//   SIMLOG_LEVEL="weave.task=trace"   only the task lifecycle, with full suspension detail
//
// Level convention: debug = lifecycle and failure-diagnosis events you want when a run hangs or
// fails (spawn / terminal state / cancellation / gather's first failure / wait_for timeout /
// TaskGroup cascade / barrier abort+reset / test start); trace = the hot bookkeeping paths
// (every suspension, event edges, lock/semaphore hand-offs, the barrier trip). The default level
// is info, so an enabled build stays silent unless SIMLOG_LEVEL says otherwise.
//
// The loggers are function-local statics of inline functions: one instance per module no matter
// how many TUs — or DSOs, since the smoke payloads inline the core sources — include this header
// (spdlog's registry dedupes by name anyway). spdlog checks the level before formatting, so a
// disabled trace costs one atomic load and the blocking point stays free.
//
// Layering: simlog is not a backend API — it carries its own per-backend time source exactly like
// weave carries its suspension, so core code depending on it does not touch the port contract and
// needs no backend knowledge here.

#include <simlog/simlog.h>

#include <memory>

namespace weave {
namespace detail {

inline std::shared_ptr<spdlog::logger> trace_task() {
  static const auto log = simlog::get("weave.task");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_event() {
  static const auto log = simlog::get("weave.event");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_locks() {
  static const auto log = simlog::get("weave.locks");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_semaphore() {
  static const auto log = simlog::get("weave.semaphore");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_barrier() {
  static const auto log = simlog::get("weave.barrier");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_sync() {
  static const auto log = simlog::get("weave.sync");
  return log;
}

inline std::shared_ptr<spdlog::logger> trace_registry() {
  static const auto log = simlog::get("weave.registry");
  return log;
}

}  // namespace detail
}  // namespace weave
