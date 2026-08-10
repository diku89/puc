/**
 * @file terminal.cpp
 * @brief Terminal transport and decoder subsystem implementation.
 */

#include "puc-cli/state/terminal.hpp"

#include <memory>
#include <utility>

#include "puc-cli/state/channels.hpp"
#include "puc-cli/state/directory.hpp"
#include "puc-cli/terminal/session.hpp"
#include "puc-cli/terminal/status.hpp"
#include "utils/ipc/directory.hpp"

namespace puc::app {

TerminalSubsystem::TerminalSubsystem(TerminalSubsystemOptions options)
    : AppSubsystem("terminal",
                   subsystem_dependencies<ScreenChannelSubsystem>()),
      options_(std::move(options)) {}

TerminalSubsystem::~TerminalSubsystem() = default;

Status TerminalSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  session_ = std::make_unique<terminal::TerminalSession>(options_.input_fd,
                                                         options_.output_fd);
  decoder_ = std::make_unique<terminal::Decoder>(options_.decoder_limits);
  terminal_status_ = terminal::Status::OK;
  if (!options_.input_configuration.has_value()) {
    return Status::OK;
  }

  terminal_status_ =
      decoder_->setup(*options_.input_configuration, options_.terminal_name,
                      options_.output_fd);
  if (!terminal::is_ok(terminal_status_)) {
    decoder_.reset();
    session_.reset();
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status TerminalSubsystem::start(AppState& app) {
  if (session_ == nullptr || decoder_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (session_->screen_channels_bound()) {
    return Status::OK;
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  terminal_status_ =
      session_->bind_screen_channels(*directory_subsystem->directory());
  return terminal::is_ok(terminal_status_) ? Status::OK
                                           : Status::SUBSYSTEM_FAILURE;
}

Status TerminalSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (session_ == nullptr) {
    return Status::OK;
  }
  session_->unbind_screen_channels();
  terminal_status_ = session_->release();
  if (decoder_ != nullptr) {
    decoder_->reset();
  }
  return terminal::is_ok(terminal_status_) ? Status::OK
                                           : Status::SUBSYSTEM_FAILURE;
}

Status TerminalSubsystem::terminate(AppState& app) noexcept {
  const Status stop_status = stop(app);
  decoder_.reset();
  session_.reset();
  return stop_status;
}

}  // namespace puc::app
