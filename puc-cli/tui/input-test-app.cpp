/**
 * @file input-test-app.cpp
 * @brief Interactive manual test application for InputFrame.
 *
 * This executable gives InputFrame the same real terminal pipeline validated by
 * the terminal conformance app: Screen owns the alternate terminal buffer,
 * Decoder loads terminfo and layered TOML mappings, and normalized keyboard,
 * paste, scroll, and mouse events are routed into the editor. The input remains
 * anchored to the bottom edge and grows with wrapped content up to its
 * screen-relative maximum.
 *
 * Run from a real terminal with:
 *
 *     bazel run //puc-cli/tui:input-test-app
 *
 * The animation below demonstrates normal text input and line numbering,
 * shared selection behavior, command mode, and the integrated terminal:
 *
 * ![The PUC input frame demonstrating normal, command, and terminal
 * modes.](../assets/puc-cli-input-frame-demo.gif)
 *
 * Ctrl-C restores the terminal and exits. `PUC_CONFIG_ROOT` and
 * `PUC_USER_CONFIG_ROOT` have the same meaning as in terminal-test.
 * `PUC_TEST_SHELL` may replace the embedded `/bin/sh` used for terminal mode.
 */

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "msgs/screen_msgs.hpp"
#include "puc-cli/state/bootstrap.hpp"
#include "puc-cli/state/command_mode.hpp"
#include "puc-cli/state/configuration.hpp"
#include "puc-cli/state/embedded_terminal.hpp"
#include "puc-cli/state/input.hpp"
#include "puc-cli/state/presentation.hpp"
#include "puc-cli/state/screen.hpp"
#include "puc-cli/state/terminal.hpp"
#include "puc-cli/terminal/decoder.hpp"
#include "puc-cli/terminal/timeouts.hpp"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/input_frame.hpp"
#include "puc-cli/tui/layout.hpp"
#include "puc-cli/tui/renderer.hpp"
#include "puc-cli/tui/screen.hpp"
#include "puc-cli/tui/theme.hpp"
#include "utils/config/config.hpp"
#include "utils/logger/logger.hpp"

/** @cond INPUT_TEST_APP_LOGGER_MODULE */
LOGGER_MODULE("Input Test App");
/** @endcond */

