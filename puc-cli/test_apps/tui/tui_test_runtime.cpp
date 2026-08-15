/**
 * @file tui_test_runtime.cpp
 * @brief Lifecycle-owned logic for the interactive TUI rendering smoke test.
 *
 * This runtime backs a manual smoke test rather than an automated unit test. It
 * exercises the integration between Screen, Canvas, Layout, ZBuffer, Frame,
 * Theme, TerminalSession, bounded IPC channels, terminal resize events,
 * Unicode rendering, true color, and parallel Canvas publication. AppState
 * owns the worker, channel, terminal, Screen, renderer, and executable-runtime
 * subsystems and orders their lifecycle from declared dependencies. Every ready
 * frame in the layout is rendered as an independent job, and the last real
 * frame to complete publishes the Canvas A/B transaction. Run it from a real
 * terminal with:
 *
 *     bazel run //puc-cli/test_apps:test-app
 *
 * The animation below demonstrates the normal layout, live terminal resizing,
 * and the small-screen fallback:
 *
 * ![The PUC TUI test app responding to terminal
 * resizes.](../assets/puc-cli-tui-test-app-resize-demo.gif)
 *
 * **Expected display**
 *
 * On a terminal large enough for the normal layout, the application should
 * display all of the following using the property-backed default-dark theme:
 *
 * - One error-colored marker touching each of the four screen corners. Each
 *   marker is two character cells wide and one cell tall, which appears
 *   approximately square with typical terminal-font proportions.
 * - One theme-text-colored `+` in the center cell of the screen.
 * - A single-line Unicode box touching the top and right screen edges. The box
 *   has a visual width-to-height ratio of 4:3, accounting for the measured
 * pixel dimensions of a terminal cell; it is not expected to contain four
 * columns for every three rows. The top-right marker is drawn over the box
 * corner to verify Z-ordering.
 * - Three lines inside the box reporting the current terminal width in
 *   characters, height in characters, and sampled frame rate. The frame rate
 *   normally settles near the loop's 60 FPS cap, but its exact value depends on
 *   the terminal and host load.
 *
 * If either terminal dimension is below the minimum required by the layout,
 * the normal display should be cleared and replaced by a centered
 * `Screen too small` message. The message may be clipped when the terminal is
 * narrower than the message itself.
 *
 * **Visual verification**
 *
 * 1. Start with a comfortably large terminal and confirm that all four corner
 *    markers touch their respective pairs of edges, the `+` is centered, and
 *    the metrics box is wider than it is tall.
 * 2. Compare the width and height shown in the box with the terminal's reported
 *    row and column counts.
 * 3. Resize the terminal in both directions. The corner markers must remain
 *    anchored, the `+` must move to the new center, the metrics values must
 *    update, and the box must retain a visually 4:3 shape without leaving stale
 *    borders or text behind.
 * 4. Shrink the terminal until `Screen too small` appears. No corner markers,
 *    center glyph, metrics box, or old frame contents should remain. Enlarge
 *    the terminal and confirm that the normal layout returns.
 * 5. Press Ctrl-C and verify that the alternate screen is left, the cursor and
 *    terminal input mode are restored, and the shell remains usable.
 *
 * @note A terminal window can contain a thin strip of pixels that is smaller
 * than one character cell, especially along its bottom edge. A cell-based TUI
 * cannot draw into that strip; anchors should be judged against the terminal's
 * addressable character grid.
 */

#include "puc-cli/test_apps/tui/tui_test_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "puc-cli/tui/frames/message_frame.hpp"
#include "puc-cli/tui/rendering/canvas.hpp"
#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/rendering/layout.hpp"
#include "puc-cli/tui/rendering/presentation_subsystem.hpp"
#include "puc-cli/tui/rendering/renderer.hpp"
#include "puc-cli/tui/rendering/screen.hpp"
#include "puc-cli/tui/rendering/screen_subsystem.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "themes/theme_subsystem.hpp"
#include "utils/logger/logger.hpp"
#include "utils/timer/deadline.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Test Runtime");
/** @endcond */

