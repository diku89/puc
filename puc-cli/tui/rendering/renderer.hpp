#pragma once

/**
 * @file renderer.hpp
 * @brief Parallel Z-buffer frame rendering and transactional publication.
 */

#include <cstddef>
#include <memory>
#include <optional>

#include "puc-cli/tui/rendering/canvas.hpp"
#include "puc-cli/tui/rendering/layout.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::tui {

/**
 * Render a ZBuffer through a reusable dependency graph on a borrowed pool.
 *
 * `start()` begins one Canvas transaction, optionally clears its base layer,
 * and executes the overlap dependencies cached in AbsoluteLayout. Disjoint
 * frames write non-intersecting Canvas rectangles concurrently; a front frame
 * becomes ready only after all intersecting prerequisites finish. The last
 * real frame to complete publishes or cancels the transaction, so the drawable
 * A/B buffer cannot change while another frame is writing. `wait()` collects
 * one reusable graph run without stopping or joining worker threads.
 *
 * The LayoutDescription, AbsoluteLayout, Theme, and Canvas must remain alive
 * and unmodified until `wait()` returns. Concrete frames capture their own
 * typed application state and may observe different state versions.
 */
class ParallelRenderer {
 public:
  /**
   * Borrow the fixed worker pool used for frame jobs and IPC delivery.
   *
   * The caller must keep the pool alive and accepting jobs through renderer
   * destruction. ParallelRenderer never stops or joins it.
   */
  explicit ParallelRenderer(multithreading::JobQueue& workers);

  ParallelRenderer(const ParallelRenderer&)            = delete;
  ParallelRenderer& operator=(const ParallelRenderer&) = delete;
  ParallelRenderer(ParallelRenderer&&)                 = delete;
  ParallelRenderer& operator=(ParallelRenderer&&)      = delete;

  /** Wait for an active batch before releasing renderer state. */
  ~ParallelRenderer();

  /**
   * Begin one asynchronous render batch.
   *
   * A supplied `base_cell` clears the writable image before frame jobs begin;
   * an absent value preserves the previously published image. Every frame is
   * scheduled according to the execution DAG cached by AbsoluteLayout. An
   * empty layout still publishes asynchronously through one publication-only
   * graph job because no real frame exists to finalize it.
   *
   * @return Status::OK once the batch is scheduled, an argument/Canvas/layout
   *         validation error before scheduling, or
   *         Status::FRAME_ALREADY_IN_PROGRESS when `wait()` has not consumed
   *         the previous batch. Frame and scheduling failures are reported by
   *         `wait()` after all scheduled work has quiesced.
   */
  Status start(std::shared_ptr<Layout::LayoutDescription> layout_description,
               const Layout::AbsoluteLayout& absolute_layout,
               const Theme& theme, std::shared_ptr<Canvas> canvas,
               std::optional<Canvas::Cell> base_cell = std::nullopt);

  /** Wait for the current batch and return its aggregate publication status. */
  Status wait() noexcept;

  /** Return whether a batch exists whose result has not yet been consumed. */
  bool active() const noexcept;

  /** Return the number of threads in the borrowed worker pool. */
  std::size_t worker_count() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Batch coordination hidden from consumers. */
};

}  // namespace puc::tui