namespace {

namespace msg = puc::msg;

using puc::tui::Canvas;
using puc::tui::CellDimensions;
using puc::tui::Frame;
using puc::tui::InputFrame;
using puc::tui::Layout;
using puc::tui::ParallelRenderer;
using puc::tui::Screen;
using puc::tui::Status;
using puc::tui::Theme;

/** Worker budget shared by rendering and asynchronous terminal commands. */
constexpr std::uint8_t kWorkerCount = 4U;
/** Approximate presentation cadence; terminal input is polled once per frame.
 */
constexpr std::chrono::milliseconds kFrameDelay{16};
/** Stable layout id used when keyboard selection targets the editor. */
constexpr std::string_view kInputFrameId = "input";
/** Message shown when InputFrame's minimum geometry is unavailable. */
constexpr std::string_view kScreenTooSmall =
    "Input frame needs at least 40 columns (terminal mode needs 6 rows)";
/** Persistent reminder rendered in InputFrame's notification row. */
constexpr std::string_view kNotification =
    "Enter reserved | Shift+Enter newline | Esc Esc clear | Esc+: command | "
    "Esc+> terminal | mouse selects | Ctrl+C quits";
/** Molokai's `#1B1D1E` background shifted gently toward blue-gray. */
constexpr std::uint32_t kTintedMolokaiBackground = 0x1b2026U;
/** Enhanced keys needed to distinguish and preserve macOS Command chords. */
constexpr std::uint32_t kKeyboardEnhancements =
    static_cast<std::uint32_t>(
        puc::terminal::KeyboardEnhancement::DISAMBIGUATE_ESCAPE_CODES) |
    static_cast<std::uint32_t>(
        puc::terminal::KeyboardEnhancement::REPORT_ALTERNATE_KEYS) |
    static_cast<std::uint32_t>(
        puc::terminal::KeyboardEnhancement::REPORT_ALL_KEYS) |
    static_cast<std::uint32_t>(
        puc::terminal::KeyboardEnhancement::REPORT_ASSOCIATED_TEXT);

/** Async-signal-safe shutdown request set by SIGINT or SIGTERM. */
volatile std::sig_atomic_t stop_requested = 0;

/** Request orderly terminal restoration without doing work in a signal. */
void request_stop(int signal_number) noexcept {
  static_cast<void>(signal_number);
  stop_requested = 1;
}

/** Return whether enhanced keyboard input encoded the Ctrl-C exit gesture. */
bool requests_exit(const puc::terminal::Event& event) noexcept {
  const auto* key = std::get_if<puc::terminal::KeyEvent>(&event);
  if (key == nullptr || key->action == puc::terminal::KeyAction::RELEASE ||
      !key->modifiers.contains(puc::terminal::Modifier::CONTROL)) {
    return false;
  }
  const auto* character = std::get_if<char32_t>(&key->key.value);
  return character != nullptr && (*character == U'c' || *character == U'C');
}

/** Return one environment variable without retaining its raw pointer. */
std::string environment_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

/** Return whether a directory contains all packaged terminal configuration. */
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

/** Locate the installed or Bazel-runfiles primary configuration root. */
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

/** Return the optional user configuration root for this manual run. */
std::filesystem::path user_config_root(
    const std::filesystem::path& primary_root) {
  const std::string configured = environment_value("PUC_USER_CONFIG_ROOT");
  return configured.empty() ? primary_root / ".puc-no-user-overrides"
                            : std::filesystem::path{configured};
}

/** Construct a complete terminal cell. */
Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Adapt a row grid to Canvas's span-based write interface. */
Status write_grid(Canvas& canvas, const Canvas::Rect& rect,
                  std::vector<std::vector<Canvas::Cell>>& cells) {
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
}

/** Center a clipped warning when the terminal cannot fit InputFrame. */
class SmallScreenFrame final : public Frame {
 public:
  /** Construct the fallback frame. */
  SmallScreenFrame() : Frame("screen-too-small") {}

  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override {
    if (rect.width == 0U || rect.height == 0U) {
      return Status::OK;
    }

    const Theme::Colors colors = theme.get_colors();
    const std::size_t count    = std::min(rect.width, kScreenTooSmall.size());
    std::vector<std::vector<Canvas::Cell>> cells(
        1U, std::vector<Canvas::Cell>(
                count, cell(U' ', colors.text_warning, colors.background)));
    for (std::size_t index = 0U; index < count; ++index) {
      cells.front()[index] =
          cell(static_cast<unsigned char>(kScreenTooSmall[index]),
               colors.text_warning, colors.background);
    }
    return write_grid(canvas,
                      Canvas::Rect{
                          .x      = rect.x + (rect.width - count) / 2U,
                          .y      = rect.y + rect.height / 2U,
                          .width  = count,
                          .height = 1U,
                      },
                      cells);
  }
};

/** Add one frame and its ordered constraints to a layout description. */
Status add_frame(Layout& layout,
                 const std::shared_ptr<Layout::LayoutDescription>& description,
                 std::string id, std::shared_ptr<Frame> frame,
                 std::initializer_list<Layout::Constraint> constraints) {
  Status status =
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
  return Status::OK;
}

/** Own one interactive run while borrowing AppState-managed mechanisms. */
class InputTestApplication {
 public:
  /** Retain running subsystem mechanisms and timeout configuration. */
  InputTestApplication(puc::terminal::Decoder& decoder,
                       std::shared_ptr<InputFrame> input_frame, Screen& screen,
                       ParallelRenderer& renderer,
                       puc::app::CommandModeSubsystem& command_mode,
                       puc::app::EmbeddedTerminalSubsystem& embedded_terminal,
                       const puc::config::Config& configuration)
      : decoder_(decoder),
        configuration_(configuration),
        input_frame_(std::move(input_frame)),
        screen_(&screen),
        renderer_(&renderer),
        command_mode_(&command_mode),
        embedded_terminal_(&embedded_terminal) {}

