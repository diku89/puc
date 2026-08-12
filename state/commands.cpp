/**
 * @file commands.cpp
 * @brief Command registry and dispatcher subsystem implementation.
 */

#include "state/commands.hpp"

#include <memory>

#include "state/channels.hpp"
#include "state/control.hpp"
#include "state/directory.hpp"
#include "state/properties.hpp"
#include "state/screen.hpp"
#include "state/workers.hpp"

namespace puc::app {

CommandSubsystem::CommandSubsystem()
    : AppSubsystem(
          "commands",
          subsystem_dependencies<CommandNotificationChannelSubsystem,
                                 ApplicationControlSubsystem, ScreenSubsystem,
                                 WorkerSubsystem, DirectorySubsystem,
                                 PropertiesSubsystem>()) {}

Status CommandSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  dispatcher_ = std::make_unique<command::CommandDispatcher>();
  return Status::OK;
}

Status CommandSubsystem::start(AppState& app) {
  if (dispatcher_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  const command::CommonCommandArgs args = common_args(app);
  return args.workers == nullptr || args.screen == nullptr ||
                 args.directory == nullptr || args.properties == nullptr ||
                 args.control == nullptr
             ? Status::SUBSYSTEM_FAILURE
             : Status::OK;
}

Status CommandSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return Status::OK;
}

Status CommandSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(app);
  dispatcher_.reset();
  return Status::OK;
}

command::CommonCommandArgs CommandSubsystem::common_args(
    AppState& app) const noexcept {
  WorkerSubsystem* worker_subsystem = app.get_subsystem<WorkerSubsystem>();
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  ScreenSubsystem* screen_subsystem = app.get_subsystem<ScreenSubsystem>();
  ApplicationControlSubsystem* control_subsystem =
      app.get_subsystem<ApplicationControlSubsystem>();
  PropertiesSubsystem* properties_subsystem =
      app.get_subsystem<PropertiesSubsystem>();
  return command::CommonCommandArgs{
      .workers =
          worker_subsystem == nullptr ? nullptr : worker_subsystem->workers(),
      .screen =
          screen_subsystem == nullptr ? nullptr : screen_subsystem->screen(),
      .directory  = directory_subsystem == nullptr
                        ? nullptr
                        : directory_subsystem->directory(),
      .properties = properties_subsystem == nullptr
                        ? nullptr
                        : properties_subsystem->properties(),
      .control =
          control_subsystem == nullptr ? nullptr : control_subsystem->control(),
      .state = &app,
  };
}

}  // namespace puc::app
