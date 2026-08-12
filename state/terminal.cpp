/**
 * @file terminal.cpp
 * @brief Terminal transport and decoder subsystem implementation.
 */

#include "state/terminal.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "msgs/status.hpp"
#include "msgs/terminal_msgs.hpp"
#include "puc-cli/tui/terminal/event_messages.hpp"
#include "puc-cli/tui/terminal/session.hpp"
#include "puc-cli/tui/terminal/status.hpp"
#include "puc-cli/tui/terminal/timeouts.hpp"
#include "state/channels.hpp"
#include "state/directory.hpp"
#include "state/properties.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/status.hpp"
#include "utils/timer/poller.hpp"

namespace puc::app {

TerminalSubsystem::TerminalSubsystem(TerminalSubsystemOptions options)
    : AppSubsystem(
          "terminal",
          subsystem_dependencies<ScreenChannelSubsystem,
                                 TerminalInputChannelSubsystem,
                                 PropertiesSubsystem, DirectorySubsystem>()),
      options_(std::move(options)) {}

TerminalSubsystem::~TerminalSubsystem() = default;

Status TerminalSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  session_ = std::make_unique<terminal::TerminalSession>(options_.input_fd,
                                                         options_.output_fd);
  decoder_ = std::make_unique<terminal::Decoder>(options_.decoder_limits);
  terminal_status_ = terminal::Status::OK;
  return Status::OK;
}

Status TerminalSubsystem::start(AppState& app) {
  if (session_ == nullptr || decoder_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (session_->screen_channels_bound()) {
    return Status::OK;
  }
  if (options_.configure_decoder) {
    PropertiesSubsystem* properties = app.get_subsystem<PropertiesSubsystem>();
    if (properties == nullptr || properties->properties() == nullptr) {
      return Status::SUBSYSTEM_FAILURE;
    }
    terminal_status_ = decoder_->setup(
        *properties->properties(), options_.terminal_name, options_.output_fd);
    if (!terminal::is_ok(terminal_status_)) {
      return Status::SUBSYSTEM_FAILURE;
    }
    terminal_status_ = terminal::load_timeout_settings(
        *properties->properties(), timeout_settings_);
    if (!terminal::is_ok(terminal_status_)) {
      return Status::SUBSYSTEM_FAILURE;
    }
  }
  DirectorySubsystem* directory_subsystem =
      app.get_subsystem<DirectorySubsystem>();
  if (directory_subsystem == nullptr ||
      directory_subsystem->directory() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  terminal_status_ =
      session_->bind_screen_channels(*directory_subsystem->directory());
  if (!terminal::is_ok(terminal_status_)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (directory_subsystem->directory()->get_channel(
          msg::kTerminalInputEventChannel) == nullptr) {
    session_->unbind_screen_channels();
    terminal_status_ = terminal::Status::CHANNEL_SETUP_FAILED;
    return Status::SUBSYSTEM_FAILURE;
  }
  directory_ = directory_subsystem->directory();
  decoder_timeout_.cancel();
  return Status::OK;
}

Status TerminalSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (session_ == nullptr) {
    return Status::OK;
  }
  directory_ = nullptr;
  decoder_timeout_.cancel();
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

Status TerminalSubsystem::poll_input(std::chrono::milliseconds timeout,
                                     bool& end_of_input) {
  end_of_input = false;
  if (session_ == nullptr || decoder_ == nullptr || directory_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }

  const timer::PollResult readiness =
      timer::poll_readable(options_.input_fd, timeout);
  if (readiness.status == timer::Status::TIMED_OUT) {
    return resolve_decoder_timeout();
  }
  if (readiness.status == timer::Status::CLOSED) {
    end_of_input = true;
    return Status::OK;
  }
  if (!timer::is_ok(readiness.status) || !readiness.readable) {
    terminal_status_ = terminal::Status::TERMINAL_READ_FAILED;
    return Status::SUBSYSTEM_FAILURE;
  }

  std::vector<terminal::Event> events;
  std::size_t bytes_read = 0U;
  terminal_status_ =
      session_->read(*decoder_, events, bytes_read, end_of_input);
  if (!terminal::is_ok(terminal_status_)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  const Status publish_status = publish_events(events);
  if (!is_ok(publish_status)) {
    return publish_status;
  }
  refresh_decoder_timeout();
  return Status::OK;
}

Status TerminalSubsystem::publish_events(
    const std::vector<terminal::Event>& events) {
  if (directory_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  try {
    msg::TerminalInputEventCodec codec;
    for (const terminal::Event& event : events) {
      msg::TerminalInputEvent message;
      std::vector<std::uint8_t> payload;
      if (!msg::is_ok(terminal::to_message(event, message)) ||
          !msg::is_ok(codec.serialize(message, payload))) {
        terminal_status_ = terminal::Status::OUTPUT_LIMIT_EXCEEDED;
        return Status::SUBSYSTEM_FAILURE;
      }
      const ipc::TransferResult transfer =
          directory_->transmit(msg::kTerminalInputEventChannel, payload);
      if (!ipc::is_ok(transfer.status) || transfer.bytes != payload.size()) {
        terminal_status_ = terminal::Status::CHANNEL_SETUP_FAILED;
        return Status::SUBSYSTEM_FAILURE;
      }
    }
  } catch (...) {
    terminal_status_ = terminal::Status::OUTPUT_LIMIT_EXCEEDED;
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status TerminalSubsystem::resolve_decoder_timeout() {
  refresh_decoder_timeout();
  const std::optional<terminal::TimeoutInput> due =
      decoder_timeout_.take_if_due();
  if (!due.has_value()) {
    return Status::OK;
  }
  std::vector<terminal::Event> events;
  terminal_status_ = decoder_->handle_timeout(*due, events);
  if (!terminal::is_ok(terminal_status_)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  const Status status = publish_events(events);
  refresh_decoder_timeout();
  return status;
}

void TerminalSubsystem::refresh_decoder_timeout() noexcept {
  if (decoder_ == nullptr) {
    decoder_timeout_.cancel();
    return;
  }
  decoder_timeout_.synchronize(decoder_->pending_timeout(),
                               timeout_settings_.input_sequence);
}

}  // namespace puc::app
