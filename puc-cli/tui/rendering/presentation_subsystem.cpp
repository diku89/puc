/**
 * @file presentation_subsystem.cpp
 * @brief Parallel presentation lifecycle implementation.
 */

#include "puc-cli/tui/rendering/presentation_subsystem.hpp"

#include <memory>

#include "puc-cli/tui/rendering/renderer.hpp"
#include "utils/multithreading/job_queue.hpp"
#include "utils/multithreading/worker_subsystem.hpp"

namespace puc::app {

PresentationSubsystem::PresentationSubsystem()
    : AppSubsystem("presentation", subsystem_dependencies<WorkerSubsystem>()) {}

PresentationSubsystem::~PresentationSubsystem() = default;

Status PresentationSubsystem::initialize(AppState& app) {
  return app.get_subsystem<WorkerSubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status PresentationSubsystem::start(AppState& app) {
  if (renderer_ != nullptr) {
    return Status::OK;
  }
  WorkerSubsystem* workers = app.get_subsystem<WorkerSubsystem>();
  if (workers == nullptr || workers->workers() == nullptr ||
      !workers->workers()->active()) {
    return Status::SUBSYSTEM_FAILURE;
  }
  renderer_ = std::make_unique<tui::ParallelRenderer>(*workers->workers());
  renderer_status_ = tui::Status::OK;
  return Status::OK;
}

Status PresentationSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (renderer_ == nullptr) {
    return Status::OK;
  }
  renderer_status_ = renderer_->wait();
  renderer_.reset();
  return tui::is_ok(renderer_status_) ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status PresentationSubsystem::terminate(AppState& app) noexcept {
  return stop(app);
}

}  // namespace puc::app