  InputTestApplication(const InputTestApplication&)            = delete;
  InputTestApplication& operator=(const InputTestApplication&) = delete;

  /** Restore terminal resources if the caller exits an error path. */
  ~InputTestApplication() { static_cast<void>(shutdown()); }

  /** Configure timeouts, rendering, and the first resolved layout. */
  bool setup() {
    const puc::terminal::Status timeout_status =
        puc::terminal::load_timeout_settings(configuration_, timeout_settings_);
    if (!puc::terminal::is_ok(timeout_status)) {
      Logger<ERROR> << "Terminal timeout setup failed: "
                    << puc::terminal::status_message(timeout_status);
      return false;
    }

    configure_theme();
    if (input_frame_ == nullptr || screen_ == nullptr || renderer_ == nullptr ||
        command_mode_ == nullptr || embedded_terminal_ == nullptr) {
      Logger<ERROR> << "Input application mechanisms are unavailable";
      return false;
    }
    if (input_frame_->snapshot().notification.empty()) {
      input_frame_->set_notification(std::string{kNotification});
    }
    Status status = Status::OK;
    const auto geometry_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (true) {
      status = screen_->get_dimensions(width_, height_, cell_dimensions_);
      if (puc::tui::is_ok(status)) {
        break;
      }
      if (status != Status::TERMINAL_QUERY_FAILED ||
          std::chrono::steady_clock::now() >= geometry_deadline) {
        Logger<ERROR> << "Could not observe terminal geometry: "
                      << puc::tui::status_message(status);
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    return attach_canvas(width_, height_) && setup_small_layout() &&
           update_layout();
  }

  /** Poll input, resize the editor, and present one complete frame. */
  bool draw() {
    std::size_t current_width  = 0U;
    std::size_t current_height = 0U;
    CellDimensions current_cells;
    Status status =
        screen_->get_dimensions(current_width, current_height, current_cells);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not refresh terminal geometry: "
                    << puc::tui::status_message(status);
      return false;
    }

    const bool dimensions_changed =
        current_width != width_ || current_height != height_;
    const bool cells_changed = current_cells != cell_dimensions_;
    if (dimensions_changed || cells_changed) {
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

    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!poll_input()) {
      return false;
    }
    input_frame_->advance_time(InputFrame::Clock::now());
    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!update_layout()) {
      return false;
    }
    if (!synchronize_terminal_process()) {
      return false;
    }
    screen_too_small_ = width_ < InputFrame::kMinimumWidth ||
                        height_ < input_frame_->minimum_height();
    if (!update_layout()) {
      return false;
    }

    const auto& description =
        screen_too_small_ ? small_layout_description_ : layout_description_;
    const Layout::AbsoluteLayout& absolute =
        screen_too_small_ ? small_absolute_layout_ : absolute_layout_;
    const Theme::Colors colors = theme_.get_colors();
    status = renderer_->start(description, absolute, theme_, canvas_,
                              cell(U' ', colors.text, colors.background));
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not schedule input frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = renderer_->wait();
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Input frame rendering failed: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = screen_->draw();
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not present input frame: "
                    << puc::tui::status_message(status);
      return false;
    }

    std::this_thread::sleep_for(kFrameDelay);
    return true;
  }

  /** Release generation bindings before their owning subsystems stop. */
  bool shutdown() noexcept {
    bool quiesced = true;
    if (renderer_ != nullptr) {
      quiesced = puc::tui::is_ok(renderer_->wait());
    }
    input_frame_.reset();
    screen_            = nullptr;
    renderer_          = nullptr;
    command_mode_      = nullptr;
    embedded_terminal_ = nullptr;
    return quiesced;
  }

 private:
  /** App-owned scheduling data for one explicit state-machine timeout. */
  struct PendingTimeout {
    puc::terminal::TimeoutInput input; /**< Generation to deliver. */
    std::chrono::steady_clock::time_point deadline; /**< Delivery time. */
  };

