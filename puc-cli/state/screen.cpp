/**
 * @file screen.cpp
 * @brief Terminal presentation subsystem implementation.
 */

#include "puc-cli/state/screen.hpp"

#include <memory>
#include <utility>

#include "puc-cli/state/directory.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/terminal/session.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/status.hpp"

namespace puc::app {

ScreenSubsystem::ScreenSubsystem(ScreenSubsystemOptions options)
    : AppSubsystem("screen", subsystem_dependencies<TerminalSubsystem>()),
      options_(std::move(options)) {}

ScreenSubsystem::~ScreenSubsystem() = default;

Status ScreenSubsystem::initialize(AppState& app) {
  return app.get_subsystem<TerminalSubsystem>() == nullptr ||
                 app.get_subsystem<DirectorySubsystem>() == nullptr
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
  if (app.operating_mode() != OperatingMode::TUI || !options_.take_terminal) {
    return Status::OK;
  }

  screen_status_ = options_.session_options.has_value()
                       ? screen_->take(*options_.session_options)
                       : screen_->take();
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
