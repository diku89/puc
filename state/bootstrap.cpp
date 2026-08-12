/**
 * @file bootstrap.cpp
 * @brief Canonical application subsystem registration implementation.
 */

#include "state/bootstrap.hpp"

#include <memory>
#include <utility>

#include "state/builtin_commands.hpp"
#include "state/channels.hpp"
#include "state/command_mode.hpp"
#include "state/commands.hpp"
#include "state/control.hpp"
#include "state/directory.hpp"
#include "state/embedded_terminal.hpp"
#include "state/input.hpp"
#include "state/logger.hpp"
#include "state/metronome.hpp"
#include "state/presentation.hpp"
#include "state/properties.hpp"
#include "state/screen.hpp"
#include "state/terminal.hpp"
#include "state/theme.hpp"
#include "state/timer.hpp"
#include "state/workers.hpp"

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

  auto logger  = std::make_unique<LoggerSubsystem>(std::move(options.logger));
  auto control = std::make_unique<ApplicationControlSubsystem>();
  auto properties =
      std::make_unique<PropertiesSubsystem>(std::move(options.properties));
  auto workers   = std::make_unique<WorkerSubsystem>(options.worker_count);
  auto timer     = std::make_unique<TimerSubsystem>();
  auto theme     = std::make_unique<ThemeSubsystem>();
  auto directory = std::make_unique<DirectorySubsystem>();
  auto screen_channels = std::make_unique<ScreenChannelSubsystem>();
  auto terminal_input_channel =
      std::make_unique<TerminalInputChannelSubsystem>();
  auto terminal =
      std::make_unique<TerminalSubsystem>(std::move(options.terminal));
  auto screen = std::make_unique<ScreenSubsystem>();

  Status status = app.register_subsystem(std::move(logger));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(control));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(properties));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(workers));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(timer));
  if (!is_ok(status)) {
    return status;
  }
  status = app.register_subsystem(std::move(theme));
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
  status = app.register_subsystem(std::move(terminal_input_channel));
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
    status =
        app.register_subsystem(std::make_unique<BuiltinCommandSubsystem>());
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
