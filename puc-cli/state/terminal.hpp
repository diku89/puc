#pragma once

/**
 * @file terminal.hpp
 * @brief Lifecycle adapter for terminal transport and input decoding.
 */

#include <memory>
#include <string>

#include "puc-cli/state/state.hpp"
#include "puc-cli/terminal/decoder.hpp"
#include "puc-cli/terminal/status.hpp"

namespace puc::terminal {
class TerminalSession;
}

namespace puc::app {

/** Immutable construction and decoder setup inputs for TerminalSubsystem. */
struct TerminalSubsystemOptions {
  int input_fd  = 0; /**< Borrowed terminal input descriptor. */
  int output_fd = 1; /**< Borrowed terminal output descriptor. */
  terminal::DecoderLimits decoder_limits; /**< Untrusted-input limits. */
  bool configure_decoder =
      false;                 /**< Load the input Trie from Configuration. */
  std::string terminal_name; /**< Explicit terminfo/profile name, or empty. */
};

/**
 * Own one TerminalSession, Decoder, and the Decoder's immutable input Trie.
 *
 * Initialization constructs and optionally configures both mechanisms without
 * taking terminal ownership. Start binds TerminalSession as the consumer of
 * ScreenChannelSubsystem's command route. Screen remains the producer that
 * requests take, presentation, clipboard, and release operations.
 */
class TerminalSubsystem final : public AppSubsystem {
 public:
  /** Construct an adapter retaining terminal descriptors and decoder inputs. */
  explicit TerminalSubsystem(TerminalSubsystemOptions options = {});

  /** Destroy released terminal mechanisms. */
  ~TerminalSubsystem() override;

  /** Construct and optionally configure TerminalSession and Decoder. */
  Status initialize(AppState& app) override;

  /** Subscribe TerminalSession to the lifecycle-owned Screen channels. */
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

  /** Return the latest mechanism status observed by a lifecycle hook. */
  terminal::Status terminal_status() const noexcept { return terminal_status_; }

 private:
  TerminalSubsystemOptions options_; /**< Inputs retained across restarts. */
  std::unique_ptr<terminal::TerminalSession>
      session_; /**< Transport retained from initialize through terminate. */
  std::unique_ptr<terminal::Decoder>
      decoder_; /**< Decoder and input Trie retained while initialized. */
  terminal::Status terminal_status_ = terminal::Status::OK; /**< Detail. */
};

}  // namespace puc::app
