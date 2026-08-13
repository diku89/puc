#pragma once

/**
 * @file presentation_subsystem.hpp
 * @brief Lifecycle adapter for parallel frame presentation work.
 */

#include <memory>

#include "puc-cli/tui/rendering/status.hpp"
#include "state/state.hpp"

namespace puc::tui {
class ParallelRenderer;
}

namespace puc::app {

/**
 * Own a ParallelRenderer for each running worker-pool generation.
 *
 * Layouts, themes, canvases, and Frames are durable presentation data owned by
 * an application-specific runtime subsystem. This adapter owns the active
 * scheduler that borrows WorkerSubsystem and guarantees that every render job
 * is joined before the worker generation is stopped.
 */
class PresentationSubsystem final : public AppSubsystem {
 public:
  /** Declare the shared worker-pool dependency. */
  PresentationSubsystem();

  /** Destroy a quiescent renderer generation. */
  ~PresentationSubsystem() override;

  /** Validate the registered worker-pool owner. */
  Status initialize(AppState& app) override;

  /** Construct a renderer borrowing the current worker generation. */
  Status start(AppState& app) override;

  /** Join active render work and release the current renderer. */
  Status stop(AppState& app) noexcept override;

  /** Release any renderer retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the running renderer, or nullptr while stopped. */
  tui::ParallelRenderer* renderer() noexcept { return renderer_.get(); }

  /** Return the running renderer, or nullptr while stopped. */
  const tui::ParallelRenderer* renderer() const noexcept {
    return renderer_.get();
  }

  /** Return the latest renderer status observed while quiescing. */
  tui::Status presentation_status() const noexcept { return renderer_status_; }

 private:
  std::unique_ptr<tui::ParallelRenderer>
      renderer_; /**< Scheduler for the current running generation. */
  tui::Status renderer_status_ = tui::Status::OK; /**< Last detailed status. */
};

}  // namespace puc::app
