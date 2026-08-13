/**
 * @file terminal_test_runtime.cpp
 * @brief Lifecycle-owned terminal input conformance runtime.
 *
 * This runtime complements `puc-cli/tui/test-app.cpp`. The TUI test app
 * validates presentation; this code reuses its Screen, Canvas, Layout,
 * ZBuffer, Theme, ParallelRenderer, four-worker ownership, resize handling,
 * and small-screen behavior as a trusted interface around the real terminal
 * input stack. TerminalSubsystem reads and decodes bytes with the
 * runtime-configured terminfo/TOML Trie, then publishes normalized terminal
 * Events for this application to match.
 *
 * Run `bazel run //puc-cli/test_apps:terminal-test` in a real terminal for
 * the complete plan. Append `-- --list` to inspect stable test names without
 * entering terminal mode, or `-- --test clipboard-paste` to execute only the
 * named check.
 *
 * The central box shows one of fourteen actions and a fifteen-heartbeat
 * countdown. A correct event turns the border green briefly; a timeout turns
 * it red and advances. The top-right panel identifies the selected terminal
 * environment. Mouse tracking, focus reporting, and bracketed paste are
 * enabled only for this reversible alternate-screen session. After the final
 * check, the terminal is restored before a durable pass/timeout report is
 * printed.
 *
 * During the clipboard check, normal mouse input creates a PUC-owned logical
 * selection and highlight. Selection alone never mutates the host clipboard.
 * The active OS-default or user-overridden Trie mapping emits a CommandEvent;
 * COPY then calls Screen::copy_selection() before the operator pastes the
 * result back. No key chord is hardcoded in this application.
 *
 * The file-drop check records the portable fallback exposed by most terminal
 * emulators: ordinary or bracketed-paste text containing a path. PUC does not
 * infer a structured drop from arbitrary text, and this test does not enable a
 * terminal-specific drag-and-drop extension.
 *
 * `PUC_CONFIG_ROOT` may select the primary configuration directory and
 * `PUC_USER_CONFIG_ROOT` its user-overlay directory. Without them, the program
 * locates Bazel's `input_keys.toml` runfile and uses no user overlay.
 */

#include "puc-cli/test_apps/terminal/terminal_test_runtime.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "msgs/null_message.hpp"
#include "puc-cli/application/application_control_subsystem.hpp"
#include "puc-cli/test_apps/terminal/terminal_test_runner.hpp"
#include "puc-cli/test_apps/terminal/terminal_test_selection.hpp"
#include "puc-cli/tui/rendering/canvas.hpp"
#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/rendering/layout.hpp"
#include "puc-cli/tui/rendering/presentation_subsystem.hpp"
#include "puc-cli/tui/rendering/renderer.hpp"
#include "puc-cli/tui/rendering/screen.hpp"
#include "puc-cli/tui/rendering/screen_subsystem.hpp"
#include "puc-cli/tui/rendering/theme.hpp"
#include "puc-cli/tui/terminal/configuration_paths.hpp"
#include "puc-cli/tui/terminal/terminal_subsystem.hpp"
#include "themes/theme_subsystem.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/logger/logger.hpp"
#include "utils/metronome/metronome.hpp"
#include "utils/metronome/metronome_subsystem.hpp"
#include "utils/timer/deadline.hpp"

/** @cond TERMINAL_TEST_LOGGER_MODULE */
LOGGER_MODULE("Terminal Test Runtime");
/** @endcond */

