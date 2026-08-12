/**
 * @file metronome.cpp
 * @brief Production heartbeat lifecycle implementation.
 */

#include "puc-cli/state/metronome.hpp"

#include <memory>

#include "puc-cli/state/directory.hpp"
#include "puc-cli/state/timer.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/metronome/metronome.hpp"
#include "utils/timer/scheduler.hpp"

namespace puc::app {

MetronomeSubsystem::MetronomeSubsystem()
    : AppSubsystem(
          "metronome",
          subsystem_dependencies<DirectorySubsystem, TimerSubsystem>()) {}

MetronomeSubsystem::~MetronomeSubsystem() = default;

Status MetronomeSubsystem::initialize(AppState& app) {
  return app.get_subsystem<DirectorySubsystem>() == nullptr ||
                 app.get_subsystem<TimerSubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status MetronomeSubsystem::start(AppState& app) {
  if (metronome_ != nullptr) {
    return Status::OK;
  }
  DirectorySubsystem* directory = app.get_subsystem<DirectorySubsystem>();
  TimerSubsystem* timer         = app.get_subsystem<TimerSubsystem>();
  if (directory == nullptr || directory->directory() == nullptr ||
      timer == nullptr || timer->scheduler() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  auto publisher = std::make_unique<metronome::Metronome>(
      *directory->directory(), *timer->scheduler());
  status_ = publisher->start();
  if (!metronome::is_ok(status_)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  metronome_ = std::move(publisher);
  return Status::OK;
}

Status MetronomeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (metronome_ != nullptr) {
    metronome_->stop();
    metronome_.reset();
  }
  return Status::OK;
}

Status MetronomeSubsystem::terminate(AppState& app) noexcept {
  return stop(app);
}

}  // namespace puc::app
