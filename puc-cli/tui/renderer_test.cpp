/**
 * @file renderer_test.cpp
 * @brief Unit tests for dependency-aware parallel frame rendering.
 */

#include "puc-cli/tui/renderer.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/tui/frame.hpp"
#include "utils/multithreading/job_queue.hpp"

namespace puc::tui {
namespace {

using namespace std::chrono_literals;

/** Condition-variable gate used to hold frame jobs at known points. */
class Gate {
 public:
  /** Block until open() is called. */
  void wait() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return open_; });
  }

  /** Release every current and future waiter. */
  void open() {
    {
      const std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;                /**< Protects open_. */
  std::condition_variable changed_; /**< Wakes waiters. */
  bool open_ = false;               /**< Whether waiters may proceed. */
};

/** Synchronized observation count with a bounded wait. */
class Counter {
 public:
  /** Record one observation and wake waiters. */
  void increment() {
    {
      const std::lock_guard lock(mutex_);
      ++value_;
    }
    changed_.notify_all();
  }

  /** Wait for at least expected observations. */
  bool wait_for(std::size_t expected, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this, expected] { return value_ >= expected; });
  }

  /** Return the current observation count. */
  std::size_t value() const {
    const std::lock_guard lock(mutex_);
    return value_;
  }

 private:
  mutable std::mutex mutex_;        /**< Protects value_. */
  std::condition_variable changed_; /**< Wakes observers. */
  std::size_t value_ = 0U;          /**< Recorded observations. */
};

/** Frame double with caller-defined no-throw draw behavior. */
class CallbackFrame final : public Frame {
 public:
  using Callback = std::function<Status(Canvas&, const Canvas::Rect&)>;

  CallbackFrame(std::string name, Callback callback)
      : Frame(std::move(name)), callback_(std::move(callback)) {}

  Status draw(const Theme&, Canvas& canvas, const Canvas::Rect& rect) override {
    return callback_(canvas, rect);
  }

 private:
  Callback callback_; /**< Behavior invoked by draw(). */
};

/** Fill one frame rectangle with a distinctive character. */
Status fill(Canvas& canvas, const Canvas::Rect& rect, char32_t character) {
  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height, std::vector<Canvas::Cell>(
                       rect.width, Canvas::Cell{.character = character}));
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
}

/** Add a frame to a fresh description and assert successful construction. */
void add(Layout& layout,
         const std::shared_ptr<Layout::LayoutDescription>& description,
         std::string id, std::shared_ptr<Frame> frame) {
  ASSERT_EQ(layout.add_frame_to_layout_description(description, std::move(id),
                                                   std::move(frame)),
            Status::OK);
}

TEST(ParallelRendererTest, RunsDisjointFramesTogetherAndPublishesAfterBoth) {
  multithreading::JobQueue workers(4U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("disjoint");
  Gate gate;
  Counter started;

  add(layout, description, "left",
      std::make_shared<CallbackFrame>(
          "left", [&](Canvas& canvas, const Canvas::Rect& rect) {
            started.increment();
            gate.wait();
            return fill(canvas, rect, U'L');
          }));
  add(layout, description, "right",
      std::make_shared<CallbackFrame>(
          "right", [&](Canvas& canvas, const Canvas::Rect& rect) {
            started.increment();
            gate.wait();
            return fill(canvas, rect, U'R');
          }));

  const Layout::AbsoluteLayout absolute{
      .frame_layouts =
          {
              {"left", {.x = 0U, .y = 0U, .width = 1U, .height = 1U}},
              {"right", {.x = 1U, .y = 0U, .width = 1U, .height = 1U}},
          },
      .frame_dependencies = {},
  };
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(2U, 1U);

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas,
                           Canvas::Cell{.character = U'.'}),
            Status::OK);
  ASSERT_TRUE(started.wait_for(2U));
  EXPECT_TRUE(renderer.active());
  EXPECT_EQ(canvas->get_drawable_buffer()[0].character, U' ');
  EXPECT_EQ(canvas->get_drawable_buffer()[1].character, U' ');

  gate.open();
  EXPECT_EQ(renderer.wait(), Status::OK);
  EXPECT_FALSE(renderer.active());
  EXPECT_EQ(canvas->get_drawable_buffer()[0].character, U'L');
  EXPECT_EQ(canvas->get_drawable_buffer()[1].character, U'R');
}

TEST(ParallelRendererTest, NeverStopsItsBorrowedWorkerPool) {
  multithreading::JobQueue workers(2U);
  {
    ParallelRenderer renderer(workers);
    EXPECT_EQ(renderer.worker_count(), 2U);
  }
  EXPECT_TRUE(workers.active());
}

TEST(ParallelRendererTest, HoldsAnOverlappingFrontFrameUntilBackCompletes) {
  multithreading::JobQueue workers(4U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("overlap");
  Gate back_gate;
  Counter back_started;
  Counter front_started;

  add(layout, description, "back",
      std::make_shared<CallbackFrame>(
          "back", [&](Canvas& canvas, const Canvas::Rect& rect) {
            back_started.increment();
            back_gate.wait();
            return fill(canvas, rect, U'B');
          }));
  add(layout, description, "front",
      std::make_shared<CallbackFrame>(
          "front", [&](Canvas& canvas, const Canvas::Rect& rect) {
            front_started.increment();
            return fill(canvas, rect, U'F');
          }));

  const Layout::AbsoluteLayout absolute{
      .frame_layouts =
          {
              {"back", {.x = 0U, .y = 0U, .width = 1U, .height = 1U}},
              {"front", {.x = 0U, .y = 0U, .width = 1U, .height = 1U}},
          },
      .frame_dependencies = {{.prerequisite = 0U, .dependent = 1U}},
  };
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas), Status::OK);
  ASSERT_TRUE(back_started.wait_for(1U));
  EXPECT_EQ(front_started.value(), 0U);

  back_gate.open();
  EXPECT_EQ(renderer.wait(), Status::OK);
  EXPECT_EQ(front_started.value(), 1U);
  EXPECT_EQ(canvas->get_drawable_buffer().front().character, U'F');
}