namespace {

using puc::terminal::HighlightSpan;
using puc::terminal::InputConformanceOutcome;
using puc::terminal::InputConformancePhase;
using puc::terminal::InputConformanceResult;
using puc::terminal::InputConformanceRunner;
using puc::terminal::InputConformanceView;
using puc::terminal::InputInteractionRegion;
using puc::terminal::SelectableLine;
using puc::terminal::TerminalTestSelection;
using puc::tui::Canvas;
using puc::tui::CellDimensions;
using puc::tui::Frame;
using puc::tui::Layout;
using puc::tui::ParallelRenderer;
using puc::tui::Screen;
using puc::tui::Theme;

/** Approximate presentation cadence; input is polled once per frame. */
constexpr std::chrono::milliseconds kFrameDelay{16};
/** Maximum central instruction-box geometry. */
constexpr std::size_t kPromptWidth  = 72U;
constexpr std::size_t kPromptHeight = 9U;
/** Width and height of the top-right terminal information panel. */
constexpr std::size_t kInfoWidth  = 36U;
constexpr std::size_t kInfoHeight = 5U;
/** Minimum geometry that keeps the prompt usable and below terminal info. */
constexpr std::size_t kMinimumWidth = 44U;
constexpr std::size_t kMinimumHeight =
    kPromptHeight + (2U * (kInfoHeight + 2U));

/** Terminal and host labels displayed and retained in the final report. */
struct EnvironmentInfo {
  std::string term;
  std::string term_program;
  std::string color_term;
  std::string operating_system;
};

/** Render an absent environment value without changing its semantic value. */
std::string_view environment_label(std::string_view value) noexcept {
  return value.empty() ? std::string_view{"<unset>"} : value;
}

/** Replace control and non-ASCII bytes before rendering environment labels. */
std::string printable_ascii(std::string value) {
  for (char& byte : value) {
    const unsigned char character = static_cast<unsigned char>(byte);
    if (character < 0x20U || character > 0x7eU) {
      byte = '?';
    }
  }
  return value;
}

/** Collect terminal variables and the POSIX uname identity once. */
EnvironmentInfo collect_environment() {
  EnvironmentInfo info{
      .term = printable_ascii(puc::terminal::environment_value("TERM")),
      .term_program =
          printable_ascii(puc::terminal::environment_value("TERM_PROGRAM")),
      .color_term =
          printable_ascii(puc::terminal::environment_value("COLORTERM")),
      .operating_system = "<unknown>",
  };
  struct utsname identity {};
  if (::uname(&identity) == 0) {
    info.operating_system = printable_ascii(std::format(
        "{} {} {}", identity.sysname, identity.release, identity.machine));
  }
  return info;
}

/** Construct one complete Canvas cell. */
Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Write clipped ASCII bytes at one grid position. */
void write_text(std::vector<std::vector<Canvas::Cell>>& cells, std::size_t row,
                std::size_t column, std::string_view text,
                std::uint32_t foreground, std::uint32_t background) {
  if (row >= cells.size() || column >= cells[row].size()) {
    return;
  }
  const std::size_t count = std::min(text.size(), cells[row].size() - column);
  for (std::size_t index = 0U; index < count; ++index) {
    cells[row][column + index] =
        cell(static_cast<unsigned char>(text[index]), foreground, background);
  }
}

/** Write clipped text centered in one full grid row. */
void write_centered(std::vector<std::vector<Canvas::Cell>>& cells,
                    std::size_t row, std::string_view text,
                    std::uint32_t foreground, std::uint32_t background) {
  if (row >= cells.size()) {
    return;
  }
  const std::size_t count = std::min(text.size(), cells[row].size());
  write_text(cells, row, (cells[row].size() - count) / 2U,
             text.substr(0U, count), foreground, background);
}

/** Draw a single-line Unicode border around one half-open local rectangle. */
void draw_border(std::vector<std::vector<Canvas::Cell>>& cells,
                 const Canvas::Rect& rect, std::uint32_t foreground,
                 std::uint32_t background) {
  if (rect.width < 2U || rect.height < 2U ||
      rect.y + rect.height > cells.size() || cells.empty() ||
      rect.x + rect.width > cells.front().size()) {
    return;
  }
  const std::size_t right  = rect.x + rect.width - 1U;
  const std::size_t bottom = rect.y + rect.height - 1U;
  for (std::size_t x = rect.x + 1U; x < right; ++x) {
    cells[rect.y][x] = cell(U'─', foreground, background);
    cells[bottom][x] = cell(U'─', foreground, background);
  }
  for (std::size_t y = rect.y + 1U; y < bottom; ++y) {
    cells[y][rect.x] = cell(U'│', foreground, background);
    cells[y][right]  = cell(U'│', foreground, background);
  }
  cells[rect.y][rect.x] = cell(U'┌', foreground, background);
  cells[rect.y][right]  = cell(U'┐', foreground, background);
  cells[bottom][rect.x] = cell(U'└', foreground, background);
  cells[bottom][right]  = cell(U'┘', foreground, background);
}

/** Wrap ASCII words without writing beyond a fixed number of output rows. */
std::vector<std::string> wrap_text(std::string_view text, std::size_t width,
                                   std::size_t maximum_lines) {
  std::vector<std::string> lines;
  if (width == 0U || maximum_lines == 0U) {
    return lines;
  }
  std::size_t offset = 0U;
  while (offset < text.size() && lines.size() < maximum_lines) {
    while (offset < text.size() && text[offset] == ' ') {
      ++offset;
    }
    if (offset == text.size()) {
      break;
    }
    const std::size_t remaining = text.size() - offset;
    if (remaining <= width) {
      lines.emplace_back(text.substr(offset));
      break;
    }
    std::size_t split = text.rfind(' ', offset + width);
    if (split == std::string_view::npos || split < offset) {
      split = offset + width;
    }
    lines.emplace_back(text.substr(offset, split - offset));
    offset = split == offset + width ? split : split + 1U;
  }
  if (offset < text.size() && !lines.empty() && width >= 3U) {
    std::string& last = lines.back();
    if (last.size() > width - 3U) {
      last.resize(width - 3U);
    }
    last.append("...");
  }
  return lines;
}

/** Full-screen prompt, countdown, feedback border, and small-screen fallback.
 */
class ConformanceFrame final : public Frame {
 public:
  /** Borrow the runner and capture typed application selection state. */
  ConformanceFrame(InputConformanceRunner& runner,
                   std::shared_ptr<TerminalTestSelection> selection_state)
      : Frame("input-conformance"),
        runner_(runner),
        selection_state_(std::move(selection_state)) {}