namespace {

using puc::tui::Canvas;
using puc::tui::CellDimensions;
using puc::tui::Frame;
using puc::tui::Layout;
using puc::tui::ParallelRenderer;
using puc::tui::Screen;
using puc::tui::Status;
using puc::tui::Theme;

/** Message displayed in place of the layout below its minimum dimensions. */
constexpr std::string_view kScreenTooSmall = "Screen too small";

/** Application-specific metrics observed by the smoke-test frames. */
struct TestAppState {
  mutable std::shared_mutex mutex; /**< Synchronizes metrics snapshots. */
  size_t screen_width      = 0U;   /**< Latest terminal columns. */
  size_t screen_height     = 0U;   /**< Latest terminal rows. */
  double frames_per_second = 0.0;  /**< Latest sampled presentation rate. */
};

/**
 * Construct a fully specified Canvas cell.
 *
 * @param[in] character Unicode scalar value to display.
 * @param[in] foreground Packed `0xRRGGBB` foreground.
 * @param[in] background Packed `0xRRGGBB` background.
 * @return A cell containing the supplied values.
 */
Canvas::Cell make_cell(char32_t character, uint32_t foreground,
                       uint32_t background) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Bordered panel showing live terminal dimensions and sampled frame rate. */
class MetricsFrame final : public Frame {
 public:
  /** Construct a named metrics panel over typed smoke-test state. */
  MetricsFrame(std::string name, std::shared_ptr<TestAppState> app_state)
      : Frame(std::move(name)), app_state_(std::move(app_state)) {}

  Status draw(const Theme& current_theme, Canvas& canvas,
              const Canvas::Rect& rect) override {
    if (rect.width == 0 || rect.height == 0) {
      return Status::OK;
    }

    size_t screen_width      = 0U;
    size_t screen_height     = 0U;
    double frames_per_second = 0.0;
    {
      const std::shared_lock lock(app_state_->mutex);
      screen_width      = app_state_->screen_width;
      screen_height     = app_state_->screen_height;
      frames_per_second = app_state_->frames_per_second;
    }

    const Theme::Colors colors = current_theme.get_colors();
    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height,
        std::vector<Canvas::Cell>(
            rect.width, make_cell(U' ', colors.text, colors.background)));

    draw_border(cells, colors.text, colors.background);
    write_line(cells, 1, "width:  " + format_count(screen_width) + " chars",
               colors.text, colors.background);
    write_line(cells, 2, "height: " + format_count(screen_height) + " chars",
               colors.text, colors.background);
    write_line(cells, 3, format_frame_rate(frames_per_second), colors.text,
               colors.background);
    return canvas.write_cells(rect, cells);
  }

 private:
  /** Typed state shared with the smoke-test application. */
  std::shared_ptr<TestAppState> app_state_;

  /** Format a cell count in the panel's fixed-width numeric column. */
  static std::string format_count(size_t value) {
    std::ostringstream output;
    output << std::setw(4) << value;
    return output.str();
  }

  /** Format the sampled frame rate as a whole-number FPS line. */
  static std::string format_frame_rate(double frames_per_second) {
    std::ostringstream output;
    output << "frame rate: " << std::fixed << std::setprecision(0)
           << std::setw(2) << frames_per_second << " fps";
    return output.str();
  }

  /** Draw a single-line Unicode box around a rectangular cell grid. */
  static void draw_border(std::vector<std::vector<Canvas::Cell>>& cells,
                          uint32_t foreground, uint32_t background) {
    if (cells.size() < 2 || cells.front().size() < 2) {
      return;
    }

    const size_t last_row    = cells.size() - 1;
    const size_t last_column = cells.front().size() - 1;

    for (size_t x = 1; x < last_column; ++x) {
      cells.front()[x] = make_cell(U'─', foreground, background);
      cells.back()[x]  = make_cell(U'─', foreground, background);
    }

    for (size_t y = 1; y < last_row; ++y) {
      cells[y].front() = make_cell(U'│', foreground, background);
      cells[y].back()  = make_cell(U'│', foreground, background);
    }

    cells.front().front() = make_cell(U'┌', foreground, background);
    cells.front().back()  = make_cell(U'┐', foreground, background);
    cells.back().front()  = make_cell(U'└', foreground, background);
    cells.back().back()   = make_cell(U'┘', foreground, background);
  }

