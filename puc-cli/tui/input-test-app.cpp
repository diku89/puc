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
 * Ctrl-C restores the terminal and exits. `PUC_CONFIG_ROOT` and
 * `PUC_USER_CONFIG_ROOT` have the same meaning as in terminal-test.
 * `PUC_TEST_SHELL` may replace the embedded `/bin/sh` used for terminal mode.
 */

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "msgs/screen_msgs.hpp"
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
#include "utils/multithreading/job_queue.hpp"

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

/** Append one Unicode scalar as UTF-8 terminal input. */
void append_utf8(char32_t character, std::string& output) {
  const std::uint32_t codepoint = static_cast<std::uint32_t>(character);
  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  }
}

/** Return whether an enhanced key event describes only modifier state. */
bool is_modifier_key(puc::terminal::NamedKey key) noexcept {
  switch (key) {
    case puc::terminal::NamedKey::LEFT_SHIFT:
    case puc::terminal::NamedKey::LEFT_CONTROL:
    case puc::terminal::NamedKey::LEFT_ALT:
    case puc::terminal::NamedKey::LEFT_SUPER:
    case puc::terminal::NamedKey::LEFT_HYPER:
    case puc::terminal::NamedKey::LEFT_META:
    case puc::terminal::NamedKey::RIGHT_SHIFT:
    case puc::terminal::NamedKey::RIGHT_CONTROL:
    case puc::terminal::NamedKey::RIGHT_ALT:
    case puc::terminal::NamedKey::RIGHT_SUPER:
    case puc::terminal::NamedKey::RIGHT_HYPER:
    case puc::terminal::NamedKey::RIGHT_META:
    case puc::terminal::NamedKey::ISO_LEVEL3_SHIFT:
    case puc::terminal::NamedKey::ISO_LEVEL5_SHIFT:
      return true;
    default:
      return false;
  }
}

/** Encode an xterm cursor key, retaining modifiers useful to child programs. */
void append_cursor_key(char final, puc::terminal::Modifiers modifiers,
                       std::string& output) {
  unsigned int parameter = 1U;
  parameter += modifiers.contains(puc::terminal::Modifier::SHIFT) ? 1U : 0U;
  parameter += modifiers.contains(puc::terminal::Modifier::ALT) ? 2U : 0U;
  parameter += modifiers.contains(puc::terminal::Modifier::CONTROL) ? 4U : 0U;
  parameter += modifiers.contains(puc::terminal::Modifier::SUPER) ? 8U : 0U;
  parameter += modifiers.contains(puc::terminal::Modifier::HYPER) ? 16U : 0U;
  parameter += modifiers.contains(puc::terminal::Modifier::META) ? 32U : 0U;
  output.append("\x1b[");
  if (parameter != 1U) {
    output.append("1;");
    output.append(std::to_string(parameter));
  }
  output.push_back(final);
}