  bool is_selectable() const noexcept override {
    const InputConformanceView view = runner_.view();
    return view.phase == InputConformancePhase::ACTIVE &&
           view.test == puc::terminal::InputConformanceTest::CLIPBOARD_PASTE;
  }

  puc::tui::Status update_selection(
      const puc::tui::SelectionEvent& event) override {
    return is_selectable() || event.type == puc::tui::SelectionEventType::RESET
               ? selection_state_->update(event)
               : puc::tui::Status::FRAME_NOT_SELECTABLE;
  }

  puc::tui::Status selected_text(std::string& output) const override {
    return selection_state_->selected_text(output);
  }

  puc::tui::Status draw(const Theme& theme, Canvas& canvas,
                        const Canvas::Rect& rect) override {
    const Theme::Colors colors = theme.get_colors();
    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height,
        std::vector<Canvas::Cell>(rect.width,
                                  cell(U' ', colors.text, colors.background)));
    if (rect.width < kMinimumWidth || rect.height < kMinimumHeight) {
      selection_state_->set_lines({});
      runner_.set_interaction_region({});
      write_centered(cells, rect.height / 2U, "Screen too small",
                     colors.text_warning, colors.background);
      return canvas.write_cells(rect, cells);
    }

    const InputConformanceView view = runner_.view();
    const std::size_t box_width     = std::min(kPromptWidth, rect.width - 4U);
    const std::size_t box_height    = kPromptHeight;
    const Canvas::Rect box{
        .x      = (rect.width - box_width) / 2U,
        .y      = (rect.height - box_height) / 2U,
        .width  = box_width,
        .height = box_height,
    };
    runner_.set_interaction_region(InputInteractionRegion{
        .x      = rect.x + box.x,
        .y      = rect.y + box.y,
        .width  = box.width,
        .height = box.height,
    });

    std::uint32_t border = colors.primary;
    if (view.phase == InputConformancePhase::PASSED_FEEDBACK) {
      border = colors.text_success;
    } else if (view.phase == InputConformancePhase::TIMED_OUT_FEEDBACK) {
      border = colors.text_error;
    }
    draw_border(cells, box, border, colors.background);
    write_centered(
        cells, box.y - 2U,
        std::format("Test {} of {}", view.test_number, view.test_count),
        colors.text_emphasis, colors.background);
    write_centered(cells, box.y + 2U, view.name, colors.text_emphasis,
                   colors.background);

