#pragma once

/**
 * @file terminal.hpp
 * @brief Lifecycle adapter for terminal transport and input decoding.
 */

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "puc-cli/state/state.hpp"
#include "puc-cli/terminal/decoder.hpp"
#include "puc-cli/terminal/status.hpp"
#include "puc-cli/terminal/timeouts.hpp"
#include "utils/timer/deadline.hpp"

namespace puc::terminal {
class TerminalSession;
}

namespace puc::ipc {
class Directory;
}

namespace puc::app {

/** Immutable construction and decoder setup inputs for TerminalSubsystem. */
struct TerminalSubsystemOptions {
  int input_fd  = 0; /**< Borrowed terminal input descriptor. */
  int output_fd = 1; /**< Borrowed terminal output descriptor. */
  terminal::DecoderLimits decoder_limits; /**< Untrusted-input limits. */
  bool configure_decoder =
      false;                 /**< Load the input Trie through Properties. */
  std::string terminal_name; /**< Explicit terminfo/profile name, or empty. */
};

/**
 * Own one TerminalSession, Decoder, and the Decoder's immutable input Trie.
 *
 * Initialization constructs both durable mechanisms without taking terminal
 * ownership. Each start reapplies the current Properties-backed Trie and
 * timeout settings, binds TerminalSession as the consumer of
 * ScreenChannelSubsystem's command route, and publishes decoded input through
 * TerminalInputChannelSubsystem. Screen remains the producer that requests
 * take, presentation, clipboard, and release operations.
 */
class TerminalSubsystem final : public AppSubsystem {
 public:
  /** Construct an adapter retaining terminal descriptors and decoder inputs. */
  explicit TerminalSubsystem(TerminalSubsystemOptions options = {});

  /** Destroy released terminal mechanisms. */
  ~TerminalSubsystem() override;

  /** Construct the durable TerminalSession and Decoder mechanisms. */
  Status initialize(AppState& app) override;

  /** Apply current properties and bind lifecycle-owned terminal channels. */
  Status start(AppState& app) override;

  /** Unsubscribe and synchronously restore any active terminal modes. */
  Status stop(AppState& app) noexcept override;

  /** Release the terminal mechanisms retained while stopped. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized terminal session, or nullptr outside its lifetime.
   */
  terminal::TerminalSession* session() noexcept { return session_.get(); }

  /** Return the initialized terminal session, or nullptr outside its lifetime.
   */
  const terminal::TerminalSession* session() const noexcept {
    return session_.get();
  }

  /** Return the initialized decoder, or nullptr outside its lifetime. */
  terminal::Decoder* decoder() noexcept { return decoder_.get(); }

  /** Return the initialized decoder, or nullptr outside its lifetime. */
  const terminal::Decoder* decoder() const noexcept { return decoder_.get(); }

  /** Return the borrowed terminal input descriptor. */
  int input_fd() const noexcept { return options_.input_fd; }

  /**
   * Poll, decode, and publish all currently available terminal input.
   *
   * A timeout is not an error: it also gives Decoder's pending ambiguity
   * deadline an opportunity to emit an event. `end_of_input` is reset on entry
   * and set when the descriptor closes or a read reaches EOF.
   */
  Status poll_input(std::chrono::milliseconds timeout, bool& end_of_input);

  /** Return the configured terminal input and click timing policy. */
  const terminal::TimeoutSettings& timeout_settings() const noexcept {
    return timeout_settings_;
  }

  /** Return the latest mechanism status observed by a lifecycle hook. */
  terminal::Status terminal_status() const noexcept { return terminal_status_; }

 private:
  /** Publish normalized events on the lifecycle-owned terminal route. */
  Status publish_events(const std::vector<terminal::Event>& events);

  /** Deliver Decoder's pending generation when its deadline has elapsed. */
  Status resolve_decoder_timeout();

  /** Synchronize the timer primitive with Decoder's current generation. */
  void refresh_decoder_timeout() noexcept;

  TerminalSubsystemOptions options_; /**< Inputs retained across restarts. */
  std::unique_ptr<terminal::TerminalSession>
      session_; /**< Transport retained from initialize through terminate. */
  std::unique_ptr<terminal::Decoder>
      decoder_; /**< Decoder and input Trie retained while initialized. */
  ipc::Directory* directory_ = nullptr; /**< Event route during one run. */
  terminal::TimeoutSettings timeout_settings_; /**< Input timing policy. */
  timer::TokenDeadline<terminal::TimeoutInput>
      decoder_timeout_; /**< Pending ambiguous-input generation. */
  terminal::Status terminal_status_ = terminal::Status::OK; /**< Detail. */
};

}  // namespace puc::app
