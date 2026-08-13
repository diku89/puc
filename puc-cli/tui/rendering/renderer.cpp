/**
 * @file renderer.cpp
 * @brief Execution-graph-driven frame rendering and Canvas publication.
 */

#include "puc-cli/tui/rendering/renderer.hpp"

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/rendering/zbuf.hpp"
#include "utils/execution_graph/execution_graph.hpp"
#include "utils/execution_graph/status.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Parallel Renderer");
/** @endcond */

namespace puc::tui {
namespace {

/** Graph shape cached independently of one render generation's inputs. */
struct RenderTopology {
  std::size_t frame_count = 0U; /**< Number of executable frame nodes. */
  std::vector<Layout::FrameDependency>
      dependencies; /**< Back-to-front overlap edges. */

  /** Compare all node and edge data for graph reuse. */
  bool operator==(const RenderTopology&) const noexcept = default;
};

/** Mutable inputs and aggregate result shared by stable graph jobs. */
class RenderContext {
 public:
  /** Install the inputs used by the next graph invocation. */
  void prepare(std::shared_ptr<Layout::LayoutDescription> layout_description,
               const Layout::AbsoluteLayout& absolute_layout,
               const Theme& theme, std::shared_ptr<Canvas> canvas,
               std::size_t frame_count) noexcept {
    layout_description_ = std::move(layout_description);
    absolute_layout_    = &absolute_layout;
    theme_              = &theme;
    canvas_             = std::move(canvas);
    const std::lock_guard lock(status_mutex_);
    status_           = Status::OK;
    remaining_frames_ = frame_count;
    published_        = false;
  }

  /** Render one Z-buffer entry into its solved rectangle. */
  void render(std::size_t frame_index) noexcept {
    Status frame_status = Status::OK;
    const std::vector<ZBuffer::Entry>& frames =
        layout_description_->z_buffer.frames();
    if (frame_index >= frames.size()) {
      frame_status = Status::FRAME_NOT_FOUND;
    } else {
      const ZBuffer::Entry& entry = frames[frame_index];
      const auto rectangle =
          absolute_layout_->frame_layouts.find(entry.frame_id);
      if (rectangle == absolute_layout_->frame_layouts.end()) {
        Logger<ERROR> << "No rectangle for parallel frame '" << entry.frame_id
                      << "'";
        frame_status = Status::FRAME_NOT_FOUND;
      } else {
        frame_status = entry.frame->draw(*theme_, *canvas_, rectangle->second);
        if (!is_ok(frame_status)) {
          Logger<ERROR> << "Frame '" << entry.frame_id
                        << "' failed in parallel render: "
                        << status_message(frame_status);
        }
      }
    }
    finish_frame(frame_status);
  }

  /** Publish a successful transaction or cancel one containing an error. */
  void publish() noexcept {
    Status aggregate = Status::OK;
    {
      const std::lock_guard lock(status_mutex_);
      aggregate = status_;
    }

    const Status transaction_status =
        is_ok(aggregate) ? canvas_->end_frame() : canvas_->cancel_frame();
    {
      const std::lock_guard lock(status_mutex_);
      if (is_ok(status_)) {
        status_ = transaction_status;
      } else if (!is_ok(transaction_status)) {
        Logger<ERROR> << "Could not cancel failed Canvas transaction: "
                      << status_message(transaction_status);
      }
      published_ = true;
    }
  }

  /** Cancel a transaction when graph scheduling itself could not complete. */
  Status cancel_after_graph_failure() noexcept {
    bool requires_cancel = false;
    {
      const std::lock_guard lock(status_mutex_);
      requires_cancel = !published_;
      if (is_ok(status_)) {
        status_ = Status::ASYNC_DISPATCH_FAILED;
      }
    }
    if (requires_cancel) {
      const Status cancel_status = canvas_->cancel_frame();
      if (!is_ok(cancel_status)) {
        Logger<ERROR> << "Could not cancel interrupted Canvas transaction: "
                      << status_message(cancel_status);
      }
    }
    return result();
  }

  /** Return the first rendering or publication failure. */
  Status result() const noexcept {
    const std::lock_guard lock(status_mutex_);
    return status_;
  }

  /** Release per-run object ownership after every graph job has quiesced. */
  void reset() noexcept {
    layout_description_.reset();
    absolute_layout_ = nullptr;
    theme_           = nullptr;
    canvas_.reset();
    const std::lock_guard lock(status_mutex_);
    remaining_frames_ = 0U;
  }