    const std::vector<std::string> instructions =
        wrap_text(view.instruction, box.width - 6U, 3U);
    std::vector<SelectableLine> selectable_lines;
    if (is_selectable()) {
      selectable_lines.reserve(instructions.size());
      for (std::size_t line = 0U; line < instructions.size(); ++line) {
        const std::size_t count =
            std::min(instructions[line].size(), box.width - 2U);
        selectable_lines.push_back(SelectableLine{
            .x    = static_cast<std::int64_t>(box.x + (box.width - count) / 2U),
            .y    = static_cast<std::int64_t>(box.y + 4U + line),
            .text = instructions[line].substr(0U, count),
        });
      }
    }
    selection_state_->set_lines(selectable_lines);
    const std::vector<HighlightSpan> highlights =
        selection_state_->highlight_spans();
    for (std::size_t line = 0U; line < instructions.size(); ++line) {
      const std::size_t count =
          std::min(instructions[line].size(), box.width - 2U);
      const std::size_t x = box.x + (box.width - count) / 2U;
      for (std::size_t column = 0U; column < count; ++column) {
        const bool selected =
            std::any_of(highlights.begin(), highlights.end(),
                        [line, column](const HighlightSpan& span) {
                          return span.line == line && column >= span.first &&
                                 column <= span.last;
                        });
        cells[box.y + 4U + line][x + column] =
            cell(static_cast<unsigned char>(instructions[line][column]),
                 selected ? colors.highlight_text : colors.text,
                 selected ? colors.highlight_background : colors.background);
      }
    }
    write_centered(cells, box.y + box.height - 2U, view.last_observation,
                   colors.text_muted, colors.background);
    write_centered(cells, box.y + box.height + 1U,
                   std::format("{} second{} remaining", view.seconds_remaining,
                               view.seconds_remaining == 1U ? "" : "s"),
                   colors.text_info, colors.background);
    return canvas.write_cells(rect, cells);
  }

 private:
  InputConformanceRunner& runner_; /**< Synchronized test-plan state. */
  std::shared_ptr<TerminalTestSelection>
      selection_state_; /**< Typed logical selection state. */
};

/** Top-right identification of the terminfo selector and host environment. */
class EnvironmentFrame final : public Frame {
 public:
  /** Copy immutable report/display labels. */
  explicit EnvironmentFrame(EnvironmentInfo info)
      : Frame("terminal-info"), info_(std::move(info)) {}

  puc::tui::Status draw(const Theme& theme, Canvas& canvas,
                        const Canvas::Rect& rect) override {
    const Theme::Colors colors = theme.get_colors();
    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height,
        std::vector<Canvas::Cell>(rect.width,
                                  cell(U' ', colors.text, colors.background)));
    draw_border(cells, Canvas::Rect{.width = rect.width, .height = rect.height},
                colors.text_secondary, colors.background);
    write_text(cells, 1U, 1U,
               "TERM: " + std::string{environment_label(info_.term)},
               colors.text_secondary, colors.background);
    write_text(cells, 2U, 1U,
               "program: " + std::string{environment_label(info_.term_program)},
               colors.text_secondary, colors.background);
    write_text(cells, 3U, 1U,
               "color: " + std::string{environment_label(info_.color_term)},
               colors.text_secondary, colors.background);
    return canvas.write_cells(rect, cells);
  }

 private:
  EnvironmentInfo info_; /**< Sanitized immutable environment values. */
};

/** Own one durable conformance plan over restartable shared services. */
class TerminalTestApplication final {
 public:
  /** Capture environment and retain durable terminal and exit control. */
  TerminalTestApplication(
      puc::app::TerminalSubsystem& terminal,
      puc::app::ApplicationControl& control,
      std::optional<puc::terminal::InputConformanceTest> selected_test)
      : terminal_(&terminal),
        control_(&control),
        environment_(collect_environment()),
        runner_(InputConformanceRunner::kDefaultFeedbackDuration,
                selected_test) {}

  TerminalTestApplication(const TerminalTestApplication&)            = delete;
  TerminalTestApplication& operator=(const TerminalTestApplication&) = delete;

  /** Ensure terminal resources are quiesced before borrowed workers stop. */
  ~TerminalTestApplication() { static_cast<void>(shutdown()); }

