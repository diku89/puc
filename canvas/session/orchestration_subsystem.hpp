#pragma once

/**
 * @file orchestration_subsystem.hpp
 * @brief Presentation orchestration nodes for the extensible Turn pipeline.
 */

#include <memory>
#include <string_view>

#include "state/state.hpp"

namespace puc::canvas {
class PresentationTree;
}

namespace puc::app {

/** Register and own presentation preparation and commit graph nodes. */
class OrchestrationSubsystem final : public AppSubsystem {
 public:
  /** Stable graph node that incrementally derives a new presentation root. */
  static constexpr std::string_view kLinearizeNode = "orchestration.linearize";

  /** Stable graph node that persists and publishes a presentation commit. */
  static constexpr std::string_view kCommitPresentationNode =
      "orchestration.commit_presentation";

  /** Construct an uninitialized orchestration lifecycle adapter. */
  OrchestrationSubsystem();

  /** Destroy state released by terminate(). */
  ~OrchestrationSubsystem() override;

  /** Restore presentation state and register this subsystem's graph nodes. */
  Status initialize(AppState& app) override;

  /** Validate that the Canvas subsystem is active for publication. */
  Status start(AppState& app) override;

  /** Quiesce this stateless lifecycle generation. */
  Status stop(AppState& app) noexcept override;

  /** Unregister owned nodes and release presentation state. */
  Status terminate(AppState& app) noexcept override;

  /** Return the current materialized Presentation tree for inspection. */
  canvas::PresentationTree* presentation_tree() noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden orchestration and Merkle state. */
};

}  // namespace puc::app