  /** Write clipped ASCII text inside the panel's left and right borders. */
  static void write_line(std::vector<std::vector<Canvas::Cell>>& cells,
                         size_t row, std::string_view text, uint32_t foreground,
                         uint32_t background) {
    if (row >= cells.size() || cells[row].size() < 3) {
      return;
    }

    const size_t available = cells[row].size() - 2;
    const size_t count     = std::min(available, text.size());
    for (size_t i = 0; i < count; ++i) {
      cells[row][i + 1] = make_cell(static_cast<unsigned char>(text[i]),
                                    foreground, background);
    }
  }
};

/** Own one running visual smoke-test generation. */
class TuiTestApplication final {
 public:
  /** Borrow the lifecycle-owned presentation mechanisms and semantic theme. */
  TuiTestApplication(Screen& active_screen, ParallelRenderer& active_renderer,
                     Theme& active_theme)
      : screen(&active_screen),
        renderer(&active_renderer),
        theme(&active_theme),
        state(std::make_shared<TestAppState>()) {}

  /** Quiesce any generation still bound when this object is destroyed. */
  ~TuiTestApplication() { static_cast<void>(shutdown()); }

  /** Build the Canvas, frames, and solved layouts for this generation. */
  Status start() { return setup(); }

  /** Quiesce rendering and release all generation-bound objects. */
  Status stop() noexcept { return shutdown(); }

  /** Produce and present one complete application frame. */
  Status draw_frame() { return draw(); }