 private:
  /** Record completion and make the last real frame publish the transaction. */
  void finish_frame(Status frame_status) noexcept {
    bool publish_now = false;
    {
      const std::lock_guard lock(status_mutex_);
      if (is_ok(status_) && !is_ok(frame_status)) {
        status_ = frame_status;
      }
      if (remaining_frames_ == 0U) {
        Logger<ERROR> << "Parallel frame completion count underflow";
        if (is_ok(status_)) {
          status_ = Status::ASYNC_DISPATCH_FAILED;
        }
        return;
      }
      --remaining_frames_;
      publish_now = remaining_frames_ == 0U;
    }
    if (publish_now) {
      publish();
    }
  }

  std::shared_ptr<Layout::LayoutDescription>
      layout_description_; /**< Stable frame ownership for one run. */
  const Layout::AbsoluteLayout* absolute_layout_ =
      nullptr;                      /**< Stable solved geometry for one run. */
  const Theme* theme_ = nullptr;    /**< Immutable palette for one run. */
  std::shared_ptr<Canvas> canvas_;  /**< Active Canvas transaction. */
  mutable std::mutex status_mutex_; /**< Protects status_ and published_. */
  Status status_                = Status::OK; /**< First run failure. */
  std::size_t remaining_frames_ = 0U; /**< Real frames not yet completed. */
  bool published_ = false; /**< Whether this transaction has been finalized. */
};

/** Stable graph job that renders one frame from the current context. */
class FrameRenderJob final : public multithreading::Job {
 public:
  FrameRenderJob(std::shared_ptr<RenderContext> context,
                 std::size_t frame_index) noexcept
      : context_(std::move(context)), frame_index_(frame_index) {}

  void execute() noexcept override { context_->render(frame_index_); }

 private:
  std::shared_ptr<RenderContext> context_; /**< Reused render inputs. */
  std::size_t frame_index_;                /**< Z-buffer node index. */
};

/** Publication job used only when a layout contains no real frame. */
class PublishJob final : public multithreading::Job {
 public:
  explicit PublishJob(std::shared_ptr<RenderContext> context) noexcept
      : context_(std::move(context)) {}

  void execute() noexcept override { context_->publish(); }

 private:
  std::shared_ptr<RenderContext> context_; /**< Reused render inputs. */
};

/** Convert a generic graph failure into the TUI status domain. */
Status map_graph_status(execution_graph::Status status) noexcept {
  if (execution_graph::is_ok(status)) {
    return Status::OK;
  }
  Logger<ERROR> << "Frame execution graph failed: "
                << execution_graph::status_message(status);
  return status == execution_graph::Status::INVALID_ARGUMENT
             ? Status::INVALID_ARGUMENT
             : Status::ASYNC_DISPATCH_FAILED;
}

}  // namespace

/** Borrowed worker pool, reusable graph topology, and render context. */
class ParallelRenderer::Impl {
 public:
  /** Borrow the worker pool and allocate the reusable render context. */
  explicit Impl(multithreading::JobQueue& configured_workers)
      : workers(configured_workers),
        context(std::make_shared<RenderContext>()) {}

  /** Rebuild the reusable job graph when absolute overlap topology changes. */
  Status prepare_graph(
      std::size_t frame_count,
      const std::vector<Layout::FrameDependency>& dependencies) {
    if (topology.has_value() && topology->frame_count == frame_count &&
        topology->dependencies == dependencies) {
      return Status::OK;
    }

    auto next_graph =
        std::make_unique<execution_graph::ExecutionGraph<std::size_t>>(workers);
    for (std::size_t frame_index = 0U; frame_index < frame_count;
         ++frame_index) {
      const execution_graph::Status status = next_graph->add_node(
          frame_index, std::make_shared<FrameRenderJob>(context, frame_index));
      if (!execution_graph::is_ok(status)) {
        return map_graph_status(status);
      }
    }
    if (frame_count == 0U) {
      const execution_graph::Status graph_status =
          next_graph->add_node(0U, std::make_shared<PublishJob>(context));
      if (!execution_graph::is_ok(graph_status)) {
        return map_graph_status(graph_status);
      }
    }

    for (const Layout::FrameDependency& dependency : dependencies) {
      if (dependency.prerequisite >= frame_count ||
          dependency.dependent >= frame_count ||
          dependency.prerequisite >= dependency.dependent) {
        Logger<ERROR> << "Absolute layout contains an invalid frame dependency";
        return Status::INVALID_ARGUMENT;
      }
      const execution_graph::Status graph_status = next_graph->add_dependency(
          dependency.prerequisite, dependency.dependent);
      if (!execution_graph::is_ok(graph_status)) {
        return map_graph_status(graph_status);
      }
    }

    graph    = std::move(next_graph);
    topology = RenderTopology{
        .frame_count  = frame_count,
        .dependencies = dependencies,
    };
    return Status::OK;
  }

