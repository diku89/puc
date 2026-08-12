#pragma once

/**
 * @file tui_test_runtime.hpp
 * @brief Primary lifecycle-owned logic for the TUI rendering smoke test.
 */

#include <memory>

#include "state/state.hpp"

namespace puc::app {

/**
 * Own the app-specific state for each visual smoke-test generation.
 *
 * Screen, presentation, theme, workers, terminal ownership, and process-exit
 * handling remain in their reusable lifecycle subsystems. This adapter owns
 * only the smoke test's frames, layouts, Canvas, metrics, and draw policy.
 */
class TuiTestRuntimeSubsystem final : public AppSubsystem {
 public:
  /** Declare the Screen, presentation, and theme dependencies. */
  TuiTestRuntimeSubsystem();

  /** Destroy runtime state already released by terminate(). */
  ~TuiTestRuntimeSubsystem() override;

  /** Allocate durable adapter bookkeeping without acquiring resources. */
  Status initialize(AppState& app) override;

  /** Construct app-specific state over the current running generation. */
  Status start(AppState& app) override;

  /** Quiesce rendering and release generation-bound app state. */
  Status stop(AppState& app) noexcept override;

  /** Release all adapter bookkeeping after the final generation. */
  Status terminate(AppState& app) noexcept override;

  /** Present one complete visual smoke-test frame. */
  bool draw();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden app-specific runtime state. */
};

}  // namespace puc::app
