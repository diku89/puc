#pragma once

/**
 * @file embedded_terminal_subsystem.hpp
 * @brief Lifecycle owner for the integrated-terminal PTY child.
 */

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include "puc-cli/tui/terminal/event.hpp"
#include "state/state.hpp"

namespace puc::tui {
class InputFrame;
}

namespace puc::app {

/** Stable launch policy retained across embedded-terminal generations. */
struct EmbeddedTerminalSubsystemOptions {
  /**
   * Interactive shell executable, or empty to use the user's configured
   * login shell with `/bin/sh` as the final portability fallback.
   */
  std::string shell;
};

/**
 * Own the PTY master and exactly one child process while the app is running.
 *
 * InputFrame retains durable virtual-terminal state. This adapter observes its
 * requested session generation, starts/resizes the shell lazily, translates
 * normalized input back to PTY bytes, pumps child output into libtmt, and reaps
 * the child synchronously on stop before InputSubsystem can be stopped.
 */
class EmbeddedTerminalSubsystem final : public AppSubsystem {
 public:
  /** Retain the shell launch policy and declare the InputFrame dependency. */
  explicit EmbeddedTerminalSubsystem(
      EmbeddedTerminalSubsystemOptions options = {});

  /** Destroy already-reaped process state. */
  ~EmbeddedTerminalSubsystem() override;

  /** Bind the durable InputFrame and allocate inactive PTY state. */
  Status initialize(AppState& app) override;

  /** Enable lazy PTY generations for the running application. */
  Status start(AppState& app) override;

  /** Close the PTY and synchronously terminate/reap its owned child. */
  Status stop(AppState& app) noexcept override;

  /** Release durable PTY state and the InputFrame binding. */
  Status terminate(AppState& app) noexcept override;

  /** Translate and queue one normalized event for the active child. */
  Status send_event(const terminal::Event& event);

  /** Start, resize, pump, or reap the session requested by InputFrame. */
  Status synchronize(std::size_t screen_width, std::size_t screen_height);

  /** Return whether this running generation currently owns a live child. */
  bool child_running() const noexcept;

  /** Return the InputFrame terminal-session generation served by the child. */
  std::size_t child_generation() const noexcept;

 private:
  class Impl;

  EmbeddedTerminalSubsystemOptions
      options_;              /**< Durable shell launch policy. */
  mutable std::mutex mutex_; /**< Serializes lifecycle and nonblocking I/O. */
  std::shared_ptr<tui::InputFrame>
      input_frame_;            /**< Durable frame owned by InputSubsystem. */
  std::unique_ptr<Impl> impl_; /**< PTY descriptor, child, and pending bytes. */
  bool active_ = false; /**< Whether PTY work is accepted in this generation. */
};

}  // namespace puc::app
