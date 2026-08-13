/**
 * @file screen_subsystem.cpp
 * @brief Terminal presentation subsystem implementation.
 */

#include "puc-cli/tui/rendering/screen_subsystem.hpp"

#include <memory>

#include "puc-cli/tui/rendering/screen.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/terminal/session.hpp"
#include "puc-cli/tui/terminal/terminal_subsystem.hpp"
#include "utils/ipc/channel_subsystems.hpp"
#include "utils/ipc/directory_subsystem.hpp"

namespace puc::app {

ScreenSubsystem::ScreenSubsystem()
    : AppSubsystem("screen",
                   subsystem_dependencies<TerminalSubsystem, DirectorySubsystem,
                                          TerminalInputChannelSubsystem>()) {}

ScreenSubsystem::~ScreenSubsystem() = default;

Status ScreenSubsystem::initialize(AppState& app) {
  return app.get_subsystem<TerminalSubsystem>() == nullptr ||
                 app.get_subsystem<DirectorySubsystem>() == nullptr ||
                 app.get_subsystem<TerminalInputChannelSubsystem>() == nullptr
             ? Status::SUBSYSTEM_NOT_FOUND
             : Status::OK;
}

Status ScreenSubsystem::start(AppState& app) {
  if (screen_ != nullptr) {
    return Status::OK;
  }
  TerminalSubsystem* terminal_subsystem =
      app.get_subsystem<TerminalSubsystem>();
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (terminal_subsystem == nullptr ||
      terminal_subsystem->session() == nullptr ||
      directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  screen_ = std::make_unique<tui::Screen>(*directory_subsystem->directory(),
                                          *terminal_subsystem->session());
  screen_status_ = screen_->setup_status();
  if (!tui::is_ok(screen_status_)) {
    screen_.reset();
    return Status::SUBSYSTEM_FAILURE;
  }
  if (app.operating_mode() != OperatingMode::TUI) {
    return Status::OK;
  }

  screen_status_ = screen_->take();
  if (!tui::is_ok(screen_status_)) {
    screen_.reset();
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status ScreenSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (screen_ == nullptr) {
    return Status::OK;
  }
  screen_status_ = screen_->release();
  screen_.reset();
  return tui::is_ok(screen_status_) ? Status::OK : Status::SUBSYSTEM_FAILURE;
}

Status ScreenSubsystem::terminate(AppState& app) noexcept { return stop(app); }

}  // namespace puc::app
