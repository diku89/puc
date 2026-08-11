/**
 * @file bootstrap.cpp
 * @brief Canonical application subsystem registration implementation.
 */

#include "puc-cli/state/bootstrap.hpp"

#include <memory>
#include <utility>

#include "puc-cli/state/channels.hpp"
#include "puc-cli/state/command_mode.hpp"
#include "puc-cli/state/commands.hpp"
#include "puc-cli/state/configuration.hpp"
#include "puc-cli/state/directory.hpp"
#include "puc-cli/state/embedded_terminal.hpp"
#include "puc-cli/state/input.hpp"
#include "puc-cli/state/logger.hpp"
#include "puc-cli/state/metronome.hpp"
#include "puc-cli/state/presentation.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/state/workers.hpp"

namespace puc::app {

Status register_application_subsystems(AppState& app,
                                       ApplicationSubsystemOptions options) {
  if (app.size() != 0U) {
    return Status::INVALID_ARGUMENT;
  }
  const ApplicationSubsystemSelection& selection = options.selection;
  if ((selection.command_mode && (!selection.commands || !selection.input)) ||
      (selection.embedded_terminal && !selection.input)) {
    return Status::INVALID_ARGUMENT;
  }

  auto logger = std::make_unique<LoggerSubsystem>(std::move(options.logger));
  auto configuration = std::make_unique<ConfigurationSubsystem>(
      std::move(options.configuration));
  auto workers   = std::make_unique<WorkerSubsystem>(options.worker_count);
  auto directory = std::make_unique<DirectorySubsystem>();
  auto screen_channels = std::make_unique<ScreenChannelSubsystem>();
  auto terminal =
      std::make_unique<TerminalSubsystem>(std::move(options.terminal));
  auto screen = std::make_unique<ScreenSubsystem>(std::move(options.screen));

  Status status = app.register_subsystem(std::move(logger));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(configuration));
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
  status = app.register_subsystem(std::move(terminal));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(screen));
  if (!is_ok(status)) {
    return status;
  }
  if (selection.commands || selection.input) {
    status = app.register_subsystem(
        std::make_unique<CommandNotificationChannelSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.metronome) {
    status = app.register_subsystem(std::make_unique<MetronomeSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.presentation) {
    status = app.register_subsystem(std::make_unique<PresentationSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.commands) {
    status = app.register_subsystem(std::make_unique<CommandSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.input) {
    status = app.register_subsystem(std::make_unique<InputSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.command_mode) {
    status = app.register_subsystem(std::make_unique<CommandModeSubsystem>());
    if (!is_ok(status)) {
      return status;
    }
  }
  if (selection.embedded_terminal) {
    status = app.register_subsystem(std::make_unique<EmbeddedTerminalSubsystem>(
        std::move(options.embedded_terminal)));
    if (!is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
}

}  // namespace puc::app