  /**
   * Bind a running presentation generation, build frames, and subscribe.
   *
   * @param[in,out] active_screen Screen for the current run generation.
   * @param[in,out] active_renderer Renderer for the current worker generation.
   * @param[in] active_theme Property-backed semantic palette.
   */
  bool setup(Screen& active_screen, ParallelRenderer& active_renderer,
             Theme& active_theme) {
    screen_                 = &active_screen;
    renderer_               = &active_renderer;
    theme_                  = &active_theme;
    puc::tui::Status status = puc::tui::Status::OK;

    puc::timer::Deadline<> geometry_timeout;
    geometry_timeout.arm(std::chrono::seconds{2});
    while (true) {
      status = screen_->get_dimensions(width_, height_, cell_dimensions_);
      if (puc::tui::is_ok(status)) {
        break;
      }
      if (status != puc::tui::Status::TERMINAL_QUERY_FAILED ||
          geometry_timeout.due()) {
        Logger<ERROR> << "Could not observe terminal geometry: "
                      << puc::tui::status_message(status);
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    screen_too_small_.store(width_ < kMinimumWidth || height_ < kMinimumHeight,
                            std::memory_order_release);
    if (!attach_canvas(width_, height_) || !setup_layout() ||
        !update_layout()) {
      return false;
    }

    const puc::ipc::Status subscription_status =
        screen_->ipc_directory().subscribe(
            puc::metronome::kOneHertzChannel,
            [this](puc::ipc::Channel::Bytes payload) noexcept {
              puc::msg::NullMessage message;
              if (puc::msg::is_ok(null_codec_.deserialize(payload, message)) &&
                  !screen_too_small_.load(std::memory_order_acquire)) {
                runner_.tick();
              }
            },
            heartbeat_subscription_);
    if (!puc::ipc::is_ok(subscription_status)) {
      Logger<ERROR> << "Could not subscribe to metronome: "
                    << puc::ipc::status_message(subscription_status);
      return false;
    }
    return true;
  }

  /** Poll available terminal bytes, update state, render, and present once. */
  bool draw() {
    std::size_t current_width  = 0U;
    std::size_t current_height = 0U;
    CellDimensions current_cells;
    puc::tui::Status status =
        screen_->get_dimensions(current_width, current_height, current_cells);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not refresh terminal geometry: "
                    << puc::tui::status_message(status);
      return false;
    }
    if (current_width != width_ || current_height != height_ ||
        current_cells != cell_dimensions_) {
      const bool dimensions_changed =
          current_width != width_ || current_height != height_;
      width_           = current_width;
      height_          = current_height;
      cell_dimensions_ = current_cells;
      if (dimensions_changed && !attach_canvas(width_, height_)) {
        return false;
      }
      if (!update_layout()) {
        return false;
      }
    }

    const bool too_small = width_ < kMinimumWidth || height_ < kMinimumHeight;
    screen_too_small_.store(too_small, std::memory_order_release);
    if (!too_small) {
      runner_.update();
    }
    if (!poll_input()) {
      return false;
    }
    const Theme::Colors colors = theme_->get_colors();
    const auto& active_description =
        too_small ? small_layout_description_ : layout_description_;
    const Layout::AbsoluteLayout& active_layout =
        too_small ? small_absolute_layout_ : absolute_layout_;
    status =
        renderer_->start(active_description, active_layout, *theme_, canvas_,
                         cell(U' ', colors.text, colors.background));
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not schedule prompt frames: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = renderer_->wait();
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Prompt frame failed: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = screen_->draw();
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not present prompt: "
                    << puc::tui::status_message(status);
      return false;
    }
    std::this_thread::sleep_for(kFrameDelay);
    return true;
  }

  /** Return whether the runner has resolved every planned check. */
  bool finished() const noexcept { return runner_.finished(); }

  /** Restore the terminal and release all objects that borrow the worker pool.
   */
  bool shutdown() noexcept {
    heartbeat_subscription_.reset();
    bool quiesced = true;
    if (renderer_ != nullptr) {
      quiesced = puc::tui::is_ok(renderer_->wait());
    }
    canvas_.reset();
    layout_description_.reset();
    small_layout_description_.reset();
    absolute_layout_       = {};
    small_absolute_layout_ = {};
    selection_timeout_.cancel();
    screen_too_small_.store(false, std::memory_order_release);
    screen_   = nullptr;
    renderer_ = nullptr;
    theme_    = nullptr;
    return quiesced;
  }

