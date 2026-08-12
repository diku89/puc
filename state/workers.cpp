/**
 * @file workers.cpp
 * @brief Shared worker-pool subsystem implementation.
 */

#include "state/workers.hpp"

#include <cstdint>
#include <memory>

#include "state/logger.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::app {

WorkerSubsystem::WorkerSubsystem(std::uint8_t worker_count)
    : AppSubsystem("workers", subsystem_dependencies<LoggerSubsystem>()),
      worker_count_(worker_count) {}

WorkerSubsystem::~WorkerSubsystem() = default;

Status WorkerSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  return worker_count_ == 0U ? Status::INVALID_ARGUMENT : Status::OK;
}

Status WorkerSubsystem::start(AppState& app) {
  static_cast<void>(app);
  if (workers_ != nullptr) {
    return Status::OK;
  }
  workers_ = std::make_unique<multithreading::JobQueue>(worker_count_);
  return workers_->active() ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status WorkerSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  release_workers();
  return Status::OK;
}

Status WorkerSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  release_workers();
  return Status::OK;
}

void WorkerSubsystem::release_workers() noexcept {
  if (workers_ == nullptr) {
    return;
  }
  workers_->wait();
  workers_.reset();
}

}  // namespace puc::app
