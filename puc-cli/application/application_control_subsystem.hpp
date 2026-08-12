#pragma once

/**
 * @file application_control_subsystem.hpp
 * @brief Lifecycle adapter for deferred application-exit control.
 */

#include <csignal>
#include <memory>

#include "puc-cli/application/application_control.hpp"
#include "state/state.hpp"

namespace puc::app {

/**
 * Own durable exit control and process termination-signal handling.
 *
 * In TUI mode initialize() installs handlers for SIGINT and SIGTERM, and
 * terminate() restores the dispositions that preceded the application. The
 * handlers remain installed across every restartable start()/stop() cycle.
 * TEST mode never changes process signal dispositions.
 */
class ApplicationControlSubsystem final : public AppSubsystem {
 public:
  /** Construct an independent root subsystem. */
  ApplicationControlSubsystem();

  /** Restore any remaining process handlers and destroy control state. */
  ~ApplicationControlSubsystem() override;

  /** Create fresh exit control and install TUI process-signal handlers. */
  Status initialize(AppState& app) override;

  /** Preserve and expose the durable control during a running generation. */
  Status start(AppState& app) override;

  /** Preserve exit state across a suspend-style stop. */
  Status stop(AppState& app) noexcept override;

  /** Release the control after the final application generation. */
  Status terminate(AppState& app) noexcept override;

  /** Return the initialized control, or nullptr outside its lifetime. */
  ApplicationControl* control() noexcept { return control_.get(); }

  /** Return the initialized control, or nullptr outside its lifetime. */
  const ApplicationControl* control() const noexcept { return control_.get(); }

  /** Return whether a command or process termination signal requested exit. */
  bool exit_requested() const noexcept;

 private:
  struct SignalHandlers;

  /** Record a signal request without invoking lifecycle or library code. */
  static void handle_termination_signal(int signal_number) noexcept;

  /** Install process-global handlers while preserving prior dispositions. */
  Status install_signal_handlers();

  /** Restore dispositions retained by install_signal_handlers(). */
  Status restore_signal_handlers() noexcept;

  /** Process-global async-signal-safe request written only by the handler. */
  static volatile std::sig_atomic_t termination_signal_requested_;

  /** Enforce the process-global single-owner nature of signal dispositions. */
  static ApplicationControlSubsystem* signal_owner_;

  std::unique_ptr<ApplicationControl>
      control_; /**< Durable request state for one initialized lifetime. */
  std::unique_ptr<SignalHandlers>
      signal_handlers_; /**< Saved dispositions while this adapter owns them. */
};

}  // namespace puc::app