  /** Print the durable post-terminal report and report complete success. */
  bool print_report() const {
    const std::vector<InputConformanceResult> results = runner_.results();
    std::size_t passed                                = 0U;
    const std::size_t planned                         = runner_.plan_size();
    std::cout << "PUC terminal input conformance report\n\n"
              << "TERM:         " << environment_label(environment_.term)
              << '\n'
              << "TERM_PROGRAM: "
              << environment_label(environment_.term_program) << '\n'
              << "COLORTERM:    " << environment_label(environment_.color_term)
              << '\n'
              << "OS:           " << environment_.operating_system << '\n'
              << "Screen:       " << width_ << 'x' << height_ << " cells, "
              << "cell ratio " << cell_dimensions_.width << ':'
              << cell_dimensions_.height << "\n\n";
    for (std::size_t index = 0U; index < results.size(); ++index) {
      const InputConformanceResult& result = results[index];
      if (result.outcome == InputConformanceOutcome::PASSED) {
        ++passed;
      }
      std::cout << std::format(
          "[{}] {}/{} {}\n      {}\n",
          puc::terminal::input_conformance_outcome_name(result.outcome),
          index + 1U, planned, result.name, result.detail);
    }
    const std::size_t incomplete = planned - results.size();
    std::cout << '\n'
              << "Summary: " << passed << " passed, " << results.size() - passed
              << " timed out, " << incomplete << " incomplete; " << planned
              << " total\n";
    return results.size() == planned && passed == planned;
  }