/** Convert one decoded key press back to conventional PTY input bytes. */
void append_terminal_key(const puc::terminal::KeyEvent& event,
                         std::string& output) {
  if (event.action == puc::terminal::KeyAction::RELEASE) {
    return;
  }
  if (const auto* named =
          std::get_if<puc::terminal::NamedKey>(&event.key.value)) {
    if (is_modifier_key(*named)) {
      return;
    }
    switch (*named) {
      case puc::terminal::NamedKey::ESCAPE:
        output.push_back('\x1b');
        return;
      case puc::terminal::NamedKey::ENTER:
      case puc::terminal::NamedKey::KEYPAD_ENTER:
        output.push_back('\r');
        return;
      case puc::terminal::NamedKey::TAB:
        output.append(event.modifiers.contains(puc::terminal::Modifier::SHIFT)
                          ? "\x1b[Z"
                          : "\t");
        return;
      case puc::terminal::NamedKey::BACKSPACE:
        output.push_back('\x7f');
        return;
      case puc::terminal::NamedKey::UP:
      case puc::terminal::NamedKey::KEYPAD_UP:
        append_cursor_key('A', event.modifiers, output);
        return;
      case puc::terminal::NamedKey::DOWN:
      case puc::terminal::NamedKey::KEYPAD_DOWN:
        append_cursor_key('B', event.modifiers, output);
        return;
      case puc::terminal::NamedKey::RIGHT:
      case puc::terminal::NamedKey::KEYPAD_RIGHT:
        append_cursor_key('C', event.modifiers, output);
        return;
      case puc::terminal::NamedKey::LEFT:
      case puc::terminal::NamedKey::KEYPAD_LEFT:
        append_cursor_key('D', event.modifiers, output);
        return;
      case puc::terminal::NamedKey::HOME:
      case puc::terminal::NamedKey::KEYPAD_HOME:
        output.append("\x1b[H");
        return;
      case puc::terminal::NamedKey::END:
      case puc::terminal::NamedKey::KEYPAD_END:
        output.append("\x1b[F");
        return;
      case puc::terminal::NamedKey::INSERT:
      case puc::terminal::NamedKey::KEYPAD_INSERT:
        output.append("\x1b[2~");
        return;
      case puc::terminal::NamedKey::DELETE_KEY:
      case puc::terminal::NamedKey::KEYPAD_DELETE:
        output.append("\x1b[3~");
        return;
      case puc::terminal::NamedKey::PAGE_UP:
      case puc::terminal::NamedKey::KEYPAD_PAGE_UP:
        output.append("\x1b[5~");
        return;
      case puc::terminal::NamedKey::PAGE_DOWN:
      case puc::terminal::NamedKey::KEYPAD_PAGE_DOWN:
        output.append("\x1b[6~");
        return;
      default:
        return;
    }
  }

  const auto* character = std::get_if<char32_t>(&event.key.value);
  if (character == nullptr) {
    return;
  }
  char32_t value = event.shifted_key.value_or(*character);
  if (event.modifiers.contains(puc::terminal::Modifier::CONTROL)) {
    if (value >= U'a' && value <= U'z') {
      value -= U'a' - U'A';
    }
    if (value >= U'@' && value <= U'_') {
      output.push_back(static_cast<char>(value & 0x1fU));
    } else if (value == U'?') {
      output.push_back('\x7f');
    }
    return;
  }
  if (event.modifiers.contains(puc::terminal::Modifier::ALT)) {
    output.push_back('\x1b');
  }
  if (!event.text.empty()) {
    output.append(event.text);
  } else {
    append_utf8(value, output);
  }
}

/** Convert normalized application events into input for the embedded PTY. */
std::string terminal_input(const puc::terminal::Event& event) {
  std::string output;
  if (const auto* text = std::get_if<puc::terminal::TextEvent>(&event)) {
    output = text->utf8;
  } else if (const auto* key = std::get_if<puc::terminal::KeyEvent>(&event)) {
    append_terminal_key(*key, output);
  } else if (const auto* paste = std::get_if<puc::terminal::PasteEvent>(&event);
             paste != nullptr &&
             paste->phase == puc::terminal::PastePhase::DATA) {
    output = paste->data;
  } else if (const auto* command =
                 std::get_if<puc::terminal::CommandEvent>(&event)) {
    switch (command->command) {
      case puc::terminal::Command::MOVE_WORD_LEFT:
        output.append(
            "\x1b"
            "b");
        break;
      case puc::terminal::Command::MOVE_WORD_RIGHT:
        output.append(
            "\x1b"
            "f");
        break;
      case puc::terminal::Command::MOVE_ROW_START:
        output.push_back('\x01');
        break;
      case puc::terminal::Command::MOVE_ROW_END:
        output.push_back('\x05');
        break;
      case puc::terminal::Command::MOVE_PAGE_UP:
        output.append("\x1b[5~");
        break;
      case puc::terminal::Command::MOVE_PAGE_DOWN:
        output.append("\x1b[6~");
        break;
      case puc::terminal::Command::MOVE_BUFFER_START:
      case puc::terminal::Command::MOVE_BUFFER_END:
      case puc::terminal::Command::COPY:
      case puc::terminal::Command::SELECT_ALL:
      case puc::terminal::Command::ENTER_COMMAND_MODE:
      case puc::terminal::Command::ENTER_TERMINAL_MODE:
        break;
    }
  }
  return output;
}