  /** Install a Molokai-derived palette with a restrained blue-gray tint. */
  void configure_theme() {
    Theme::Colors colors{};
    colors.primary              = 0x66d9efU;
    colors.tertiary             = 0xae81ffU;
    colors.secondary            = 0x252c34U;
    colors.highlight_background = 0x3b5266U;
    colors.highlight_text       = 0xf8f8f2U;
    colors.text                 = 0xf8f8f2U;
    colors.text_secondary       = 0xe8e8e2U;
    colors.text_muted           = 0x7e8e91U;
    colors.text_error           = 0xf92672U;
    colors.text_warning         = 0xfd971fU;
    colors.text_success         = 0xa6e22eU;
    colors.text_info            = 0x66d9efU;
    colors.text_emphasis        = 0xffffffU;
    colors.background           = kTintedMolokaiBackground;
    theme_.load_colors(colors);
  }

  /** Attach a fresh Canvas transaction pair after a terminal resize. */
  bool attach_canvas(std::size_t width, std::size_t height) {
    auto next = std::make_shared<Canvas>(width, height);
    if (!puc::tui::is_ok(next->get_status())) {
      Logger<ERROR> << "Could not allocate Canvas: "
                    << puc::tui::status_message(next->get_status());
      return false;
    }
    const Status status = screen_->set_canvas(next);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not attach Canvas: "
                    << puc::tui::status_message(status);
      return false;
    }
    canvas_ = std::move(next);
    return true;
  }

  /** Construct the full-screen fallback shown below minimum dimensions. */
  bool setup_small_layout() {
    small_layout_description_ =
        layout_.make_layout_description("input-frame-too-small");
    const Status status =
        add_frame(layout_, small_layout_description_, "screen-too-small",
                  std::make_shared<SmallScreenFrame>(), {});
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not add small-screen frame: "
                    << puc::tui::status_message(status);
      return false;
    }
    return true;
  }

  /** Rebuild the exact-height bottom layout when wrapped content grows. */
  bool rebuild_input_layout(std::size_t input_height) {
    auto next = layout_.make_layout_description("input-frame-manual-test");
    const Status status = add_frame(
        layout_, next, std::string{kInputFrameId}, input_frame_,
        {
            Layout::make_character_constraint(Layout::ConstraintType::MIN_WIDTH,
                                              InputFrame::kMinimumWidth),
            Layout::make_character_constraint(
                Layout::ConstraintType::MIN_HEIGHT, input_height),
            Layout::make_character_constraint(
                Layout::ConstraintType::MAX_HEIGHT, input_height),
            Layout::make_character_constraint(
                Layout::ConstraintType::LEFT_ANCHOR, 0U),
            Layout::make_character_constraint(
                Layout::ConstraintType::BOTTOM_ANCHOR, 0U),
        });
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not construct input layout: "
                    << puc::tui::status_message(status);
      return false;
    }
    layout_description_ = std::move(next);
    input_height_       = input_height;
    return true;
  }

  /** Recompute bottom anchoring and the small-screen fallback rectangles. */
  bool update_layout() {
    const std::size_t minimum_height = input_frame_->minimum_height();
    const bool can_fit =
        width_ >= InputFrame::kMinimumWidth && height_ >= minimum_height;
    const std::size_t desired_height =
        can_fit ? input_frame_->preferred_height(width_, height_)
                : minimum_height;
    if (layout_description_ == nullptr || desired_height != input_height_) {
      if (!rebuild_input_layout(desired_height)) {
        return false;
      }
    }

    Status status =
        layout_.compute_absolute_layout(layout_description_, width_, height_,
                                        cell_dimensions_, absolute_layout_);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not resolve input layout: "
                    << puc::tui::status_message(status);
      return false;
    }
    status = layout_.compute_absolute_layout(small_layout_description_, width_,
                                             height_, cell_dimensions_,
                                             small_absolute_layout_);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not resolve small-screen layout: "
                    << puc::tui::status_message(status);
      return false;
    }
    return true;
  }

  /** Start, resize, pump, or reap the PTY requested by InputFrame state. */
  bool synchronize_terminal_process() {
    return embedded_terminal_ != nullptr &&
           puc::app::is_ok(embedded_terminal_->synchronize(width_, height_));
  }

  /** Read available bytes and route every normalized event into the editor. */
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
    for (const puc::terminal::Event& event : events) {
      if (requests_exit(event)) {
        stop_requested = 1;
      } else if (!screen_too_small_) {
        if (!route_event(event)) {
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

  /** Route mouse/copy integration before giving the event to InputFrame. */
  bool route_event(const puc::terminal::Event& event) {
    const puc::tui::InputMode mode = input_frame_->snapshot().mode;
    if (mode == puc::tui::InputMode::TERMINAL) {
      const auto* command = std::get_if<puc::terminal::CommandEvent>(&event);
      const bool frame_command =
          command != nullptr &&
          (command->command == puc::terminal::Command::ENTER_COMMAND_MODE ||
           command->command == puc::terminal::Command::ENTER_TERMINAL_MODE);
      const bool selection_command =
          command != nullptr &&
          (command->command == puc::terminal::Command::COPY ||
           command->command == puc::terminal::Command::SELECT_ALL);
      if (!frame_command && !selection_command &&
          !std::holds_alternative<puc::terminal::MouseEvent>(event) &&
          !std::holds_alternative<puc::terminal::ScrollEvent>(event)) {
        return embedded_terminal_ != nullptr &&
               puc::app::is_ok(embedded_terminal_->send_event(event));
      }
    }

    if (const auto* mouse = std::get_if<puc::terminal::MouseEvent>(&event)) {
      const Status status =
          screen_->handle_mouse_event(*mouse, layout_description_->z_buffer,
                                      absolute_layout_.frame_layouts);
      if (!puc::tui::is_ok(status) && status != Status::NO_SELECTION) {
        Logger<ERROR> << "Could not route mouse event: "
                      << puc::tui::status_message(status);
        return false;
      }
    }

    if (const auto* command =
            std::get_if<puc::terminal::CommandEvent>(&event)) {
      if (command->command == puc::terminal::Command::COPY) {
        const Status status = screen_->copy_selection();
        if (!puc::tui::is_ok(status) && status != Status::NO_SELECTION &&
            status != Status::FRAME_NOT_SELECTABLE) {
          Logger<ERROR> << "Could not copy selected input: "
                        << puc::tui::status_message(status);
          return false;
        }
        return true;
      }
      if (command->command == puc::terminal::Command::SELECT_ALL) {
        const Status status = screen_->select_all(kInputFrameId, input_frame_);
        if (!puc::tui::is_ok(status) && status != Status::NO_SELECTION &&
            status != Status::FRAME_NOT_SELECTABLE) {
          Logger<ERROR> << "Could not select all input: "
                        << puc::tui::status_message(status);
          return false;
        }
        return true;
      }
    }

    const Status status =
        command_mode_ == nullptr
            ? Status::INVALID_ARGUMENT
            : command_mode_->handle_event(event, InputFrame::Clock::now());
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not apply input event: "
                    << puc::tui::status_message(status);
      return false;
    }
    return true;
  }

  /** Synchronize the app-owned deadline with Decoder's timeout token. */
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

  /** Synchronize the app-owned deadline with Screen's click timeout token. */
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

  /** Deliver due decoder and multi-click timeout generations. */
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
      for (const puc::terminal::Event& event : events) {
        if (requests_exit(event)) {
          stop_requested = 1;
        } else if (!screen_too_small_) {
          if (!route_event(event)) {
            return false;
          }
        }
      }
      pending_decoder_timeout_.reset();
      refresh_decoder_timeout(now);
    }

    if (pending_selection_timeout_.has_value() &&
        now >= pending_selection_timeout_->deadline) {
      const Status status =
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

  puc::terminal::Decoder&
      decoder_; /**< AppState-owned configured input Trie. */
  const puc::config::Config& configuration_; /**< Timeout configuration. */
  puc::terminal::TimeoutSettings timeout_settings_; /**< Input timing policy. */
  std::shared_ptr<InputFrame> input_frame_; /**< Editor under manual test. */
  Screen* screen_ = nullptr;                /**< AppState-owned presenter. */
  ParallelRenderer* renderer_ =
      nullptr; /**< Lifecycle-owned frame scheduler. */
  puc::app::CommandModeSubsystem* command_mode_ =
      nullptr; /**< Lifecycle-owned command coordinator. */
  puc::app::EmbeddedTerminalSubsystem* embedded_terminal_ =
      nullptr;                     /**< Lifecycle-owned PTY child/pump. */
  std::shared_ptr<Canvas> canvas_; /**< Current screen Canvas. */
  std::shared_ptr<Layout::LayoutDescription>
      layout_description_; /**< Bottom-anchored editor layout. */
  std::shared_ptr<Layout::LayoutDescription>
      small_layout_description_;           /**< Minimum-size fallback layout. */
  Layout::AbsoluteLayout absolute_layout_; /**< Current editor rectangle. */
  Layout::AbsoluteLayout
      small_absolute_layout_; /**< Current fallback rectangle. */
  Layout layout_;             /**< Constraint solver. */
  Theme theme_;               /**< Manual editor palette. */
  CellDimensions cell_dimensions_ =
      puc::tui::kDefaultCellDimensions; /**< Terminal cell proportions. */
  std::size_t width_        = 0U;       /**< Latest terminal columns. */
  std::size_t height_       = 0U;       /**< Latest terminal rows. */
  std::size_t input_height_ = 0U;    /**< Last content-driven editor height. */
  bool screen_too_small_    = false; /**< Whether the fallback is active. */
  std::optional<PendingTimeout>
      pending_decoder_timeout_; /**< Current decoder timeout. */
  std::optional<PendingTimeout>
      pending_selection_timeout_; /**< Current click-chain timeout. */
};

/** Bind the input manual-test loop to restartable shared subsystem generations.
 */
class InputTestRuntimeSubsystem final : public puc::app::AppSubsystem {
 public:
  /** Declare every shared mechanism consumed by one interactive generation. */
  InputTestRuntimeSubsystem()
      : AppSubsystem(
            "input-test-runtime",
            puc::app::subsystem_dependencies<
                puc::app::ConfigurationSubsystem, puc::app::TerminalSubsystem,
                puc::app::ScreenSubsystem, puc::app::PresentationSubsystem,
                puc::app::InputSubsystem, puc::app::CommandModeSubsystem,
                puc::app::EmbeddedTerminalSubsystem>()) {}

  /** The durable mechanisms are already constructed by their owning adapters.
   */
  puc::app::Status initialize(puc::app::AppState& app) override {
    static_cast<void>(app);
    return puc::app::Status::OK;
  }

  /** Bind the current running generation and construct its layouts and Canvas.
   */
  puc::app::Status start(puc::app::AppState& app) override {
    auto* configuration = app.get_subsystem<puc::app::ConfigurationSubsystem>();
    auto* terminal      = app.get_subsystem<puc::app::TerminalSubsystem>();
    auto* screen        = app.get_subsystem<puc::app::ScreenSubsystem>();
    auto* presentation  = app.get_subsystem<puc::app::PresentationSubsystem>();
    auto* input         = app.get_subsystem<puc::app::InputSubsystem>();
    auto* command_mode  = app.get_subsystem<puc::app::CommandModeSubsystem>();
    auto* embedded = app.get_subsystem<puc::app::EmbeddedTerminalSubsystem>();
    if (configuration == nullptr || configuration->configuration() == nullptr ||
        terminal == nullptr || terminal->decoder() == nullptr ||
        screen == nullptr || screen->screen() == nullptr ||
        presentation == nullptr || presentation->renderer() == nullptr ||
        input == nullptr || input->input_frame() == nullptr ||
        command_mode == nullptr || embedded == nullptr) {
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }

    auto next = std::make_unique<InputTestApplication>(
        *terminal->decoder(), input->input_frame(), *screen->screen(),
        *presentation->renderer(), *command_mode, *embedded,
        *configuration->configuration());
    if (!next->setup()) {
      static_cast<void>(next->shutdown());
      return puc::app::Status::SUBSYSTEM_FAILURE;
    }
    application_ = std::move(next);
    return puc::app::Status::OK;
  }

  /** Quiesce app-owned rendering before its shared dependencies stop. */
  puc::app::Status stop(puc::app::AppState& app) noexcept override {
    static_cast<void>(app);
    if (application_ == nullptr) {
      return puc::app::Status::OK;
    }
    const bool stopped = application_->shutdown();
    application_.reset();
    return stopped ? puc::app::Status::OK : puc::app::Status::SUBSYSTEM_FAILURE;
  }

  /** Release any generation retained after partial lifecycle progress. */
  puc::app::Status terminate(puc::app::AppState& app) noexcept override {
    return stop(app);
  }

  /** Draw one frame through the currently running generation. */
  bool draw() { return application_ != nullptr && application_->draw(); }

 private:
  std::unique_ptr<InputTestApplication>
      application_; /**< Runtime state for the current start/stop generation. */
};

}  // namespace