 private:
  /** Attach a fresh Canvas transaction pair for new screen dimensions. */
  bool attach_canvas(std::size_t width, std::size_t height) {
    auto next = std::make_shared<Canvas>(width, height);
    if (!puc::tui::is_ok(next->get_status())) {
      Logger<ERROR> << "Could not allocate Canvas: "
                    << puc::tui::status_message(next->get_status());
      return false;
    }
    const puc::tui::Status status = screen_->set_canvas(next);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not attach Canvas: "
                    << puc::tui::status_message(status);
      return false;
    }
    canvas_ = std::move(next);
    return true;
  }

  /** Construct the full-screen prompt and frontmost terminal-info panel. */
  bool setup_layout() {
    layout_description_ =
        layout_.make_layout_description("terminal-input-conformance");
    const std::shared_ptr<Frame> prompt =
        std::make_shared<ConformanceFrame>(runner_, selection_state_);
    puc::tui::Status status =
        layout_.add_frame(layout_description_, "prompt", prompt, {});
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not add conformance frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = layout_.add_frame(
        layout_description_, "terminal-info",
        std::make_shared<EnvironmentFrame>(environment_),
        {
            Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                              kInfoWidth),
            Layout::make_character_constraint(Layout::ConstraintType::MAX_WIDTH,
                                              kInfoWidth),
            Layout::make_character_constraint(
                Layout::ConstraintType::MIN_HEIGHT, kInfoHeight),
            Layout::make_character_constraint(
                Layout::ConstraintType::MAX_HEIGHT, kInfoHeight),
            Layout::make_character_constraint(
                Layout::ConstraintType::RIGHT_ANCHOR, 0U),
            Layout::make_character_constraint(
                Layout::ConstraintType::TOP_ANCHOR, 0U),
        });
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not add environment frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    small_layout_description_ =
        layout_.make_layout_description("terminal-input-too-small");
    status = layout_.add_frame(small_layout_description_, "prompt", prompt, {});
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not add small-screen frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    return true;
  }

  /** Recompute rectangles and the render execution graph after resize. */
  bool update_layout() {
    puc::tui::Status status =
        layout_.compute_absolute_layout(layout_description_, width_, height_,
                                        cell_dimensions_, absolute_layout_);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not compute prompt layout: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = layout_.compute_absolute_layout(small_layout_description_, width_,
                                             height_, cell_dimensions_,
                                             small_absolute_layout_);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not compute small-screen layout: "
                    << puc::tui::status_message(status);
      return false;
    }
    return true;
  }

  /** Ask TerminalSubsystem to poll, decode, and publish available input. */
  bool poll_input() {
    bool end_of_input = false;
    if (terminal_ == nullptr ||
        !puc::app::is_ok(terminal_->poll_input(std::chrono::milliseconds{0},
                                               end_of_input))) {
      Logger<ERROR> << "Could not poll normalized terminal input";
      return false;
    }
    if (end_of_input) {
      control_->request_exit();
    }

    std::vector<puc::terminal::Event> events;
    const puc::tui::Status input_status = screen_->drain_input_events(events);
    if (!puc::tui::is_ok(input_status)) {
      Logger<ERROR> << "Could not consume normalized terminal input: "
                    << puc::tui::status_message(input_status);
      return false;
    }
    try {
      for (const puc::terminal::Event& event : events) {
        if (!screen_too_small_.load(std::memory_order_acquire) &&
            !observe_terminal_event(event)) {
          return false;
        }
        refresh_selection_timeout();
      }
    } catch (...) {
      Logger<ERROR> << "Could not apply normalized terminal input";
      return false;
    }
    return resolve_selection_timeout();
  }

  /** Route one decoded event through Screen selection and the test matcher. */
  bool observe_terminal_event(const puc::terminal::Event& event) {
    if (const auto* mouse_event =
            std::get_if<puc::terminal::MouseEvent>(&event)) {
      const bool too_small = screen_too_small_.load(std::memory_order_acquire);
      if (!too_small) {
        const puc::tui::Status status = screen_->handle_mouse_event(
            *mouse_event, layout_description_->z_buffer,
            absolute_layout_.frame_layouts);
        if (!puc::tui::is_ok(status) &&
            status != puc::tui::Status::NO_SELECTION) {
          Logger<ERROR> << "Could not route mouse selection event: "
                        << puc::tui::status_message(status);
          return false;
        }
      }
    }
    if (const auto* command = std::get_if<puc::terminal::CommandEvent>(&event);
        command != nullptr &&
        command->command == puc::terminal::Command::COPY) {
      const puc::tui::Status status = screen_->copy_selection();
      if (!puc::tui::is_ok(status) &&
          status != puc::tui::Status::NO_SELECTION) {
        Logger<ERROR> << "Could not copy the PUC-owned selection: "
                      << puc::tui::status_message(status);
        return false;
      }
    }
    runner_.observe(event);
    return true;
  }

  /** Synchronize the click recognizer with its shared timeout primitive. */
  void refresh_selection_timeout() {
    selection_timeout_.synchronize(
        screen_->pending_selection_timeout(),
        terminal_->timeout_settings().multiple_click);
  }

  /** Deliver a due click-recognizer generation. */
  bool resolve_selection_timeout() {
    refresh_selection_timeout();
    const std::optional<puc::terminal::TimeoutInput> due =
        selection_timeout_.take_if_due();
    if (!due.has_value()) {
      return true;
    }
    const puc::tui::Status status = screen_->handle_selection_timeout(*due);
    refresh_selection_timeout();
    return puc::tui::is_ok(status);
  }

  puc::app::TerminalSubsystem* terminal_ =
      nullptr; /**< Lifecycle-owned decoder and publisher. */
  puc::app::ApplicationControl* control_ =
      nullptr;                    /**< Durable application-exit request sink. */
  EnvironmentInfo environment_;   /**< Stable terminal/host identity. */
  InputConformanceRunner runner_; /**< Timed event matcher. */
  std::shared_ptr<TerminalTestSelection> selection_state_ =
      std::make_shared<TerminalTestSelection>(); /**< Typed prompt state. */
  Screen* screen_ = nullptr; /**< Lifecycle-owned presentation/session. */
  ParallelRenderer* renderer_ =
      nullptr; /**< Lifecycle-owned frame scheduler. */
  puc::ipc::Subscription
      heartbeat_subscription_;            /**< Tick callback lifetime. */
  puc::msg::NullMessageCodec null_codec_; /**< Heartbeat payload validator. */
  std::shared_ptr<Canvas> canvas_;        /**< Current screen-sized buffers. */
  std::shared_ptr<Layout::LayoutDescription>
      layout_description_; /**< Prompt and terminal-info frames. */
  std::shared_ptr<Layout::LayoutDescription>
      small_layout_description_; /**< Prompt-only small-screen fallback. */
  Layout::AbsoluteLayout absolute_layout_; /**< Current rectangles/DAG. */
  Layout::AbsoluteLayout
      small_absolute_layout_; /**< Current small-screen rectangle/DAG. */
  Layout layout_;             /**< Constraint solver. */
  Theme* theme_ = nullptr;    /**< Borrowed property-backed palette. */
  CellDimensions cell_dimensions_ =
      puc::tui::kDefaultCellDimensions; /**< Physical cell proportions. */
  std::size_t width_  = 0U;             /**< Latest terminal columns. */
  std::size_t height_ = 0U;             /**< Latest terminal rows. */
  std::atomic<bool> screen_too_small_ =
      false; /**< Pauses tests while hidden. */
  puc::timer::TokenDeadline<puc::terminal::TimeoutInput>
      selection_timeout_; /**< Current explicit click timeout. */
};

}  // namespace

