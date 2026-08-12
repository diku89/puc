/**
 * @file timer.cpp
 * @brief Timer scheduler lifecycle implementation.
 */

#include "state/timer.hpp"

#include <memory>

#include "state/workers.hpp"
#include "utils/timer/scheduler.hpp"

namespace puc::app {

TimerSubsystem::TimerSubsystem()
    : AppSubsystem("timer", subsystem_dependencies<WorkerSubsystem>()) {}

TimerSubsystem::~TimerSubsystem() = default;

Status TimerSubsystem::initialize(AppState& app) {
  return app.get_subsystem<WorkerSubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status TimerSubsystem::start(AppState& app) {
  if (scheduler_ != nullptr) {
    return Status::OK;
  }
  WorkerSubsystem* workers = app.get_subsystem<WorkerSubsystem>();
  if (workers == nullptr || workers->workers() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  scheduler_ = std::make_unique<timer::Scheduler>(*workers->workers());
  return Status::OK;
}

Status TimerSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  scheduler_.reset();
  return Status::OK;
}

Status TimerSubsystem::terminate(AppState& app) noexcept { return stop(app); }

}  // namespace puc::app
