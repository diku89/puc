/**
 * @file terminal_test.cpp
 * @brief Interactive end-to-end conformance test for terminal input decoding.
 *
 * This manual executable complements `puc-cli/tui/test-app.cpp`. The TUI test
 * app validates presentation; this program reuses its Screen, Canvas, Layout,
 * ZBuffer, Theme, ParallelRenderer, four-worker ownership, resize handling,
 * and small-screen behavior as a trusted interface around the real terminal
 * input stack. Bytes are read through Screen's owned TerminalSession, decoded
 * by the runtime-configured terminfo/TOML Trie, and matched as normalized
 * terminal Events.
 *
 * Run `bazel run //puc-cli/terminal:terminal-test` in a real terminal for the
 * complete plan. Append `-- --list` to inspect stable test names without
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

#include "puc-cli/state/terminal.hpp"

#include <poll.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "msgs/null_message.hpp"
#include "msgs/screen_msgs.hpp"
#include "puc-cli/state/bootstrap.hpp"
#include "puc-cli/state/configuration.hpp"
#include "puc-cli/state/metronome.hpp"
#include "puc-cli/state/presentation.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/terminal/decoder.hpp"
#include "puc-cli/terminal/sequences.hpp"
#include "puc-cli/terminal/terminal_test_options.hpp"
#include "puc-cli/terminal/terminal_test_runner.hpp"
#include "puc-cli/terminal/terminal_test_selection.hpp"
#include "puc-cli/terminal/timeouts.hpp"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/layout.hpp"
#include "puc-cli/tui/renderer.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/theme.hpp"
#include "utils/config/config.hpp"
#include "utils/ipc/channel.hpp"
#include "utils/logger/logger.hpp"
#include "utils/metronome/metronome.hpp"

/** @cond TERMINAL_TEST_LOGGER_MODULE */
LOGGER_MODULE("Terminal Test App");
/** @endcond */

namespace {

namespace msg = puc::msg;

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

/** Worker budget shared by presentation, IPC delivery, and the heartbeat. */
constexpr std::uint8_t kWorkerCount = 4U;
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

/** Async-signal-safe shutdown request set by SIGINT or SIGTERM. */
volatile std::sig_atomic_t stop_requested = 0;

/** Request orderly terminal restoration without doing work in a signal. */
void request_stop(int signal_number) noexcept {
  static_cast<void>(signal_number);
  stop_requested = 1;
}

/** Terminal and host labels displayed and retained in the final report. */
struct EnvironmentInfo {
  std::string term;
  std::string term_program;
  std::string color_term;
  std::string operating_system;
};

/** Return one environment value without retaining a raw environment pointer. */
std::string environment_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

/** Render an absent environment value without changing its semantic value. */
std::string_view environment_label(std::string_view value) noexcept {
  return value.empty() ? std::string_view{"<unset>"} : value;
}

/** Print command forms without acquiring terminal resources. */
void print_usage(std::ostream& output, std::string_view executable) {
  const std::string_view program =
      executable.empty() ? std::string_view{"terminal-test"} : executable;
  output << "Usage:\n"
         << "  " << program << "\n"
         << "  " << program << " --list\n"
         << "  " << program << " --test <test-name>\n"
         << "  " << program << " --help\n";
}

/** Print stable `--test` names, labels, and operator instructions. */
void print_test_list(std::ostream& output) {
  output << "Available terminal input tests:\n";
  for (const puc::terminal::InputConformanceTestDescriptor& descriptor :
       puc::terminal::input_conformance_tests()) {
    output << "  " << descriptor.cli_name << "\n"
           << "      " << descriptor.name << ": " << descriptor.instruction
           << '\n';
  }
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
      .term             = printable_ascii(environment_value("TERM")),
      .term_program     = printable_ascii(environment_value("TERM_PROGRAM")),
      .color_term       = printable_ascii(environment_value("COLORTERM")),
      .operating_system = "<unknown>",
  };
  struct utsname identity {};
  if (::uname(&identity) == 0) {
    info.operating_system = printable_ascii(std::format(
        "{} {} {}", identity.sysname, identity.release, identity.machine));
  }
  return info;
}

/** Return whether a directory contains every packaged terminal configuration.
 */
