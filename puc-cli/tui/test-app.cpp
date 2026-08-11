/**
 * @file test-app.cpp
 * @brief Interactive visual integration test for the complete TUI rendering
 *        pipeline.
 *
 * This executable is a manual smoke test rather than an automated unit test. It
 * exercises the integration between Screen, Canvas, Layout, ZBuffer, Frame,
 * Theme, TerminalSession, bounded IPC channels, terminal resize events,
 * Unicode rendering, true color, and parallel Canvas publication. AppState
 * owns the worker, channel, terminal, Screen, renderer, and executable-runtime
 * subsystems and orders their lifecycle from declared dependencies. Every ready
 * frame in the layout is rendered as an independent job, and the last real
 * frame to complete publishes the Canvas A/B transaction. Run it from a real
 * terminal with:
 *
 *     bazel run //puc-cli/tui:test-app
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
 * display all of the following on a black background:
 *
 * - One red marker touching each of the four screen corners. Each marker is two
 *   character cells wide and one cell tall, which appears approximately square
 *   with the proportions used by typical terminal fonts.
 * - One white `+` in the center cell of the screen.
 * - A single-line Unicode box touching the top and right screen edges. The box
 *   has a visual width-to-height ratio of 4:3, accounting for the measured
 * pixel dimensions of a terminal cell; it is not expected to contain four
 * columns for every three rows. The top-right red marker is drawn over the box
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
 * 1. Start with a comfortably large terminal and confirm that all four red
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

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iomanip>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "puc-cli/state/bootstrap.hpp"
#include "puc-cli/state/presentation.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/layout.hpp"
#include "puc-cli/tui/renderer.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/status.hpp"
#include "puc-cli/tui/theme.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Test App");
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

/** RGB color used by the four corner markers. */
constexpr uint32_t kRed = 0xff0000;
/** Default foreground used by the smoke-test frames. */
constexpr uint32_t kWhite = 0xffffff;
/** Default background used by the smoke-test frames. */
constexpr uint32_t kBlack = 0x000000;
/** Message displayed in place of the layout below its minimum dimensions. */
constexpr std::string_view kScreenTooSmall = "Screen too small";
/** Shared worker budget for frame rendering and terminal message delivery. */
constexpr std::uint8_t kWorkerCount = 4U;

/** Screen borrowed from ScreenSubsystem during a running generation. */
Screen* screen = nullptr;
/** Parallel frame coordinator borrowed from PresentationSubsystem. */
ParallelRenderer* renderer = nullptr;
/** Canvas matching the current terminal cell dimensions. */
std::shared_ptr<Canvas> canvas;
/** Declarative description of every smoke-test frame. */
std::shared_ptr<Layout::LayoutDescription> layout_description;
/** One-frame fallback layout used below the normal minimum dimensions. */
std::shared_ptr<Layout::LayoutDescription> small_layout_description;
/** Frame rectangles solved for the current screen geometry. */
Layout::AbsoluteLayout absolute_layout;
/** Solved rectangle for the centered small-screen message. */
Layout::AbsoluteLayout small_absolute_layout;
/** Constraint solver and compositor. */
Layout layout;
/** Application-specific metrics observed by the smoke-test frames. */
struct TestAppState {
  mutable std::shared_mutex mutex; /**< Synchronizes metrics snapshots. */
  size_t screen_width      = 0U;   /**< Latest terminal columns. */
  size_t screen_height     = 0U;   /**< Latest terminal rows. */
  double frames_per_second = 0.0;  /**< Latest sampled presentation rate. */
};

/** Typed application state captured only by frames that consume it. */
std::shared_ptr<TestAppState> state;
/** Physical proportions reported for the current terminal cells. */
CellDimensions cell_dimensions = puc::tui::kDefaultCellDimensions;
/** Semantic palette used by smoke-test frames. */
Theme theme;

/** Start of the current frame-rate sampling interval. */
std::chrono::steady_clock::time_point frame_rate_sample;
/** Frames drawn since `frame_rate_sample`. */
size_t frames_since_sample = 0;
/** Smallest terminal width that can display the normal layout. */
size_t minimum_screen_width = 0;
/** Smallest terminal height that can display the normal layout. */
size_t minimum_screen_height = 0;
/** Async-signal-safe flag set by the termination handler. */
volatile std::sig_atomic_t stop_requested = 0;
/** Tracks small-screen mode so transitions can be logged once. */
bool showing_screen_too_small = false;

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