TEST(ParallelRendererTest, CancelsPublicationWhenAFrameFails) {
  multithreading::JobQueue workers(2U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("failure");
  add(layout, description, "broken",
      std::make_shared<CallbackFrame>("broken",
                                      [](Canvas&, const Canvas::Rect&) {
                                        return Status::TERMINAL_WRITE_FAILED;
                                      }));

  const Layout::AbsoluteLayout absolute{
      .frame_layouts      = {{"broken",
                              {.x = 0U, .y = 0U, .width = 1U, .height = 1U}}},
      .frame_dependencies = {},
  };
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);
  ASSERT_EQ(canvas->begin_frame(), Status::OK);
  ASSERT_EQ(canvas->clear(Canvas::Cell{.character = U'P'}), Status::OK);
  ASSERT_EQ(canvas->end_frame(), Status::OK);

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas,
                           Canvas::Cell{.character = U'X'}),
            Status::OK);
  EXPECT_EQ(renderer.wait(), Status::TERMINAL_WRITE_FAILED);
  EXPECT_EQ(canvas->get_drawable_buffer().front().character, U'P');
}

TEST(ParallelRendererTest, ReusesOneTopologyAcrossRenderGenerations) {
  multithreading::JobQueue workers(2U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("reused");
  std::atomic<std::size_t> draws{0U};
  add(layout, description, "frame",
      std::make_shared<CallbackFrame>(
          "frame", [&](Canvas& canvas, const Canvas::Rect& rect) {
            draws.fetch_add(1U);
            return fill(canvas, rect, U'X');
          }));
  const Layout::AbsoluteLayout absolute{
      .frame_layouts      = {{"frame",
                              {.x = 0U, .y = 0U, .width = 1U, .height = 1U}}},
      .frame_dependencies = {},
  };
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas), Status::OK);
  ASSERT_EQ(renderer.wait(), Status::OK);
  ASSERT_EQ(renderer.start(description, absolute, theme, canvas), Status::OK);
  ASSERT_EQ(renderer.wait(), Status::OK);
  EXPECT_EQ(draws.load(), 2U);
}

TEST(ParallelRendererTest, RejectsASecondRunUntilWaitConsumesTheFirst) {
  multithreading::JobQueue workers(2U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("active");
  Gate gate;
  Counter started;
  add(layout, description, "frame",
      std::make_shared<CallbackFrame>("frame",
                                      [&](Canvas&, const Canvas::Rect&) {
                                        started.increment();
                                        gate.wait();
                                        return Status::OK;
                                      }));
  const Layout::AbsoluteLayout absolute{
      .frame_layouts      = {{"frame",
                              {.x = 0U, .y = 0U, .width = 1U, .height = 1U}}},
      .frame_dependencies = {},
  };
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas), Status::OK);
  ASSERT_TRUE(started.wait_for(1U));
  EXPECT_EQ(renderer.start(description, absolute, theme, canvas),
            Status::FRAME_ALREADY_IN_PROGRESS);
  gate.open();
  EXPECT_EQ(renderer.wait(), Status::OK);
}

TEST(ParallelRendererTest, ValidatesInputsAndExecutionDependencies) {
  multithreading::JobQueue workers(2U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("invalid");
  add(layout, description, "frame",
      std::make_shared<CallbackFrame>(
          "frame", [](Canvas&, const Canvas::Rect&) { return Status::OK; }));
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);
  const Layout::AbsoluteLayout valid{
      .frame_layouts      = {{"frame",
                              {.x = 0U, .y = 0U, .width = 1U, .height = 1U}}},
      .frame_dependencies = {},
  };

  EXPECT_EQ(renderer.worker_count(), 2U);
  EXPECT_EQ(renderer.start(nullptr, valid, theme, canvas),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(
      renderer.start(description, Layout::AbsoluteLayout{}, theme, canvas),
      Status::FRAME_NOT_FOUND);

  Layout::AbsoluteLayout invalid_dependency = valid;
  invalid_dependency.frame_dependencies.push_back(
      {.prerequisite = 0U, .dependent = 0U});
  EXPECT_EQ(renderer.start(description, invalid_dependency, theme, canvas),
            Status::INVALID_ARGUMENT);

  multithreading::JobQueue stopped_workers;
  stopped_workers.shutdown();
  ParallelRenderer stopped_renderer(stopped_workers);
  EXPECT_EQ(stopped_renderer.start(description, valid, theme, canvas),
            Status::INVALID_ARGUMENT);
}

TEST(ParallelRendererTest, PublishesAnEmptyLayoutThroughTheGraph) {
  multithreading::JobQueue workers(1U);
  ParallelRenderer renderer(workers);
  Layout layout;
  const auto description = layout.make_layout_description("empty");
  Theme theme;
  const auto canvas = std::make_shared<Canvas>(1U, 1U);
  const Layout::AbsoluteLayout absolute;

  ASSERT_EQ(renderer.start(description, absolute, theme, canvas,
                           Canvas::Cell{.character = U'E'}),
            Status::OK);
  EXPECT_EQ(renderer.wait(), Status::OK);
  EXPECT_EQ(canvas->get_drawable_buffer().front().character, U'E');
}

}  // namespace
}  // namespace puc::tui
