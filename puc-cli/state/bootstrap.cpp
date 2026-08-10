/**
 * @file bootstrap.cpp
 * @brief Canonical application subsystem registration implementation.
 */

#include "puc-cli/state/bootstrap.hpp"

#include <memory>
#include <utility>

#include "puc-cli/state/channels.hpp"
#include "puc-cli/state/commands.hpp"
#include "puc-cli/state/directory.hpp"
#include "puc-cli/state/input.hpp"
#include "puc-cli/state/logger.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/state/workers.hpp"

namespace puc::app {

Status register_application_subsystems(AppState& app,
                                       ApplicationSubsystemOptions options) {
  if (app.size() != 0U) {
    return Status::INVALID_ARGUMENT;
  }

  auto logger    = std::make_unique<LoggerSubsystem>(std::move(options.logger));
  auto workers   = std::make_unique<WorkerSubsystem>(options.worker_count);
  auto directory = std::make_unique<DirectorySubsystem>();
  auto screen_channels = std::make_unique<ScreenChannelSubsystem>();
  auto command_notifications =
      std::make_unique<CommandNotificationChannelSubsystem>();
  auto terminal =
      std::make_unique<TerminalSubsystem>(std::move(options.terminal));
  auto screen   = std::make_unique<ScreenSubsystem>(std::move(options.screen));
  auto commands = std::make_unique<CommandSubsystem>();
  auto input    = std::make_unique<InputSubsystem>();

  Status status = app.register_subsystem(std::move(logger));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(workers));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(directory));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(screen_channels));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(command_notifications));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(terminal));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(screen));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(commands));
  if (!is_ok(status)) {
    return status;
  }
  return app.register_subsystem(std::move(input));
}

}  // namespace puc::app
