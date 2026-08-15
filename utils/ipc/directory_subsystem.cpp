/**
 * @file directory_subsystem.cpp
 * @brief IPC channel-directory subsystem implementation.
 */

#include "utils/ipc/directory_subsystem.hpp"

#include <memory>

#include "properties/properties_subsystem.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/settings.hpp"
#include "utils/multithreading/job_queue.hpp"
#include "utils/multithreading/worker_subsystem.hpp"

namespace puc::app {

DirectorySubsystem::DirectorySubsystem()
    : AppSubsystem(
          "channel-directory",
          subsystem_dependencies<PropertiesSubsystem, WorkerSubsystem>()) {}

DirectorySubsystem::~DirectorySubsystem() = default;

Status DirectorySubsystem::initialize(AppState& app) {
  PropertiesSubsystem* properties = app.get_subsystem<PropertiesSubsystem>();
  if (properties == nullptr || properties->properties() == nullptr ||
      app.get_subsystem<WorkerSubsystem>() == nullptr) {
    return Status::SUBSYSTEM_NOT_FOUND;
  }
  ipc::Settings settings;
  if (!ipc::load_settings(*properties->properties(), settings)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  maximum_message_bytes_ = settings.maximum_message_bytes;
  return Status::OK;
}

Status DirectorySubsystem::start(AppState& app) {
  if (directory_ != nullptr) {
    return Status::OK;
  }
  WorkerSubsystem* worker_subsystem = app.get_subsystem<WorkerSubsystem>();
  if (worker_subsystem == nullptr || worker_subsystem->workers() == nullptr ||
      !worker_subsystem->workers()->active() || maximum_message_bytes_ == 0U) {
    return Status::SUBSYSTEM_FAILURE;
  }
  directory_ = std::make_unique<ipc::Directory>(*worker_subsystem->workers());
  return Status::OK;
}

Status DirectorySubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  directory_.reset();
  return Status::OK;
}

Status DirectorySubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  directory_.reset();
  maximum_message_bytes_ = 0U;
  return Status::OK;
}

}  // namespace puc::app
