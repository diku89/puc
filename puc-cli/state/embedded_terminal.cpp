/**
 * @file embedded_terminal.cpp
 * @brief Integrated-terminal PTY lifecycle implementation.
 */

#include "puc-cli/state/embedded_terminal.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||      \
    defined(__NetBSD__) || defined(__DragonFly__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "puc-cli/state/input.hpp"
#include "puc-cli/tui/input_frame.hpp"
#include "puc-cli/tui/status.hpp"
#include "utils/logger/logger.hpp"

/** @cond EMBEDDED_TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("Embedded Terminal");
/** @endcond */

namespace puc::app {
namespace {

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
bool is_modifier_key(terminal::NamedKey key) noexcept {
  switch (key) {
    case terminal::NamedKey::LEFT_SHIFT:
    case terminal::NamedKey::LEFT_CONTROL:
    case terminal::NamedKey::LEFT_ALT:
    case terminal::NamedKey::LEFT_SUPER:
    case terminal::NamedKey::LEFT_HYPER:
    case terminal::NamedKey::LEFT_META:
    case terminal::NamedKey::RIGHT_SHIFT:
    case terminal::NamedKey::RIGHT_CONTROL:
    case terminal::NamedKey::RIGHT_ALT:
    case terminal::NamedKey::RIGHT_SUPER:
    case terminal::NamedKey::RIGHT_HYPER:
    case terminal::NamedKey::RIGHT_META:
    case terminal::NamedKey::ISO_LEVEL3_SHIFT:
    case terminal::NamedKey::ISO_LEVEL5_SHIFT:
      return true;
    default:
      return false;
  }
}

/** Encode an xterm cursor key, retaining modifiers useful to child programs. */
void append_cursor_key(char final, terminal::Modifiers modifiers,
                       std::string& output) {
  unsigned int parameter = 1U;
  parameter += modifiers.contains(terminal::Modifier::SHIFT) ? 1U : 0U;
  parameter += modifiers.contains(terminal::Modifier::ALT) ? 2U : 0U;
  parameter += modifiers.contains(terminal::Modifier::CONTROL) ? 4U : 0U;
  parameter += modifiers.contains(terminal::Modifier::SUPER) ? 8U : 0U;
  parameter += modifiers.contains(terminal::Modifier::HYPER) ? 16U : 0U;
  parameter += modifiers.contains(terminal::Modifier::META) ? 32U : 0U;
  output.append("\x1b[");
  if (parameter != 1U) {
    output.append("1;");
    output.append(std::to_string(parameter));
  }
  output.push_back(final);
}

/** Convert one decoded key press back to conventional PTY input bytes. */
void append_terminal_key(const terminal::KeyEvent& event, std::string& output) {
  if (event.action == terminal::KeyAction::RELEASE) {
    return;
  }
  if (const auto* named = std::get_if<terminal::NamedKey>(&event.key.value)) {
    if (is_modifier_key(*named)) {
      return;
    }
    switch (*named) {
      case terminal::NamedKey::ESCAPE:
        output.push_back('\x1b');
        return;
      case terminal::NamedKey::ENTER:
      case terminal::NamedKey::KEYPAD_ENTER:
        output.push_back('\r');
        return;
      case terminal::NamedKey::TAB:
        output.append(event.modifiers.contains(terminal::Modifier::SHIFT)
                          ? "\x1b[Z"
                          : "\t");
        return;
      case terminal::NamedKey::BACKSPACE:
        output.push_back('\x7f');
        return;
      case terminal::NamedKey::UP:
      case terminal::NamedKey::KEYPAD_UP:
        append_cursor_key('A', event.modifiers, output);
        return;
      case terminal::NamedKey::DOWN:
      case terminal::NamedKey::KEYPAD_DOWN:
        append_cursor_key('B', event.modifiers, output);
        return;
      case terminal::NamedKey::RIGHT:
      case terminal::NamedKey::KEYPAD_RIGHT:
        append_cursor_key('C', event.modifiers, output);
        return;
      case terminal::NamedKey::LEFT:
      case terminal::NamedKey::KEYPAD_LEFT:
        append_cursor_key('D', event.modifiers, output);
        return;
      case terminal::NamedKey::HOME:
      case terminal::NamedKey::KEYPAD_HOME:
        output.append("\x1b[H");
        return;
      case terminal::NamedKey::END:
      case terminal::NamedKey::KEYPAD_END:
        output.append("\x1b[F");
        return;
      case terminal::NamedKey::INSERT:
      case terminal::NamedKey::KEYPAD_INSERT:
        output.append("\x1b[2~");
        return;
      case terminal::NamedKey::DELETE_KEY:
      case terminal::NamedKey::KEYPAD_DELETE:
        output.append("\x1b[3~");
        return;
      case terminal::NamedKey::PAGE_UP:
      case terminal::NamedKey::KEYPAD_PAGE_UP:
        output.append("\x1b[5~");
        return;
      case terminal::NamedKey::PAGE_DOWN:
      case terminal::NamedKey::KEYPAD_PAGE_DOWN:
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
  if (event.modifiers.contains(terminal::Modifier::CONTROL)) {
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
  if (event.modifiers.contains(terminal::Modifier::ALT)) {
    output.push_back('\x1b');
  }
  if (!event.text.empty()) {
    output.append(event.text);
  } else {
    append_utf8(value, output);
  }
}

/** Convert one normalized application event into PTY input bytes. */
std::string terminal_input(const terminal::Event& event) {
  std::string output;
  if (const auto* text = std::get_if<terminal::TextEvent>(&event)) {
    output = text->utf8;
  } else if (const auto* key = std::get_if<terminal::KeyEvent>(&event)) {
    append_terminal_key(*key, output);
  } else if (const auto* paste = std::get_if<terminal::PasteEvent>(&event);
             paste != nullptr && paste->phase == terminal::PastePhase::DATA) {
    output = paste->data;
  } else if (const auto* command =
                 std::get_if<terminal::CommandEvent>(&event)) {
    switch (command->command) {
      case terminal::Command::MOVE_WORD_LEFT:
        output.append(
            "\x1b"
            "b");
        break;
      case terminal::Command::MOVE_WORD_RIGHT:
        output.append(
            "\x1b"
            "f");
        break;
      case terminal::Command::MOVE_ROW_START:
        output.push_back('\x01');
        break;
      case terminal::Command::MOVE_ROW_END:
        output.push_back('\x05');
        break;
      case terminal::Command::MOVE_PAGE_UP:
        output.append("\x1b[5~");
        break;
      case terminal::Command::MOVE_PAGE_DOWN:
        output.append("\x1b[6~");
        break;
      case terminal::Command::MOVE_BUFFER_START:
      case terminal::Command::MOVE_BUFFER_END:
      case terminal::Command::COPY:
      case terminal::Command::SELECT_ALL:
      case terminal::Command::ENTER_COMMAND_MODE:
      case terminal::Command::ENTER_TERMINAL_MODE:
        break;
    }
  }
  return output;
}

}  // namespace

/** Child process and master side of one embedded pseudo-terminal. */
class EmbeddedTerminalSubsystem::Impl {
 public:
  /** Result of one nonblocking output and exit-status pump. */
  enum class PumpResult { RUNNING, EXITED, FAILED };

  /** Terminate and reap only the child process created by this object. */
  ~Impl() { stop(); }

  /** Start an interactive shell with the requested terminal geometry. */
  bool start(std::string_view shell, std::size_t generation,
             std::size_t columns, std::size_t rows) {
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

    const std::string executable =
        shell.empty() ? "/bin/sh" : std::string{shell};
    int master_fd     = -1;
    const pid_t child = ::forkpty(&master_fd, nullptr, nullptr, &size);
    if (child < 0) {
      Logger<ERROR> << "Could not create embedded terminal process";
      return false;
    }
    if (child == 0) {
      char* const arguments[]{const_cast<char*>(executable.c_str()),
                              const_cast<char*>("-i"), nullptr};
      ::execv(executable.c_str(), arguments);
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

  /** Change geometry and let the kernel deliver SIGWINCH to the child. */
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

  /** Queue bytes for the child and flush nonblocking output as far as possible.
   */
  bool send(std::string_view input) {
    pending_input_.append(input);
    return !running() || flush_input();
  }

  /** Read every available output block and observe natural child termination.
   */
  PumpResult pump(std::string& output) {
    output.clear();
    if (!running()) {
      return PumpResult::EXITED;
    }
    char bytes[8192];
    while (true) {
      const ssize_t count = ::read(master_fd_, bytes, sizeof(bytes));
      if (count > 0) {
        output.append(bytes, static_cast<std::size_t>(count));
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
    if (wait_result == 0 || (wait_result < 0 && errno == EINTR)) {
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

EmbeddedTerminalSubsystem::EmbeddedTerminalSubsystem(
    EmbeddedTerminalSubsystemOptions options)
    : AppSubsystem("embedded-terminal",
                   subsystem_dependencies<InputSubsystem>()),
      options_(std::move(options)) {}

EmbeddedTerminalSubsystem::~EmbeddedTerminalSubsystem() = default;

Status EmbeddedTerminalSubsystem::initialize(AppState& app) {
  InputSubsystem* input = app.get_subsystem<InputSubsystem>();
  if (input == nullptr || input->input_frame() == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  input_frame_ = input->input_frame();
  impl_        = std::make_unique<Impl>();
  return Status::OK;
}

Status EmbeddedTerminalSubsystem::start(AppState& app) {
  static_cast<void>(app);
  const std::lock_guard lock(mutex_);
  if (impl_ == nullptr || input_frame_ == nullptr) {
    return Status::SUBSYSTEM_FAILURE;
  }
  active_ = true;
  return Status::OK;
}

Status EmbeddedTerminalSubsystem::stop(AppState& app) noexcept {
  static_cast<void>(app);
  const std::lock_guard lock(mutex_);
  active_ = false;
  if (impl_ != nullptr) {
    impl_->stop();
  }
  return Status::OK;
}

Status EmbeddedTerminalSubsystem::terminate(AppState& app) noexcept {
  static_cast<void>(stop(app));
  const std::lock_guard lock(mutex_);
  impl_.reset();
  input_frame_.reset();
  return Status::OK;
}

Status EmbeddedTerminalSubsystem::send_event(const terminal::Event& event) {
  const std::lock_guard lock(mutex_);
  if (!active_ || impl_ == nullptr) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }
  return impl_->send(terminal_input(event)) ? Status::OK
                                            : Status::SUBSYSTEM_FAILURE;
}

Status EmbeddedTerminalSubsystem::synchronize(std::size_t screen_width,
                                              std::size_t screen_height) {
  const std::lock_guard lock(mutex_);
  if (!active_ || impl_ == nullptr || input_frame_ == nullptr) {
    return Status::INVALID_LIFECYCLE_TRANSITION;
  }
  const tui::InputFrameSnapshot state = input_frame_->snapshot();
  if (!state.terminal_session_active) {
    impl_->stop();
    return Status::OK;
  }

  const bool terminal_fits =
      screen_width >= tui::InputFrame::kMinimumWidth &&
      screen_height >= tui::InputFrame::kTerminalMinimumHeight;
  const std::size_t outer_height =
      tui::InputFrame::terminal_height(screen_height);
  const std::size_t columns = screen_width > 5U ? screen_width - 5U : 0U;
  const std::size_t rows    = outer_height > 4U ? outer_height - 4U : 0U;
  if ((!impl_->running() || impl_->generation() != state.terminal_generation) &&
      terminal_fits &&
      !impl_->start(options_.shell, state.terminal_generation, columns, rows)) {
    input_frame_->close_terminal();
    return Status::SUBSYSTEM_FAILURE;
  }
  if (impl_->running() && state.mode == tui::InputMode::TERMINAL &&
      !impl_->resize(columns, rows)) {
    Logger<ERROR> << "Could not resize embedded terminal process";
    return Status::SUBSYSTEM_FAILURE;
  }

  const std::string responses = input_frame_->take_terminal_responses();
  if (!responses.empty() && !impl_->send(responses)) {
    return Status::SUBSYSTEM_FAILURE;
  }
  if (!impl_->running()) {
    return Status::OK;
  }

  std::string output;
  switch (impl_->pump(output)) {
    case Impl::PumpResult::RUNNING:
      return output.empty() || tui::is_ok(input_frame_->write_terminal(output))
                 ? Status::OK
                 : Status::SUBSYSTEM_FAILURE;
    case Impl::PumpResult::EXITED:
      if (!output.empty() &&
          !tui::is_ok(input_frame_->write_terminal(output))) {
        return Status::SUBSYSTEM_FAILURE;
      }
      input_frame_->close_terminal();
      return Status::OK;
    case Impl::PumpResult::FAILED:
      return Status::SUBSYSTEM_FAILURE;
  }
  return Status::SUBSYSTEM_FAILURE;
}

bool EmbeddedTerminalSubsystem::child_running() const noexcept {
  const std::lock_guard lock(mutex_);
  return impl_ != nullptr && impl_->running();
}

std::size_t EmbeddedTerminalSubsystem::child_generation() const noexcept {
  const std::lock_guard lock(mutex_);
  return impl_ == nullptr ? 0U : impl_->generation();
}

}  // namespace puc::app
