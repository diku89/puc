#pragma once

/**
 * @file input_test_runtime.hpp
 * @brief Primary lifecycle-owned logic for the InputFrame manual test app.
 */

#include <memory>

#include "puc-cli/state/state.hpp"

namespace puc::app {

/**
 * Own the complete app-specific input-test generation.
 *
 * Shared terminal, screen, rendering, frame, theme, command, and embedded-PTY
 * mechanisms remain in their dedicated subsystems. This runtime owns only the
 * test app's layouts, Canvas generation, event-routing policy, and draw loop.
 */
class InputTestRuntimeSubsystem final : public AppSubsystem {
 public:
  /** Declare every shared mechanism consumed by the test app. */
  InputTestRuntimeSubsystem();

  /** Destroy runtime state already released by terminate(). */
  ~InputTestRuntimeSubsystem() override;

  /** Allocate durable runtime bookkeeping without starting I/O. */
  Status initialize(AppState& app) override;

  /** Bind the current subsystem generations and construct presentation state.
   */
  Status start(AppState& app) override;

  /** Quiesce rendering and subscriptions before dependencies stop. */
  Status stop(AppState& app) noexcept override;

  /** Release all durable test-app state after the final generation. */
  Status terminate(AppState& app) noexcept override;

  /** Poll input and present one complete frame. */
  bool draw();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden app-specific runtime state. */
};

}  // namespace puc::app