namespace puc::app {

/** Hidden durable plan and restartable presentation ownership. */
class TerminalTestRuntimeSubsystem::Impl final {
 public:
  /** Retain the immutable command-line plan selection. */
  explicit Impl(std::optional<terminal::InputConformanceTest> selected)
      : selected_test(selected) {}

  std::optional<terminal::InputConformanceTest>
      selected_test; /**< Durable command-line plan selection. */
  std::shared_ptr<TerminalTestApplication>
      application; /**< Durable runner and restartable presentation bindings. */
};

TerminalTestRuntimeSubsystem::TerminalTestRuntimeSubsystem(
    std::optional<terminal::InputConformanceTest> selected_test)
    : AppSubsystem(
          "terminal-test-runtime",
          subsystem_dependencies<ApplicationControlSubsystem, TerminalSubsystem,
                                 ScreenSubsystem, PresentationSubsystem,
                                 MetronomeSubsystem, ThemeSubsystem>()),
      impl_(std::make_unique<Impl>(selected_test)) {}

TerminalTestRuntimeSubsystem::~TerminalTestRuntimeSubsystem() = default;

Status TerminalTestRuntimeSubsystem::initialize(AppState& app) {
  auto* terminal = app.get_subsystem<TerminalSubsystem>();
  auto* control  = app.get_subsystem<ApplicationControlSubsystem>();
  if (impl_ == nullptr || terminal == nullptr ||
      terminal->decoder() == nullptr || control == nullptr ||
      control->control() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  impl_->application = std::make_shared<TerminalTestApplication>(
      *terminal, *control->control(), impl_->selected_test);
  return Status::OK;
}

Status TerminalTestRuntimeSubsystem::start(AppState& app) {
  auto* screen       = app.get_subsystem<ScreenSubsystem>();
  auto* presentation = app.get_subsystem<PresentationSubsystem>();
  auto* metronome    = app.get_subsystem<MetronomeSubsystem>();
  auto* theme        = app.get_subsystem<ThemeSubsystem>();
  if (impl_ == nullptr || impl_->application == nullptr || screen == nullptr ||
      screen->screen() == nullptr || presentation == nullptr ||
      presentation->renderer() == nullptr || metronome == nullptr ||
      metronome->metronome() == nullptr || theme == nullptr ||
      theme->theme() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (!impl_->application->setup(*screen->screen(), *presentation->renderer(),
                                 *theme->theme())) {
    static_cast<void>(impl_->application->shutdown());
    return Status::SUBSYSTEM_FAILURE;
  }
  return Status::OK;
}

Status TerminalTestRuntimeSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  return impl_ == nullptr || impl_->application == nullptr ||
                 impl_->application->shutdown()
             ? Status::OK
             : Status::SUBSYSTEM_FAILURE;
}

Status TerminalTestRuntimeSubsystem::terminate(AppState& app) noexcept {
  const Status status = stop(app);
  if (impl_ != nullptr) {
    impl_->application.reset();
  }
  impl_.reset();
  return status;
}

bool TerminalTestRuntimeSubsystem::draw() {
  return impl_ != nullptr && impl_->application != nullptr &&
         impl_->application->draw();
}

bool TerminalTestRuntimeSubsystem::finished() const {
  return impl_ == nullptr || impl_->application == nullptr ||
         impl_->application->finished();
}

bool TerminalTestRuntimeSubsystem::print_report() const {
  return impl_ != nullptr && impl_->application != nullptr &&
         impl_->application->print_report();
}

}  // namespace puc::app