/** Own the child process and master side of one embedded pseudo-terminal. */
class EmbeddedTerminalProcess {
 public:
  /** Result of one nonblocking output/exit-status pump. */
  enum class PumpResult { RUNNING, EXITED, FAILED };

  EmbeddedTerminalProcess()                                          = default;
  EmbeddedTerminalProcess(const EmbeddedTerminalProcess&)            = delete;
  EmbeddedTerminalProcess& operator=(const EmbeddedTerminalProcess&) = delete;

  /** Terminate and reap only the child process created by this object. */
  ~EmbeddedTerminalProcess() { stop(); }

  /** Start an interactive POSIX shell with the requested terminal geometry. */
  bool start(std::size_t generation, std::size_t columns, std::size_t rows) {
    const bool preserve_pending_input = !running();
    std::string pending_input =
        preserve_pending_input ? std::move(pending_input_) : std::string{};
    stop();
    pending_input_ = std::move(pending_input);
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(
        columns, std::numeric_limits<unsigned short>::max()));
    size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(
        rows, std::numeric_limits<unsigned short>::max()));

    const std::string configured_shell = environment_value("PUC_TEST_SHELL");
    const std::string shell =
        configured_shell.empty() ? "/bin/sh" : configured_shell;
    int master_fd     = -1;
    const pid_t child = ::forkpty(&master_fd, nullptr, nullptr, &size);
    if (child < 0) {
      Logger<ERROR> << "Could not create embedded terminal process";
      return false;
    }
    if (child == 0) {
      char* const arguments[]{const_cast<char*>(shell.c_str()),
                              const_cast<char*>("-i"), nullptr};
      ::execv(shell.c_str(), arguments);
      ::_exit(127);
    }

    const int flags = ::fcntl(master_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(master_fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
        ::fcntl(master_fd, F_SETFD, FD_CLOEXEC) != 0) {
      static_cast<void>(::close(master_fd));
      static_cast<void>(::kill(child, SIGKILL));
      static_cast<void>(::waitpid(child, nullptr, 0));
      Logger<ERROR> << "Could not configure embedded terminal transport";
      return false;
    }

    master_fd_  = master_fd;
    child_pid_  = child;
    generation_ = generation;
    return flush_input();
  }

  /** Change the child terminal geometry and let the kernel deliver SIGWINCH. */
  bool resize(std::size_t columns, std::size_t rows) const noexcept {
    if (!running()) {
      return true;
    }
    struct winsize size {};
    size.ws_col = static_cast<unsigned short>(std::min<std::size_t>(
        columns, std::numeric_limits<unsigned short>::max()));
    size.ws_row = static_cast<unsigned short>(std::min<std::size_t>(
        rows, std::numeric_limits<unsigned short>::max()));
    return ::ioctl(master_fd_, TIOCSWINSZ, &size) == 0;
  }

  /** Queue bytes for the child and write as much as nonblocking I/O permits. */
  bool send(std::string_view input) {
    pending_input_.append(input);
    return !running() || flush_input();
  }

  /** Read available output into libtmt and observe natural child termination.
   */
  PumpResult pump(InputFrame& frame) {
    if (!running()) {
      return PumpResult::EXITED;
    }
    char bytes[8192];
    while (true) {
      const ssize_t count = ::read(master_fd_, bytes, sizeof(bytes));
      if (count > 0) {
        if (!puc::tui::is_ok(frame.write_terminal(
                std::string_view{bytes, static_cast<std::size_t>(count)}))) {
          return PumpResult::FAILED;
        }
        continue;
      }
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0 &&
          (errno == EAGAIN || errno == EWOULDBLOCK || errno == EIO)) {
        break;
      }
      if (count < 0) {
        Logger<ERROR> << "Could not read embedded terminal output";
        return PumpResult::FAILED;
      }
      break;
    }
    if (!flush_input()) {
      return PumpResult::FAILED;
    }

    int child_status        = 0;
    const pid_t wait_result = ::waitpid(child_pid_, &child_status, WNOHANG);
    if (wait_result == 0) {
      return PumpResult::RUNNING;
    }
    if (wait_result < 0 && errno == EINTR) {
      return PumpResult::RUNNING;
    }
    if (wait_result < 0) {
      Logger<ERROR> << "Could not observe embedded terminal child";
      return PumpResult::FAILED;
    }
    finish_child();
    return PumpResult::EXITED;
  }

  /** Close the PTY, terminate the owned child, and synchronously reap it. */
  void stop() noexcept {
    if (master_fd_ >= 0) {
      static_cast<void>(::close(master_fd_));
      master_fd_ = -1;
    }
    if (child_pid_ > 0) {
      static_cast<void>(::kill(child_pid_, SIGHUP));
      static_cast<void>(::kill(child_pid_, SIGKILL));
      while (::waitpid(child_pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
      child_pid_ = -1;
    }
    generation_ = 0U;
    pending_input_.clear();
  }

  bool running() const noexcept { return master_fd_ >= 0 && child_pid_ > 0; }
  std::size_t generation() const noexcept { return generation_; }

 private:
  /** Flush the pending-input prefix while retaining an EAGAIN suffix. */
  bool flush_input() {
    while (running() && !pending_input_.empty()) {
      const ssize_t count =
          ::write(master_fd_, pending_input_.data(), pending_input_.size());
      if (count > 0) {
        pending_input_.erase(0U, static_cast<std::size_t>(count));
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
      } else {
        Logger<ERROR> << "Could not write embedded terminal input";
        return false;
      }
    }
    return true;
  }

  /** Release descriptors after waitpid has already reaped the child. */
  void finish_child() noexcept {
    if (master_fd_ >= 0) {
      static_cast<void>(::close(master_fd_));
    }
    master_fd_  = -1;
    child_pid_  = -1;
    generation_ = 0U;
    pending_input_.clear();
  }

  int master_fd_          = -1; /**< Nonblocking PTY master descriptor. */
  pid_t child_pid_        = -1; /**< Shell process awaiting waitpid. */
  std::size_t generation_ = 0U; /**< InputFrame session being served. */
  std::string pending_input_;   /**< Unwritten nonblocking PTY input. */
};

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

/** Own one interactive input-frame run while borrowing main's worker pool. */
class InputTestApplication {
 public:
  /** Retain the worker pool and executable path used for runfiles lookup. */
  InputTestApplication(puc::multithreading::JobQueue& workers,
                       std::string_view executable)
      : workers_(workers), executable_(executable) {}

  InputTestApplication(const InputTestApplication&)            = delete;
  InputTestApplication& operator=(const InputTestApplication&) = delete;

  /** Restore terminal resources if the caller exits an error path. */
  ~InputTestApplication() { static_cast<void>(shutdown()); }

  /** Configure decoding, take the terminal, and construct the first layout. */
  bool setup() {
    const std::filesystem::path primary = primary_config_root(executable_);
    const puc::config::Config configuration{primary, user_config_root(primary)};
    const puc::terminal::Status decoder_status =
        decoder_.setup(configuration, environment_value("TERM"), STDOUT_FILENO);
    if (!puc::terminal::is_ok(decoder_status)) {
      Logger<ERROR> << "Input decoder setup failed: "
                    << puc::terminal::status_message(decoder_status);
      return false;
    }
    const puc::terminal::Status timeout_status =
        puc::terminal::load_timeout_settings(configuration, timeout_settings_);
    if (!puc::terminal::is_ok(timeout_status)) {
      Logger<ERROR> << "Terminal timeout setup failed: "
                    << puc::terminal::status_message(timeout_status);
      return false;
    }

    configure_theme();
    input_frame_ = std::make_shared<InputFrame>("input");
    input_frame_->set_notification(std::string{kNotification});
    screen_   = std::make_unique<Screen>(workers_);
    renderer_ = std::make_unique<ParallelRenderer>(workers_);

    const msg::ScreenSessionOptions options{
        .preserve_signals     = true,
        .alternate_screen     = true,
        .hide_cursor          = true,
        .disable_auto_wrap    = true,
        .bracketed_paste      = true,
        .focus_reporting      = true,
        .mouse                = msg::ScreenMouseTracking::DRAG,
        .kitty_keyboard_flags = kKeyboardEnhancements,
    };
    Status status = screen_->take(options);
    if (!puc::tui::is_ok(status)) {
      Logger<ERROR> << "Could not request terminal ownership: "
                    << puc::tui::status_message(status);
      return false;
    }

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

  /** Quiesce rendering and restore every terminal mode requested by setup(). */
  bool shutdown() noexcept {
    renderer_.reset();
    terminal_process_.stop();
    bool released = true;
    if (screen_ != nullptr) {
      const Status status = screen_->release();
      released            = puc::tui::is_ok(status);
      if (!released) {
        Logger<ERROR> << "Could not request terminal restoration: "
                      << puc::tui::status_message(status);
      }
      screen_.reset();
    }
    return released;
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
    const puc::tui::InputFrameSnapshot state = input_frame_->snapshot();
    if (!state.terminal_session_active) {
      terminal_process_.stop();
      return true;
    }

    const bool terminal_fits = width_ >= InputFrame::kMinimumWidth &&
                               height_ >= InputFrame::kTerminalMinimumHeight;
    const std::size_t outer_height = InputFrame::terminal_height(height_);
    const std::size_t columns      = width_ > 5U ? width_ - 5U : 0U;
    const std::size_t rows         = outer_height > 4U ? outer_height - 4U : 0U;
    if ((!terminal_process_.running() ||
         terminal_process_.generation() != state.terminal_generation) &&
        terminal_fits) {
      if (!terminal_process_.start(state.terminal_generation, columns, rows)) {
        input_frame_->close_terminal();
        return false;
      }
    }
    if (terminal_process_.running() &&
        state.mode == puc::tui::InputMode::TERMINAL &&
        !terminal_process_.resize(columns, rows)) {
      Logger<ERROR> << "Could not resize embedded terminal process";
      return false;
    }

    const std::string responses = input_frame_->take_terminal_responses();
    if (!responses.empty() && !terminal_process_.send(responses)) {
      return false;
    }
    if (!terminal_process_.running()) {
      return true;
    }
    switch (terminal_process_.pump(*input_frame_)) {
      case EmbeddedTerminalProcess::PumpResult::RUNNING:
        return true;
      case EmbeddedTerminalProcess::PumpResult::EXITED:
        input_frame_->close_terminal();
        return true;
      case EmbeddedTerminalProcess::PumpResult::FAILED:
        return false;
    }
    return false;
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
        return terminal_process_.send(terminal_input(event));
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
        input_frame_->handle_event(event, InputFrame::Clock::now());
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

  puc::multithreading::JobQueue& workers_; /**< Main-owned shared workers. */
  std::string executable_;                 /**< argv[0] runfiles fallback. */
  puc::terminal::Decoder decoder_; /**< Runtime-configured input Trie. */
  puc::terminal::TimeoutSettings timeout_settings_; /**< Input timing policy. */
  std::shared_ptr<InputFrame> input_frame_;    /**< Editor under manual test. */
  EmbeddedTerminalProcess terminal_process_;   /**< PTY child behind libtmt. */
  std::unique_ptr<Screen> screen_;             /**< Terminal/session owner. */
  std::unique_ptr<ParallelRenderer> renderer_; /**< Frame scheduler. */
  std::shared_ptr<Canvas> canvas_;             /**< Current screen Canvas. */
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
  LOGGER_INIT(logger_config);

  if (std::setlocale(LC_CTYPE, "") == nullptr) {
    Logger<WARN> << "Could not activate the environment's character encoding; "
                    "embedded terminal output may replace non-ASCII text";
  }

  if (std::signal(SIGINT, request_stop) == SIG_ERR ||
      std::signal(SIGTERM, request_stop) == SIG_ERR) {
    Logger<ERROR> << "Could not install termination signal handlers";
    return 1;
  }

  const std::string_view executable = argc > 0 && argv[0] != nullptr
                                          ? std::string_view{argv[0]}
                                          : std::string_view{};
  puc::multithreading::JobQueue workers(kWorkerCount);
  InputTestApplication application(workers, executable);
  bool success = application.setup();
  while (success && stop_requested == 0) {
    success = application.draw();
  }
  success = application.shutdown() && success;
  workers.wait();
  return success ? 0 : 1;
}
