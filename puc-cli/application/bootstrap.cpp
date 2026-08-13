/**
 * @file bootstrap.cpp
 * @brief Canonical application subsystem registration implementation.
 */

#include "puc-cli/application/bootstrap.hpp"

#include <memory>
#include <utility>

#include "commands/builtin_command_subsystem.hpp"
#include "commands/command_mode_subsystem.hpp"
#include "commands/command_subsystem.hpp"
#include "properties/properties_subsystem.hpp"
#include "puc-cli/application/application_control_subsystem.hpp"
#include "puc-cli/tui/frames/input_subsystem.hpp"
#include "puc-cli/tui/rendering/presentation_subsystem.hpp"
#include "puc-cli/tui/rendering/screen_subsystem.hpp"
#include "puc-cli/tui/terminal/embedded_terminal_subsystem.hpp"
#include "puc-cli/tui/terminal/terminal_subsystem.hpp"
#include "themes/theme_subsystem.hpp"
#include "utils/ipc/channel_subsystems.hpp"
#include "utils/ipc/directory_subsystem.hpp"
#include "utils/logger/logger_subsystem.hpp"
#include "utils/metronome/metronome_subsystem.hpp"
#include "utils/multithreading/worker_subsystem.hpp"
#include "utils/timer/timer_subsystem.hpp"

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