/**
 * Adapt a vector-of-vectors grid to Canvas's row-span write interface.
 *
 * @param[in,out] canvas Active Canvas transaction receiving the cells.
 * @param[in] rect Destination rectangle.
 * @param[in,out] cells Row-major cell grid matching `rect`.
 * @return The result of Canvas::write_cells().
 */
Status write_grid(Canvas& canvas, const Canvas::Rect& rect,
                  std::vector<std::vector<Canvas::Cell>>& cells) {
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());

  for (auto& row : cells) {
    rows.emplace_back(row);
  }

  const std::span<std::span<Canvas::Cell>> cell_span{rows};
  return canvas.write_cells(rect, cell_span);
}

/** Frame that fills its assigned rectangle with one background color. */
class SolidFrame final : public Frame {
 public:
  /**
   * Construct a solid-color frame.
   *
   * @param[in] name Frame name.
   * @param[in] color Packed RGB value used for foreground and background.
   */
  SolidFrame(std::string name, uint32_t color)
      : Frame(std::move(name)), color_(color) {}

  Status draw(const Theme&, Canvas& canvas, const Canvas::Rect& rect) override {
    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height,
        std::vector<Canvas::Cell>(rect.width, make_cell(U' ', color_, color_)));
    return write_grid(canvas, rect, cells);
  }

 private:
  /** Packed fill color. */
  uint32_t color_;
};

/** Frame that places one glyph at the center of its assigned rectangle. */
class GlyphFrame final : public Frame {
 public:
  /**
   * Construct a centered-glyph frame.
   *
   * @param[in] name Frame name.
   * @param[in] glyph Unicode scalar value placed at the center cell.
   * @param[in] foreground Packed glyph foreground color.
   * @param[in] background Packed rectangle background color.
   */
  GlyphFrame(std::string name, char32_t glyph, uint32_t foreground,
             uint32_t background)
      : Frame(std::move(name)),
        glyph_(glyph),
        foreground_(foreground),
        background_(background) {}

  Status draw(const Theme&, Canvas& canvas, const Canvas::Rect& rect) override {
    if (rect.width == 0 || rect.height == 0) {
      return Status::OK;
    }

    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height,
        std::vector<Canvas::Cell>(rect.width,
                                  make_cell(U' ', foreground_, background_)));
    cells[rect.height / 2][rect.width / 2] =
        make_cell(glyph_, foreground_, background_);
    return write_grid(canvas, rect, cells);
  }

 private:
  /** Glyph placed at the center cell. */
  char32_t glyph_;
  /** Packed glyph foreground color. */
  uint32_t foreground_;
  /** Packed rectangle background color. */
  uint32_t background_;
};

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
    return write_grid(canvas, rect, cells);
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

/** Frame that centers a clipped warning inside its assigned rectangle. */
class MessageFrame final : public Frame {
 public:
  /** Construct a named frame containing one immutable ASCII message. */
  MessageFrame(std::string name, std::string message)
      : Frame(std::move(name)), message_(std::move(message)) {}

  Status draw(const Theme& current_theme, Canvas& canvas,
              const Canvas::Rect& rect) override {
    if (rect.width == 0U || rect.height == 0U) {
      return Status::OK;
    }

    const Theme::Colors colors   = current_theme.get_colors();
    const size_t character_count = std::min(rect.width, message_.size());
    std::vector<std::vector<Canvas::Cell>> cells(
        1U, std::vector<Canvas::Cell>(
                character_count,
                make_cell(U' ', colors.text_warning, colors.background)));
    for (size_t index = 0U; index < character_count; ++index) {
      cells.front()[index] =
          make_cell(static_cast<unsigned char>(message_[index]),
                    colors.text_warning, colors.background);
    }

    return write_grid(canvas,
                      Canvas::Rect{
                          .x     = rect.x + (rect.width - character_count) / 2U,
                          .y     = rect.y + rect.height / 2U,
                          .width = character_count,
                          .height = 1U,
                      },
                      cells);
  }

 private:
  std::string message_; /**< Text centered and clipped by draw(). */
};

/**
 * Add a frame and all of its constraints to one test layout.
 *
 * The frame is retained if a later constraint fails, which is acceptable
 * during one-shot setup because the error aborts application initialization.
 *
 * @param[in] description Layout description receiving the frame.
 * @param[in] frame_id Unique layout id.
 * @param[in] frame Frame implementation for which ownership is shared.
 * @param[in] constraints Constraints applied in the supplied order.
 * @return Status::OK or the first frame/constraint insertion error.
 */