 private:
  /**
   * Add one fixed `2 x 1` error-colored marker anchored to a screen corner.
   *
   * @param[in] frame_id Unique marker id.
   * @param[in] horizontal LEFT_ANCHOR or RIGHT_ANCHOR.
   * @param[in] vertical TOP_ANCHOR or BOTTOM_ANCHOR.
   * @return Status::OK or a layout construction error.
   */
  Status add_corner(const std::string& frame_id,
                    Layout::ConstraintType horizontal,
                    Layout::ConstraintType vertical) {
    return layout.add_frame(
        layout_description, frame_id,
        std::make_shared<puc::tui::MessageFrame>(frame_id, "  ",
                                                 Theme::ColorTypes::TEXT_ERROR,
                                                 Theme::ColorTypes::TEXT_ERROR),
        {
            Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                              2),
            Layout::make_character_constraint(Layout::ConstraintType::MAX_WIDTH,
                                              2),
            Layout::make_character_constraint(
                Layout::ConstraintType::MIN_HEIGHT, 1),
            Layout::make_character_constraint(
                Layout::ConstraintType::MAX_HEIGHT, 1),
            Layout::make_character_constraint(horizontal, 0),
            Layout::make_character_constraint(vertical, 0),
        });
  }

  /**
   * Construct the complete smoke-test frame hierarchy and constraints.
   *
   * The metrics panel and center glyph are added before corner markers so the
   * markers remain visible when an extremely small terminal causes overlap.
   *
   * @return Status::OK or the first layout construction error.
   */
  Status setup_layout() {
    layout_description = layout.make_layout_description("canvas-test");

    constexpr std::string_view kMetricsFrameId = "metrics";
    Status status                              = layout.add_frame(
        layout_description, std::string{kMetricsFrameId},
        std::make_shared<MetricsFrame>(std::string{kMetricsFrameId}, state),
        {
            Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                              24),
            Layout::make_percentage_constraint(
                Layout::ConstraintType::MAX_WIDTH, 0.40F),
            Layout::make_ratio_constraint(Layout::ConstraintType::ASPECT_RATIO,
                                          4, 3),
            Layout::make_character_constraint(
                Layout::ConstraintType::TOP_ANCHOR, 0),
            Layout::make_character_constraint(
                Layout::ConstraintType::RIGHT_ANCHOR, 0),
        });
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    constexpr std::string_view kCenterFrameId = "center";
    auto center_frame = std::make_shared<puc::tui::MessageFrame>(
        std::string{kCenterFrameId}, "+", Theme::ColorTypes::TEXT,
        Theme::ColorTypes::BACKGROUND);
    status =
        layout.add_frame(layout_description, std::string{kCenterFrameId},
                         std::move(center_frame),
                         {
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::MIN_WIDTH, 1),
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::MAX_WIDTH, 1),
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::MIN_HEIGHT, 1),
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::MAX_HEIGHT, 1),
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::HORIZONTAL_CENTER, 0),
                             Layout::make_character_constraint(
                                 Layout::ConstraintType::VERTICAL_CENTER, 0),
                         });
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    // Corner frames are added last so that the ZBuffer draws them over the
    // metrics box when the top-right regions overlap.
    status = add_corner("top-left", Layout::ConstraintType::LEFT_ANCHOR,
                        Layout::ConstraintType::TOP_ANCHOR);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = add_corner("top-right", Layout::ConstraintType::RIGHT_ANCHOR,
                        Layout::ConstraintType::TOP_ANCHOR);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = add_corner("bottom-left", Layout::ConstraintType::LEFT_ANCHOR,
                        Layout::ConstraintType::BOTTOM_ANCHOR);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = add_corner("bottom-right", Layout::ConstraintType::RIGHT_ANCHOR,
                        Layout::ConstraintType::BOTTOM_ANCHOR);
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    small_layout_description =
        layout.make_layout_description("screen-too-small");
    return layout.add_frame(small_layout_description, "message",
                            std::make_shared<puc::tui::MessageFrame>(
                                "message", std::string{kScreenTooSmall}),
                            {});
  }

  /**
   * Allocate and attach a Canvas matching new terminal dimensions.
   *
   * The existing Canvas is retained if allocation validation or Screen
   * attachment fails.
   *
   * @param[in] width New terminal columns.
   * @param[in] height New terminal rows.
   * @return Status::OK or the Canvas/Screen error.
   */
  Status attach_canvas(size_t width, size_t height) {
    auto next_canvas = std::make_shared<Canvas>(width, height);
    if (!puc::tui::is_ok(next_canvas->get_status())) {
      return next_canvas->get_status();
    }

    const Status status = screen->set_canvas(next_canvas);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    canvas = std::move(next_canvas);
    return Status::OK;
  }

  /** Update the displayed FPS value at most four times per second. */
  void update_frame_rate() {
    ++frames_since_sample;
    const auto now     = std::chrono::steady_clock::now();
    const auto elapsed = now - frame_rate_sample;

    if (elapsed >= std::chrono::milliseconds{250}) {
      const double frames_per_second =
          static_cast<double>(frames_since_sample) /
          std::chrono::duration<double>(elapsed).count();
      {
        const std::unique_lock lock(state->mutex);
        state->frames_per_second = frames_per_second;
      }
      frames_since_sample = 0;
      frame_rate_sample   = now;
    }
  }

  /**
   * Recompute the terminal size required by the normal layout.
   *
   * The message width and one display row are included even if the declarative
   * layout itself would require less space.
   *
   * @return Status::OK or a layout minimum-size error.
   */
  Status update_minimum_screen_dimensions() {
    Status status = layout.compute_minimum_dimensions(
        layout_description, cell_dimensions, minimum_screen_width,
        minimum_screen_height);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    minimum_screen_width =
        std::max(minimum_screen_width, kScreenTooSmall.size());
    minimum_screen_height = std::max(minimum_screen_height, size_t{1});
    Logger<INFO> << "Test app requires at least " << minimum_screen_width << 'x'
                 << minimum_screen_height << " terminal cells with "
                 << cell_dimensions.width << ':' << cell_dimensions.height
                 << " cell dimensions";
    return Status::OK;
  }

  /**
   * Solve frame rectangles for the currently recorded terminal geometry.
   *
   * @return Status::OK or the first layout validation or resolution error.
   */
  Status update_absolute_layout() {
    size_t width  = 0U;
    size_t height = 0U;
    {
      const std::shared_lock lock(state->mutex);
      width  = state->screen_width;
      height = state->screen_height;
    }
    Status status = layout.compute_absolute_layout(
        layout_description, width, height, cell_dimensions, absolute_layout);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    return layout.compute_absolute_layout(small_layout_description, width,
                                          height, cell_dimensions,
                                          small_absolute_layout);
  }

  /**
   * Initialize Canvas, layout, and timing state over borrowed mechanisms.
   *
   * @return Status::OK when the draw loop can begin, otherwise the first setup
   *         error. AppState remains responsible for stopping shared mechanisms.
   */
  Status setup() {
    if (screen == nullptr || renderer == nullptr || theme == nullptr ||
        state == nullptr) {
      return Status::INVALID_ARGUMENT;
    }
    Status status = Status::OK;

    size_t width  = 0;
    size_t height = 0;
    puc::timer::Deadline<> geometry_timeout;
    geometry_timeout.arm(std::chrono::seconds{2});
    while (true) {
      status = screen->get_dimensions(width, height, cell_dimensions);
      if (puc::tui::is_ok(status)) {
        break;
      }
      if (status != Status::TERMINAL_QUERY_FAILED || geometry_timeout.due()) {
        return status;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    {
      const std::unique_lock lock(state->mutex);
      state->screen_width  = width;
      state->screen_height = height;
    }
    status = attach_canvas(width, height);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = setup_layout();
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = update_minimum_screen_dimensions();
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = update_absolute_layout();
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    frame_rate_sample = std::chrono::steady_clock::now();
    return Status::OK;
  }

  /** Quiesce app-owned render state before Screen and workers stop. */
  Status shutdown() noexcept {
    Status status = renderer == nullptr ? Status::OK : renderer->wait();
    canvas.reset();
    layout_description.reset();
    small_layout_description.reset();
    absolute_layout          = {};
    small_absolute_layout    = {};
    frames_since_sample      = 0U;
    showing_screen_too_small = false;
    screen                   = nullptr;
    renderer                 = nullptr;
    theme                    = nullptr;
    return status;
  }

  /**
   * Produce and present one complete application frame.
   *
   * Terminal dimensions and cell proportions are refreshed first. Resize
   * creates a matching Canvas, a cell-proportion change recomputes minimum
   * dimensions, and the selected normal or small-screen composition is then
   * published and presented. A short delay caps the loop near 60 frames per
   * second.
   *
   * @return Status::OK or the first query, layout, Canvas, or Screen error.
   */
  Status draw() {
    size_t width  = 0;
    size_t height = 0;
    CellDimensions current_cell_dimensions;
    Status status =
        screen->get_dimensions(width, height, current_cell_dimensions);
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    size_t previous_width  = 0U;
    size_t previous_height = 0U;
    {
      const std::shared_lock lock(state->mutex);
      previous_width  = state->screen_width;
      previous_height = state->screen_height;
    }
    const bool screen_dimensions_changed =
        width != previous_width || height != previous_height;
    const bool cell_dimensions_changed =
        current_cell_dimensions != cell_dimensions;
    if (screen_dimensions_changed || cell_dimensions_changed) {
      {
        const std::unique_lock lock(state->mutex);
        state->screen_width  = width;
        state->screen_height = height;
      }
      cell_dimensions = current_cell_dimensions;
      if (screen_dimensions_changed) {
        status = attach_canvas(width, height);
        if (!puc::tui::is_ok(status)) {
          return status;
        }
      }
      status = update_absolute_layout();
      if (!puc::tui::is_ok(status)) {
        return status;
      }
      if (cell_dimensions_changed) {
        status = update_minimum_screen_dimensions();
        if (!puc::tui::is_ok(status)) {
          return status;
        }
      }
    }

    update_frame_rate();
    const bool screen_too_small =
        width < minimum_screen_width || height < minimum_screen_height;
    if (screen_too_small != showing_screen_too_small) {
      Logger<INFO> << (screen_too_small ? "Showing small-screen message"
                                        : "Restoring test layout");
      showing_screen_too_small = screen_too_small;
    }

    const Theme::Colors colors = theme->get_colors();
    status                     = renderer->start(
        screen_too_small ? small_layout_description : layout_description,
        screen_too_small ? small_absolute_layout : absolute_layout, *theme,
        canvas, make_cell(U' ', colors.text, colors.background));
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    status = renderer->wait();
    if (!puc::tui::is_ok(status)) {
      return status;
    }

    status = screen->draw();
    if (!puc::tui::is_ok(status)) {
      return status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{16});
    return Status::OK;
  }

  Screen* screen;             /**< Borrowed running Screen generation. */
  ParallelRenderer* renderer; /**< Borrowed running renderer generation. */
  Theme* theme;               /**< Borrowed property-backed semantic palette. */
  std::shared_ptr<Canvas> canvas; /**< Current screen-sized Canvas. */
  std::shared_ptr<Layout::LayoutDescription>
      layout_description; /**< Normal smoke-test layout. */
  std::shared_ptr<Layout::LayoutDescription>
      small_layout_description;           /**< Small-screen fallback layout. */
  Layout::AbsoluteLayout absolute_layout; /**< Solved normal rectangles. */
  Layout::AbsoluteLayout
      small_absolute_layout;           /**< Solved fallback rectangles. */
  Layout layout;                       /**< Constraint solver and compositor. */
  std::shared_ptr<TestAppState> state; /**< Metrics consumed by frames. */
  CellDimensions cell_dimensions =
      puc::tui::kDefaultCellDimensions; /**< Physical terminal cell proportions.
                                         */
  std::chrono::steady_clock::time_point
      frame_rate_sample; /**< Start of the FPS sampling interval. */
  size_t frames_since_sample    = 0U; /**< Frames in the current FPS sample. */
  size_t minimum_screen_width   = 0U; /**< Normal-layout width floor. */
  size_t minimum_screen_height  = 0U; /**< Normal-layout height floor. */
  bool showing_screen_too_small = false; /**< Logged fallback state. */
};

}  // namespace