/**
 * Run the standalone InputFrame manual test until interrupted.
 *
 * @param[in] argc Conventional process argument count.
 * @param[in] argv Conventional argument vector; argv[0] locates runfiles.
 * @return Zero after successful setup, rendering, and terminal restoration.
 */
int main(int argc, char** argv) {
  const puc::logger::LoggerConf logger_config{
      .global_level = puc::logger::LogLevel::WARN,
  };

  if (std::setlocale(LC_CTYPE, "") == nullptr) {
    std::fprintf(stderr,
                 "Could not activate the environment's character encoding; "
                 "embedded terminal output may replace non-ASCII text\n");
  }

  if (std::signal(SIGINT, request_stop) == SIG_ERR ||
      std::signal(SIGTERM, request_stop) == SIG_ERR) {
    std::fprintf(stderr, "Could not install termination signal handlers\n");
    return 1;
  }

  const std::string_view executable   = argc > 0 && argv[0] != nullptr
                                            ? std::string_view{argv[0]}
                                            : std::string_view{};
  const std::filesystem::path primary = primary_config_root(executable);
  const std::filesystem::path user    = user_config_root(primary);
  const std::string configured_shell  = environment_value("PUC_TEST_SHELL");
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
                      .kitty_keyboard_flags = kKeyboardEnhancements,
                  },
          },
      .embedded_terminal =
          puc::app::EmbeddedTerminalSubsystemOptions{
              .shell = configured_shell.empty() ? "/bin/sh" : configured_shell,
          },
  };
  puc::app::AppState state;
  puc::app::Status app_status = puc::app::register_application_subsystems(
      state, std::move(subsystem_options));
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not register application subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    return 1;
  }
  auto runtime = std::make_unique<InputTestRuntimeSubsystem>();
  InputTestRuntimeSubsystem* runtime_view = runtime.get();
  app_status = state.register_subsystem(std::move(runtime));
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not register input runtime: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    return 1;
  }
  app_status = state.initialize(puc::app::OperatingMode::TUI);
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not initialize application subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    static_cast<void>(state.terminate());
    return 1;
  }
  app_status = state.start();
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not start application subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    static_cast<void>(state.terminate());
    return 1;
  }

  bool success = true;
  while (success && stop_requested == 0) {
    success = runtime_view->draw();
  }
  app_status = state.stop();
  if (!puc::app::is_ok(app_status)) {
    success = false;
  }
  app_status = state.terminate();
  if (!puc::app::is_ok(app_status)) {
    const std::string_view message = puc::app::status_message(app_status);
    std::fprintf(stderr, "Could not terminate application subsystems: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    success = false;
  }
  return success ? 0 : 1;
}