Status add_frame(const std::shared_ptr<Layout::LayoutDescription>& description,
                 std::string frame_id, std::shared_ptr<Frame> frame,
                 std::initializer_list<Layout::Constraint> constraints) {
  Status status = layout.add_frame_to_layout_description(description, frame_id,
                                                         std::move(frame));
  if (!puc::tui::is_ok(status)) {
    return status;
  }

  for (const Layout::Constraint& constraint : constraints) {
    status = layout.add_constraint_to_frame(description, frame_id, constraint);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
}

/**
 * Add one fixed `2 x 1` red marker anchored to a screen corner.
 *
 * @param[in] frame_id Unique marker id.
 * @param[in] horizontal LEFT_ANCHOR or RIGHT_ANCHOR.
 * @param[in] vertical TOP_ANCHOR or BOTTOM_ANCHOR.
 * @return Status::OK or a layout construction error.
 */
Status add_corner(const std::string& frame_id,
                  Layout::ConstraintType horizontal,
                  Layout::ConstraintType vertical) {
  return add_frame(layout_description, frame_id,
                   std::make_shared<SolidFrame>(frame_id, kRed),
                   {
                       Layout::make_character_constraint(
                           Layout::ConstraintType::MIN_WIDTH, 2),
                       Layout::make_character_constraint(
                           Layout::ConstraintType::MAX_WIDTH, 2),
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
  Status status                              = add_frame(
      layout_description, std::string{kMetricsFrameId},
      std::make_shared<MetricsFrame>(std::string{kMetricsFrameId}, state),
      {
          Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                                                         24),
          Layout::make_percentage_constraint(Layout::ConstraintType::MAX_WIDTH,
                                                                          0.40F),
          Layout::make_ratio_constraint(Layout::ConstraintType::ASPECT_RATIO, 4,
                                                                     3),
          Layout::make_character_constraint(Layout::ConstraintType::TOP_ANCHOR,
                                                                         0),
          Layout::make_character_constraint(
              Layout::ConstraintType::RIGHT_ANCHOR, 0),
      });
  if (!puc::tui::is_ok(status)) {
    return status;
  }

  constexpr std::string_view kCenterFrameId = "center";
  status = add_frame(layout_description, std::string{kCenterFrameId},
                     std::make_shared<GlyphFrame>(std::string{kCenterFrameId},
                                                  U'+', kWhite, kBlack),
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

  small_layout_description = layout.make_layout_description("screen-too-small");
  return add_frame(
      small_layout_description, "message",
      std::make_shared<MessageFrame>("message", std::string{kScreenTooSmall}),
      {});
}

/**
 * Allocate and attach a Canvas matching new terminal dimensions.
 *
 * The existing global canvas is retained if allocation validation or Screen
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
 * Request shutdown from a process signal without performing unsafe work.
 *
 * @param[in] signal_number Ignored signal number supplied by `std::signal`.
 */
void request_stop(int signal_number) {
  static_cast<void>(signal_number);
  stop_requested = 1;
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
  minimum_screen_width = std::max(minimum_screen_width, kScreenTooSmall.size());
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
  return layout.compute_absolute_layout(small_layout_description, width, height,
                                        cell_dimensions, small_absolute_layout);
}

/**
 * Initialize terminal ownership, palette, Canvas, layout, and timing state.
 *
 * @param[in,out] active_screen Screen for the current running generation.
 * @param[in,out] active_renderer Renderer borrowing the current worker pool.
 * @return Status::OK when the draw loop can begin, otherwise the first setup
 *         error. AppState remains responsible for stopping shared mechanisms.
 */
Status setup(Screen& active_screen, ParallelRenderer& active_renderer) {
  screen        = &active_screen;
  renderer      = &active_renderer;
  Status status = Status::OK;
  Theme::Colors colors{};
  colors.primary              = kRed;
  colors.highlight_background = 0x264f78U;
  colors.highlight_text       = kWhite;
  colors.text                 = kWhite;
  colors.text_warning         = kWhite;
  colors.background           = kBlack;
  theme.load_colors(colors);

  size_t width  = 0;
  size_t height = 0;
  const auto geometry_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{2};
  while (true) {
    status = screen->get_dimensions(width, height, cell_dimensions);
    if (puc::tui::is_ok(status)) {
      break;
    }
    if (status != Status::TERMINAL_QUERY_FAILED ||
        std::chrono::steady_clock::now() >= geometry_deadline) {
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
Status stop_runtime() noexcept {
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
  return status;
}

/**
 * Produce and present one complete application frame.
 *
 * Terminal dimensions and cell proportions are refreshed first. Resize creates
 * a matching Canvas, a cell-proportion change recomputes minimum dimensions,
 * and the selected normal or small-screen composition is then published and
 * presented. A short delay caps the loop near 60 frames per second.
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

  const Theme::Colors colors = theme.get_colors();
  status                     = renderer->start(
      screen_too_small ? small_layout_description : layout_description,
      screen_too_small ? small_absolute_layout : absolute_layout, theme, canvas,
      make_cell(U' ', colors.text, colors.background));
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

/** Lifecycle boundary for this executable's visual smoke-test state. */
class TuiTestRuntimeSubsystem final : public puc::app::AppSubsystem {
 public:
  /** Run only after the shared Screen and renderer generation are available. */
  TuiTestRuntimeSubsystem()
      : AppSubsystem(
            "tui-test-runtime",
            puc::app::subsystem_dependencies<
                puc::app::ScreenSubsystem, puc::app::PresentationSubsystem>()) {
  }

  puc::app::Status initialize(puc::app::AppState& app) override {
    static_cast<void>(app);
    state = std::make_shared<TestAppState>();
    return puc::app::Status::OK;
  }

  puc::app::Status start(puc::app::AppState& app) override {
    auto* screen_subsystem = app.get_subsystem<puc::app::ScreenSubsystem>();
    auto* presentation = app.get_subsystem<puc::app::PresentationSubsystem>();
    if (screen_subsystem == nullptr || screen_subsystem->screen() == nullptr ||
        presentation == nullptr || presentation->renderer() == nullptr) {
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    const Status status =
        setup(*screen_subsystem->screen(), *presentation->renderer());
    if (!puc::tui::is_ok(status)) {
      static_cast<void>(stop_runtime());
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    return puc::app::Status::OK;
  }

  puc::app::Status stop(puc::app::AppState& app) noexcept override {
    static_cast<void>(app);
    return puc::tui::is_ok(stop_runtime())
               ? puc::app::Status::OK
               : puc::app::Status::SUBSYSTEM_FAILURE;
  }

  puc::app::Status terminate(puc::app::AppState& app) noexcept override {
    const puc::app::Status status = stop(app);
    state.reset();
    return status;
  }

  Status draw_frame() { return draw(); }
};

}  // namespace

/**
 * Run the interactive TUI smoke test until signaled or an operation fails.
 *
 * @return Zero after successful setup, drawing, and terminal restoration;
 *         otherwise one.
 */
int main() {
  const puc::logger::LoggerConf logger_config{
      .global_level = puc::logger::LogLevel::WARN,
  };

  if (std::signal(SIGINT, request_stop) == SIG_ERR ||
      std::signal(SIGTERM, request_stop) == SIG_ERR) {
    std::fprintf(stderr, "Could not install termination signal handlers\n");
    return 1;
  }

  puc::app::ApplicationSubsystemOptions options{
      .logger       = logger_config,
      .worker_count = kWorkerCount,
      .screen =
          puc::app::ScreenSubsystemOptions{
              .take_terminal = true,
          },
      .selection =
          puc::app::ApplicationSubsystemSelection{
              .metronome         = false,
              .presentation      = true,
              .commands          = false,
              .input             = false,
              .command_mode      = false,
              .embedded_terminal = false,
          },
  };
  puc::app::AppState app;
  puc::app::Status app_status =
      puc::app::register_application_subsystems(app, std::move(options));
  auto runtime = std::make_unique<TuiTestRuntimeSubsystem>();
  TuiTestRuntimeSubsystem* runtime_view = runtime.get();
  if (puc::app::is_ok(app_status)) {
    app_status = app.register_subsystem(std::move(runtime));
  }
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not register TUI test subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    return 1;
  }
  app_status = app.initialize(puc::app::OperatingMode::TUI);
  if (puc::app::is_ok(app_status)) {
    app_status = app.start();
  }
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not start TUI test subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    static_cast<void>(app.terminate());
    return 1;
  }

  Status status = Status::OK;
  while (true) {
    if (stop_requested != 0) {
      break;
    }
    status = runtime_view->draw_frame();
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Test app draw failed: "
                    << puc::tui::status_message(status);
      break;
    }
  }

  const puc::app::Status stop_status      = app.stop();
  const puc::app::Status terminate_status = app.terminate();
  return puc::tui::is_ok(status) && puc::app::is_ok(stop_status) &&
                 puc::app::is_ok(terminate_status)
             ? 0
             : 1;
}