  multithreading::JobQueue& workers; /**< Borrowed caller-owned executor. */
  std::shared_ptr<RenderContext> context; /**< Inputs read by stable jobs. */
  std::unique_ptr<execution_graph::ExecutionGraph<std::size_t>>
      graph;                              /**< Reusable dependency scheduler. */
  std::optional<RenderTopology> topology; /**< Shape represented by graph. */
  mutable std::mutex mutex; /**< Serializes coordinator lifecycle methods. */
  bool active = false;      /**< Whether one graph result is unconsumed. */
};

ParallelRenderer::ParallelRenderer(multithreading::JobQueue& workers)
    : impl_(std::make_unique<Impl>(workers)) {}

ParallelRenderer::~ParallelRenderer() { static_cast<void>(wait()); }

Status ParallelRenderer::start(
    std::shared_ptr<Layout::LayoutDescription> layout_description,
    const Layout::AbsoluteLayout& absolute_layout, const Theme& theme,
    std::shared_ptr<Canvas> canvas, std::optional<Canvas::Cell> base_cell) {
  const std::lock_guard lock(impl_->mutex);
  if (impl_->active) {
    return Status::FRAME_ALREADY_IN_PROGRESS;
  }
  if (!impl_->workers.active() || layout_description == nullptr ||
      canvas == nullptr) {
    Logger<ERROR> << "Cannot start parallel rendering with missing inputs";
    return Status::INVALID_ARGUMENT;
  }

  const std::size_t frame_count = layout_description->z_buffer.frames().size();
  if (absolute_layout.frame_layouts.size() != frame_count) {
    Logger<ERROR> << "Absolute layout does not cover every frame";
    return Status::FRAME_NOT_FOUND;
  }
  for (const ZBuffer::Entry& entry : layout_description->z_buffer.frames()) {
    if (!absolute_layout.frame_layouts.contains(entry.frame_id)) {
      Logger<ERROR> << "Absolute layout omits frame '" << entry.frame_id << "'";
      return Status::FRAME_NOT_FOUND;
    }
  }

  Status status =
      impl_->prepare_graph(frame_count, absolute_layout.frame_dependencies);
  if (!is_ok(status)) {
    return status;
  }

  status = canvas->begin_frame();
  if (!is_ok(status)) {
    return status;
  }
  if (base_cell.has_value()) {
    status = canvas->clear(*base_cell);
    if (!is_ok(status)) {
      static_cast<void>(canvas->cancel_frame());
      return status;
    }
  }

  impl_->context->prepare(std::move(layout_description), absolute_layout, theme,
                          std::move(canvas), frame_count);
  const execution_graph::Status graph_status = impl_->graph->start();
  if (!execution_graph::is_ok(graph_status)) {
    status = map_graph_status(graph_status);
    static_cast<void>(impl_->context->cancel_after_graph_failure());
    impl_->context->reset();
    return status;
  }
  impl_->active = true;
  return Status::OK;
}

Status ParallelRenderer::wait() noexcept {
  const std::lock_guard lock(impl_->mutex);
  if (!impl_->active) {
    return Status::OK;
  }

  const execution_graph::Status graph_status = impl_->graph->wait();
  Status status                              = Status::OK;
  if (execution_graph::is_ok(graph_status)) {
    status = impl_->context->result();
  } else {
    static_cast<void>(map_graph_status(graph_status));
    status = impl_->context->cancel_after_graph_failure();
  }
  impl_->context->reset();
  impl_->active = false;
  return status;
}

bool ParallelRenderer::active() const noexcept {
  const std::lock_guard lock(impl_->mutex);
  return impl_->active;
}

std::size_t ParallelRenderer::worker_count() const noexcept {
  return impl_->workers.worker_count();
}

}  // namespace puc::tui