bool contains_terminal_configuration(const std::filesystem::path& root) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(root / "input_keys.toml", error) ||
      error) {
    return false;
  }
  error.clear();
  const std::string_view operating_system_defaults =
      puc::terminal::operating_system_defaults_path(
          puc::terminal::current_operating_system());
  if (operating_system_defaults.empty() ||
      !std::filesystem::is_regular_file(root / operating_system_defaults,
                                        error) ||
      error) {
    return false;
  }
  error.clear();
  return std::filesystem::is_regular_file(
             root / puc::terminal::kTimeoutConfigurationPath, error) &&
         !error;
}

/**
 * Locate an installed or Bazel-runfiles primary configuration root.
 *
 * The returned path may be invalid when no candidate exists; Decoder::setup()
 * then reports the authoritative configuration failure.
 */
std::filesystem::path primary_config_root(std::string_view executable) {
  const std::string configured = environment_value("PUC_CONFIG_ROOT");
  if (!configured.empty()) {
    return configured;
  }

  std::vector<std::filesystem::path> candidates;
  std::error_code error;
  candidates.push_back(std::filesystem::current_path(error));
  const std::string runfiles = environment_value("RUNFILES_DIR");
  if (!runfiles.empty()) {
    candidates.emplace_back(runfiles);
    candidates.emplace_back(std::filesystem::path{runfiles} / "_main");
    candidates.emplace_back(std::filesystem::path{runfiles} / "puc");
  }
  if (!executable.empty()) {
    const std::filesystem::path executable_path{executable};
    candidates.emplace_back(executable_path.string() + ".runfiles/_main");
    candidates.emplace_back(executable_path.string() + ".runfiles/puc");
  }

  for (const std::filesystem::path& candidate : candidates) {
    if (contains_terminal_configuration(candidate)) {
      return candidate;
    }
  }
  return candidates.empty() ? std::filesystem::path{} : candidates.front();
}

/** Return the optional user overlay root chosen for this manual run. */
std::filesystem::path user_config_root(
    const std::filesystem::path& primary_root) {
  const std::string configured = environment_value("PUC_USER_CONFIG_ROOT");
  return configured.empty() ? primary_root / ".puc-no-user-overrides"
                            : std::filesystem::path{configured};
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

/** Write a rectangular vector grid through Canvas's row-span interface. */
puc::tui::Status write_grid(Canvas& canvas, const Canvas::Rect& rect,
                            std::vector<std::vector<Canvas::Cell>>& cells) {
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
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
      return write_grid(canvas, rect, cells);
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
    return write_grid(canvas, rect, cells);
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
    return write_grid(canvas, rect, cells);
  }

 private:
  EnvironmentInfo info_; /**< Sanitized immutable environment values. */
};

/** Add a frame and each supplied constraint to a layout description. */
puc::tui::Status add_frame(
    Layout& layout,
    const std::shared_ptr<Layout::LayoutDescription>& description,
    std::string id, std::shared_ptr<Frame> frame,
    std::initializer_list<Layout::Constraint> constraints) {
  puc::tui::Status status =
      layout.add_frame_to_layout_description(description, id, std::move(frame));
  if (!puc::tui::is_ok(status)) {
    return status;
  }
  for (const Layout::Constraint& constraint : constraints) {
    status = layout.add_constraint_to_frame(description, id, constraint);
    if (!puc::tui::is_ok(status)) {
      return status;
    }
  }
  return puc::tui::Status::OK;
}