namespace puc::app {

/** Hidden ownership for one optional running smoke-test generation. */
class TuiTestRuntimeSubsystem::Impl final {
 public:
  std::unique_ptr<TuiTestApplication>
      application; /**< Current app-specific running generation. */
};

TuiTestRuntimeSubsystem::TuiTestRuntimeSubsystem()
    : AppSubsystem(
          "tui-test-runtime",
          subsystem_dependencies<ScreenSubsystem, PresentationSubsystem,
                                 ThemeSubsystem>()),
      impl_(std::make_unique<Impl>()) {}

TuiTestRuntimeSubsystem::~TuiTestRuntimeSubsystem() = default;

puc::app::Status TuiTestRuntimeSubsystem::initialize(AppState& app) {
  static_cast<void>(app);
  return impl_ == nullptr ? puc::app::Status::SUBSYSTEM_FAILURE
                          : puc::app::Status::OK;
}

puc::app::Status TuiTestRuntimeSubsystem::start(AppState& app) {
  auto* screen       = app.get_subsystem<ScreenSubsystem>();
  auto* presentation = app.get_subsystem<PresentationSubsystem>();
  auto* theme        = app.get_subsystem<ThemeSubsystem>();
  if (impl_ == nullptr || screen == nullptr || screen->screen() == nullptr ||
      presentation == nullptr || presentation->renderer() == nullptr ||
      theme == nullptr || theme->theme() == nullptr) {
    return puc::app::Status::SUBSYSTEM_FAILURE;
  }

  auto application = std::make_unique<TuiTestApplication>(
      *screen->screen(), *presentation->renderer(), *theme->theme());
  const tui::Status setup_status = application->start();
  if (!tui::is_ok(setup_status)) {
    Logger<ERROR> << "Could not set up TUI test runtime: "
                  << tui::status_message(setup_status);
    static_cast<void>(application->stop());
    return puc::app::Status::SUBSYSTEM_FAILURE;
  }
  impl_->application = std::move(application);
  return puc::app::Status::OK;
}

puc::app::Status TuiTestRuntimeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  if (impl_ == nullptr || impl_->application == nullptr) {
    return puc::app::Status::OK;
  }
  const tui::Status status = impl_->application->stop();
  impl_->application.reset();
  return tui::is_ok(status) ? puc::app::Status::OK
                            : puc::app::Status::SUBSYSTEM_FAILURE;
}

puc::app::Status TuiTestRuntimeSubsystem::terminate(AppState& app) noexcept {
  const puc::app::Status status = stop(app);
  impl_.reset();
  return status;
}

bool TuiTestRuntimeSubsystem::draw() {
  if (impl_ == nullptr || impl_->application == nullptr) {
    return false;
  }
  const tui::Status status = impl_->application->draw_frame();
  if (!tui::is_ok(status)) {
    Logger<ERROR> << "TUI test draw failed: " << tui::status_message(status);
    return false;
  }
  return true;
}

}  // namespace puc::app
