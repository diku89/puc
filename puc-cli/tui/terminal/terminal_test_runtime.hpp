#pragma once

/**
 * @file terminal_test_runtime.hpp
 * @brief Lifecycle-owned runtime for the terminal input conformance app.
 */

#include <memory>
#include <optional>

#include "puc-cli/tui/terminal/terminal_test_runner.hpp"
#include "state/state.hpp"

namespace puc::app {

/**
 * Preserve one conformance plan while rebinding restartable shared services.
 *
 * Terminal decoding, screen ownership, presentation, theme, metronome, timer,
 * and process-exit handling remain in reusable subsystems. This adapter owns
 * only the manual test runner, its frames/layouts, selection state, and report.
 */
class TerminalTestRuntimeSubsystem final : public AppSubsystem {
 public:
  /** Retain the complete plan, or one selected test, until terminate(). */
  explicit TerminalTestRuntimeSubsystem(
      std::optional<terminal::InputConformanceTest> selected_test);

  /** Destroy runtime state already released by terminate(). */
  ~TerminalTestRuntimeSubsystem() override;

  /** Construct the durable conformance plan over initialized dependencies. */
  Status initialize(AppState& app) override;

  /** Bind the current Screen, renderer, theme, and heartbeat generation. */
  Status start(AppState& app) override;

  /** Release subscriptions and generation-bound presentation state. */
  Status stop(AppState& app) noexcept override;

  /** Release the durable plan after its final report has been consumed. */
  Status terminate(AppState& app) noexcept override;

  /** Poll terminal input and present one conformance frame. */
  bool draw();

  /** Return whether every selected check has produced a result. */
  bool finished() const;

  /** Print the durable report and return whether every planned check passed. */
  bool print_report() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden durable conformance state. */
};

}  // namespace puc::app