/** Own one durable conformance plan over restartable shared services. */
class TerminalTestApplication {
 public:
  /** Capture environment and retain durable decoder/configuration services. */
  TerminalTestApplication(
      puc::terminal::Decoder& decoder, const puc::config::Config& configuration,
      std::optional<puc::terminal::InputConformanceTest> selected_test)
      : decoder_(decoder),
        configuration_(configuration),
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
   */
  bool setup(Screen& active_screen, ParallelRenderer& active_renderer) {
    const puc::terminal::Status timeout_status =
        puc::terminal::load_timeout_settings(configuration_, timeout_settings_);
    if (!puc::terminal::is_ok(timeout_status)) {
      Logger<ERROR> << "Terminal timeout setup failed: "
                    << puc::terminal::status_message(timeout_status);
      return false;
    }

    screen_                 = &active_screen;
    renderer_               = &active_renderer;
    puc::tui::Status status = puc::tui::Status::OK;

    const auto geometry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (true) {
      status = screen_->get_dimensions(width_, height_, cell_dimensions_);
      if (puc::tui::is_ok(status)) {
        break;
      }
      if (status != puc::tui::Status::TERMINAL_QUERY_FAILED ||
          std::chrono::steady_clock::now() >= geometry_deadline) {
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
    const Theme::Colors colors = theme_.get_colors();
    const auto& active_description =
        too_small ? small_layout_description_ : layout_description_;
    const Layout::AbsoluteLayout& active_layout =
        too_small ? small_absolute_layout_ : absolute_layout_;
    status =
        renderer_->start(active_description, active_layout, theme_, canvas_,
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

  /** Return the number of checks selected for this run. */
  std::size_t planned_count() const noexcept { return runner_.plan_size(); }

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
    pending_decoder_timeout_.reset();
    pending_selection_timeout_.reset();
    screen_too_small_.store(false, std::memory_order_release);
    screen_   = nullptr;
    renderer_ = nullptr;
    return quiesced;
  }

  /** Print the durable post-terminal report and return its pass count. */
  std::size_t print_report() const {
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
    return passed;
  }

  /** Return the number of checks that already produced a result. */
  std::size_t completed_count() const { return runner_.results().size(); }

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
    Theme::Colors colors{};
    colors.primary              = 0x4da3ffU;
    colors.highlight_background = 0x264f78U;
    colors.highlight_text       = 0xffffffU;
    colors.text                 = 0xffffffU;
    colors.text_secondary       = 0xc0c0c0U;
    colors.text_muted           = 0x808080U;
    colors.text_error           = 0xff4040U;
    colors.text_warning         = 0xffc040U;
    colors.text_success         = 0x40d060U;
    colors.text_info            = 0x70b7ffU;
    colors.text_emphasis        = 0xffffffU;
    colors.background           = 0x000000U;
    theme_.load_colors(colors);

    layout_description_ =
        layout_.make_layout_description("terminal-input-conformance");
    const std::shared_ptr<Frame> prompt =
        std::make_shared<ConformanceFrame>(runner_, selection_state_);
    puc::tui::Status status =
        add_frame(layout_, layout_description_, "prompt", prompt, {});
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not add conformance frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = add_frame(layout_, layout_description_, "terminal-info",
                       std::make_shared<EnvironmentFrame>(environment_),
                       {
                           Layout::make_character_constraint(
                               Layout::ConstraintType::MIN_WIDTH, kInfoWidth),
                           Layout::make_character_constraint(
                               Layout::ConstraintType::MAX_WIDTH, kInfoWidth),
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
    status =
        add_frame(layout_, small_layout_description_, "prompt", prompt, {});
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

  /** Read only after poll(2) reports bytes, then feed every event to runner. */
  bool poll_input() {
    pollfd descriptor{
        .fd      = STDIN_FILENO,
        .events  = POLLIN,
        .revents = 0,
    };
    int readiness = 0;
    do {
      readiness = ::poll(&descriptor, 1U, 0);
    } while (readiness < 0 && errno == EINTR);
    if (readiness < 0) {
      Logger<ERROR> << "Could not poll terminal input";
      return false;
    }
    if (readiness == 0) {
      return resolve_pending_timeouts();
    }
    if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
      Logger<ERROR> << "Terminal input descriptor reported a poll error";
      return false;
    }
    if ((descriptor.revents & POLLIN) == 0) {
      if ((descriptor.revents & POLLHUP) != 0) {
        stop_requested = 1;
      }
      return resolve_pending_timeouts();
    }

    std::vector<puc::terminal::Event> events;
    std::size_t bytes_read = 0U;
    bool end_of_input      = false;
    const puc::terminal::Status status =
        screen_->read_input(decoder_, events, bytes_read, end_of_input);
    if (!puc::terminal::is_ok(status)) {
      Logger<ERROR> << "Could not decode terminal input: "
                    << puc::terminal::status_message(status);
      return false;
    }
    if (!screen_too_small_.load(std::memory_order_acquire)) {
      for (const puc::terminal::Event& event : events) {
        if (!observe_terminal_event(event)) {
          return false;
        }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    refresh_decoder_timeout(now);
    refresh_selection_timeout(now);
    if (end_of_input) {
      stop_requested = 1;
    }
    return true;
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

  /** Synchronize the app-owned deadline with Decoder's current token. */
  void refresh_decoder_timeout(std::chrono::steady_clock::time_point now) {
    const std::optional<puc::terminal::TimeoutInput> requested =
        decoder_.pending_timeout();
    if (!requested.has_value()) {
      pending_decoder_timeout_.reset();
      return;
    }
    if (!pending_decoder_timeout_.has_value() ||
        pending_decoder_timeout_->input != *requested) {
      pending_decoder_timeout_ = PendingTimeout{
          .input    = *requested,
          .deadline = now + timeout_settings_.input_sequence,
      };
    }
  }

  /** Synchronize the app-owned deadline with Screen's click-chain token. */
  void refresh_selection_timeout(std::chrono::steady_clock::time_point now) {
    const std::optional<puc::terminal::TimeoutInput> requested =
        screen_->pending_selection_timeout();
    if (!requested.has_value()) {
      pending_selection_timeout_.reset();
      return;
    }
    if (!pending_selection_timeout_.has_value() ||
        pending_selection_timeout_->input != *requested) {
      pending_selection_timeout_ = PendingTimeout{
          .input    = *requested,
          .deadline = now + timeout_settings_.multiple_click,
      };
    }
  }

  /** Deliver every due explicit decoder or click-recognizer timeout. */
  bool resolve_pending_timeouts() {
    const auto now = std::chrono::steady_clock::now();
    refresh_decoder_timeout(now);
    refresh_selection_timeout(now);
    if (pending_decoder_timeout_.has_value() &&
        now >= pending_decoder_timeout_->deadline) {
      std::vector<puc::terminal::Event> events;
      const puc::terminal::Status status =
          decoder_.handle_timeout(pending_decoder_timeout_->input, events);
      if (!puc::terminal::is_ok(status)) {
        Logger<ERROR> << "Could not resolve ambiguous terminal input: "
                      << puc::terminal::status_message(status);
        return false;
      }
      if (!screen_too_small_.load(std::memory_order_acquire)) {
        for (const puc::terminal::Event& event : events) {
          if (!observe_terminal_event(event)) {
            return false;
          }
        }
      }
      pending_decoder_timeout_.reset();
      refresh_decoder_timeout(now);
    }
    if (pending_selection_timeout_.has_value() &&
        now >= pending_selection_timeout_->deadline) {
      const puc::tui::Status status =
          screen_->handle_selection_timeout(pending_selection_timeout_->input);
      if (!puc::tui::is_ok(status)) {
        Logger<ERROR> << "Could not resolve multi-click timeout: "
                      << puc::tui::status_message(status);
        return false;
      }
      pending_selection_timeout_.reset();
      refresh_selection_timeout(now);
    }
    return true;
  }

  /** App-owned scheduling data for one target-issued timeout generation. */
  struct PendingTimeout {
    puc::terminal::TimeoutInput input; /**< Generation to deliver. */
    std::chrono::steady_clock::time_point deadline; /**< Delivery time. */
  };

  puc::terminal::Decoder&
      decoder_; /**< Lifecycle-owned configured input Trie. */
  const puc::config::Config&
      configuration_;             /**< Durable configuration roots. */
  EnvironmentInfo environment_;   /**< Stable terminal/host identity. */
  InputConformanceRunner runner_; /**< Timed event matcher. */
  std::shared_ptr<TerminalTestSelection> selection_state_ =
      std::make_shared<TerminalTestSelection>();    /**< Typed prompt state. */
  puc::terminal::TimeoutSettings timeout_settings_; /**< Layered durations. */
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
  Theme theme_;               /**< Semantic test palette. */
  CellDimensions cell_dimensions_ =
      puc::tui::kDefaultCellDimensions; /**< Physical cell proportions. */
  std::size_t width_  = 0U;             /**< Latest terminal columns. */
  std::size_t height_ = 0U;             /**< Latest terminal rows. */
  std::atomic<bool> screen_too_small_ =
      false; /**< Pauses tests while hidden. */
  std::optional<PendingTimeout>
      pending_decoder_timeout_; /**< Current explicit decoder timeout. */
  std::optional<PendingTimeout>
      pending_selection_timeout_; /**< Current explicit click timeout. */
};

/** Preserve the conformance plan while rebinding restartable shared services.
 */
class TerminalTestRuntimeSubsystem final : public puc::app::AppSubsystem {
 public:
  /** Retain the selected plan until the one final terminate transition. */
  explicit TerminalTestRuntimeSubsystem(
      std::optional<puc::terminal::InputConformanceTest> selected_test)
      : AppSubsystem(
            "terminal-test-runtime",
            puc::app::subsystem_dependencies<
                puc::app::ConfigurationSubsystem, puc::app::TerminalSubsystem,
                puc::app::ScreenSubsystem, puc::app::PresentationSubsystem,
                puc::app::MetronomeSubsystem>()),
        selected_test_(selected_test) {}

  /** Construct the durable runner over durable configuration and Decoder. */
  puc::app::Status initialize(puc::app::AppState& app) override {
    auto* configuration = app.get_subsystem<puc::app::ConfigurationSubsystem>();
    auto* terminal      = app.get_subsystem<puc::app::TerminalSubsystem>();
    if (configuration == nullptr || configuration->configuration() == nullptr ||
        terminal == nullptr || terminal->decoder() == nullptr) {
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    application_ = std::make_unique<TerminalTestApplication>(
        *terminal->decoder(), *configuration->configuration(), selected_test_);
    return puc::app::Status::OK;
  }

  /** Bind Screen, renderer, and heartbeat subscription for this generation. */
  puc::app::Status start(puc::app::AppState& app) override {
    auto* screen       = app.get_subsystem<puc::app::ScreenSubsystem>();
    auto* presentation = app.get_subsystem<puc::app::PresentationSubsystem>();
    auto* metronome    = app.get_subsystem<puc::app::MetronomeSubsystem>();
    if (application_ == nullptr || screen == nullptr ||
        screen->screen() == nullptr || presentation == nullptr ||
        presentation->renderer() == nullptr || metronome == nullptr ||
        metronome->metronome() == nullptr) {
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    if (!application_->setup(*screen->screen(), *presentation->renderer())) {
      static_cast<void>(application_->shutdown());
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    return puc::app::Status::OK;
  }

  /** Drop subscriptions and Canvas bindings before shared services stop. */
  puc::app::Status stop(puc::app::AppState& app) noexcept override {
    static_cast<void>(app);
    return application_ == nullptr || application_->shutdown()
               ? puc::app::Status::OK
               : puc::app::Status::SUBSYSTEM_FAILURE;
  }

  /** Release the durable conformance plan after the final report is consumed.
   */
  puc::app::Status terminate(puc::app::AppState& app) noexcept override {
    const puc::app::Status status = stop(app);
    application_.reset();
    return status;
  }

  bool draw() { return application_ != nullptr && application_->draw(); }
  bool finished() const {
    return application_ == nullptr || application_->finished();
  }
  std::size_t print_report() const {
    return application_ == nullptr ? 0U : application_->print_report();
  }
  std::size_t completed_count() const {
    return application_ == nullptr ? 0U : application_->completed_count();
  }
  std::size_t planned_count() const {
    return application_ == nullptr ? 0U : application_->planned_count();
  }

 private:
  std::optional<puc::terminal::InputConformanceTest>
      selected_test_; /**< Durable command-line plan selection. */
  std::unique_ptr<TerminalTestApplication>
      application_; /**< Durable runner and restartable presentation bindings.
                     */
};

}  // namespace

/**
 * Run the interactive terminal input conformance plan and print its report.
 *
 * @param[in] argc Conventional process argument count.
 * @param[in] argv Conventional process argument vector; argv[0] helps locate
 *                 Bazel runfiles.
 * @return Zero for successful listing/help, or when every selected check,
 *         setup operation, and terminal restoration succeeds; otherwise
 *         nonzero.
 */
int main(int argc, char** argv) {
  const puc::logger::LoggerConf logger_config{
      .global_level = puc::logger::LogLevel::WARN,
  };

  const std::string_view executable = argc > 0 && argv[0] != nullptr
                                          ? std::string_view{argv[0]}
                                          : std::string_view{};
  std::vector<std::string_view> arguments;
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  }
  puc::terminal::TerminalTestOptions options;
  const puc::terminal::TerminalTestOptionsStatus option_status =
      puc::terminal::parse_terminal_test_options(arguments, options);
  if (option_status != puc::terminal::TerminalTestOptionsStatus::OK) {
    std::cerr << "terminal-test: "
              << puc::terminal::terminal_test_options_status_message(
                     option_status);
    if (!options.argument.empty()) {
      std::cerr << ": " << options.argument;
    }
    std::cerr << "\n\n";
    print_usage(std::cerr, executable);
    std::cerr << "\nRun --list to see valid test names.\n";
    return 2;
  }
  if (options.command == puc::terminal::TerminalTestCommand::LIST) {
    print_test_list(std::cout);
    return 0;
  }
  if (options.command == puc::terminal::TerminalTestCommand::HELP) {
    print_usage(std::cout, executable);
    return 0;
  }

  if (std::signal(SIGINT, request_stop) == SIG_ERR ||
      std::signal(SIGTERM, request_stop) == SIG_ERR) {
    std::cerr << "Could not install termination signal handlers\n";
    return 1;
  }

  const std::filesystem::path primary = primary_config_root(executable);
  const std::filesystem::path user    = user_config_root(primary);
  puc::app::ApplicationSubsystemOptions subsystem_options{
      .logger       = logger_config,
      .worker_count = kWorkerCount,
      .configuration =
          puc::app::ConfigurationSubsystemOptions{
              .primary_root        = primary,
              .user_overrides_root = user,
          },
      .terminal =
          puc::app::TerminalSubsystemOptions{
              .input_fd          = STDIN_FILENO,
              .output_fd         = STDOUT_FILENO,
              .decoder_limits    = {},
              .configure_decoder = true,
              .terminal_name     = environment_value("TERM"),
          },
      .screen =
          puc::app::ScreenSubsystemOptions{
              .take_terminal = true,
              .session_options =
                  msg::ScreenSessionOptions{
                      .preserve_signals     = true,
                      .alternate_screen     = true,
                      .hide_cursor          = true,
                      .disable_auto_wrap    = true,
                      .bracketed_paste      = true,
                      .focus_reporting      = true,
                      .mouse                = msg::ScreenMouseTracking::DRAG,
                      .kitty_keyboard_flags = static_cast<std::uint32_t>(
                          puc::terminal::KeyboardEnhancement::
                              DISAMBIGUATE_ESCAPE_CODES),
                  },
          },
      .selection =
          puc::app::ApplicationSubsystemSelection{
              .metronome         = true,
              .presentation      = true,
              .commands          = false,
              .input             = false,
              .command_mode      = false,
              .embedded_terminal = false,
          },
  };
  puc::app::AppState app;
  puc::app::Status app_status = puc::app::register_application_subsystems(
      app, std::move(subsystem_options));
  auto runtime =
      std::make_unique<TerminalTestRuntimeSubsystem>(options.selected_test);
  TerminalTestRuntimeSubsystem* runtime_view = runtime.get();
  if (puc::app::is_ok(app_status)) {
    app_status = app.register_subsystem(std::move(runtime));
  }
  if (!puc::app::is_ok(app_status)) {
    std::cerr << "Could not register terminal-test subsystems: "
              << puc::app::status_message(app_status) << '\n';
    return 1;
  }
  app_status = app.initialize(puc::app::OperatingMode::TUI);
  if (puc::app::is_ok(app_status)) {
    app_status = app.start();
  }
  if (!puc::app::is_ok(app_status)) {
    std::cerr << "Could not start terminal-test subsystems: "
              << puc::app::status_message(app_status) << '\n';
    static_cast<void>(app.terminate());
    return 1;
  }

  bool healthy = true;
  while (healthy && stop_requested == 0 && !runtime_view->finished()) {
    healthy = runtime_view->draw();
  }

  const puc::app::Status stop_status      = app.stop();
  const std::size_t passed                = runtime_view->print_report();
  const std::size_t completed             = runtime_view->completed_count();
  const std::size_t planned               = runtime_view->planned_count();
  const puc::app::Status terminate_status = app.terminate();
  return healthy && puc::app::is_ok(stop_status) &&
                 puc::app::is_ok(terminate_status) && completed == planned &&
                 passed == planned
             ? 0
             : 1;
}
