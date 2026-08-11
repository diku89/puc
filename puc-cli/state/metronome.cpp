/**
 * @file metronome.cpp
 * @brief Production heartbeat lifecycle implementation.
 */

#include "puc-cli/state/metronome.hpp"

#include <memory>

#include "puc-cli/state/directory.hpp"
#include "puc-cli/state/workers.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/metronome/metronome.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::app {

MetronomeSubsystem::MetronomeSubsystem()
    : AppSubsystem(
          "metronome",
          subsystem_dependencies<DirectorySubsystem, WorkerSubsystem>()) {}

MetronomeSubsystem::~MetronomeSubsystem() = default;

Status MetronomeSubsystem::initialize(AppState& app) {
  return app.get_subsystem<DirectorySubsystem>() == nullptr ||
                 app.get_subsystem<WorkerSubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status MetronomeSubsystem::start(AppState& app) {
  if (metronome_ != nullptr) {
    return Status::OK;
  }
  DirectorySubsystem* directory = app.get_subsystem<DirectorySubsystem>();
  WorkerSubsystem* workers      = app.get_subsystem<WorkerSubsystem>();
  if (directory == nullptr || directory->directory() == nullptr ||
      workers == nullptr || workers->workers() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  auto publisher = std::make_unique<metronome::Metronome>(
      *directory->directory(), *workers->workers());
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
