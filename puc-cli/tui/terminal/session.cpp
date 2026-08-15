/**
 * @file session.cpp
 * @brief POSIX terminal lifecycle, transport, dimensions, and clipboard I/O.
 */

#include "puc-cli/tui/terminal/session.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "msgs/screen_msgs.hpp"
#include "puc-cli/tui/terminal/clipboard.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/logger/logger.hpp"

/** @cond TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("TerminalSession");
/** @endcond */

namespace puc {
namespace terminal {

namespace {

/** Kitty keyboard features required by PUC's normalized input contract. */
constexpr std::uint32_t kKeyboardEnhancements =
    static_cast<std::uint32_t>(KeyboardEnhancement::DISAMBIGUATE_ESCAPE_CODES) |
    static_cast<std::uint32_t>(KeyboardEnhancement::REPORT_ALTERNATE_KEYS) |
    static_cast<std::uint32_t>(KeyboardEnhancement::REPORT_ALL_KEYS) |
    static_cast<std::uint32_t>(KeyboardEnhancement::REPORT_ASSOCIATED_TEXT);

/** Terminate after logging an unrecoverable asynchronous terminal failure. */
[[noreturn]] void fatal_async_failure(std::string_view operation,
                                      Status status) noexcept {
  Logger<ERROR> << "Asynchronous terminal " << operation
                << " failed: " << status_message(status);
  std::terminate();
}

}  // namespace

TerminalSession::TerminalSession() noexcept
    : TerminalSession(STDIN_FILENO, STDOUT_FILENO) {}

TerminalSession::TerminalSession(int input_fd, int output_fd) noexcept
    : input_fd_(input_fd), output_fd_(output_fd) {}

TerminalSession::~TerminalSession() {
  unbind_screen_channels();
  const Status status = release();
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not release terminal during session destruction: "
                  << status_message(status);
  }
}

Status TerminalSession::take() noexcept {
  if (active_) {
    Logger<WARN> << status_message(Status::ALREADY_ACTIVE);
    return Status::ALREADY_ACTIVE;
  }
  if (::isatty(input_fd_) == 0 || ::isatty(output_fd_) == 0) {
    Logger<ERROR> << status_message(Status::TERMINAL_NOT_AVAILABLE);
    return Status::TERMINAL_NOT_AVAILABLE;
  }
  if (::tcgetattr(input_fd_, &original_terminal_state_) != 0) {
    Logger<ERROR> << "Could not read terminal attributes: "
                  << std::strerror(errno);
    return Status::TERMINAL_CONFIG_FAILED;
  }
  has_original_terminal_state_ = true;

  termios raw_terminal_state = original_terminal_state_;
  ::cfmakeraw(&raw_terminal_state);
  raw_terminal_state.c_lflag |= original_terminal_state_.c_lflag & ISIG;
  if (::tcsetattr(input_fd_, TCSANOW, &raw_terminal_state) != 0) {
    Logger<ERROR> << "Could not enable raw terminal mode: "
                  << std::strerror(errno);
    has_original_terminal_state_ = false;
    return Status::TERMINAL_CONFIG_FAILED;
  }

  active_modes_ = 0;
  std::string enter_sequence;
  build_enter_sequence(enter_sequence);
  const Status write_status = write_all(enter_sequence);
  if (!is_ok(write_status)) {
    std::string leave_sequence;
    build_leave_sequence(leave_sequence);
    static_cast<void>(write_all(leave_sequence));
    if (::tcsetattr(input_fd_, TCSANOW, &original_terminal_state_) != 0) {
      Logger<ERROR> << "Could not restore terminal attributes after failed "
                       "session setup: "
                    << std::strerror(errno);
    }
    active_modes_                = 0;
    has_original_terminal_state_ = false;
    return write_status;
  }

  active_ = true;
  Logger<INFO> << "Took terminal session in PUC interactive mode";
  return Status::OK;
}

Status TerminalSession::release() noexcept {
  if (!active_) {
    return Status::OK;
  }

  Status result = Status::OK;
  if (!screen_final_bytes_.empty()) {
    result = write_all(screen_final_bytes_);
  }
  std::string leave_sequence;
  build_leave_sequence(leave_sequence);
  const Status leave_status = write_all(leave_sequence);
  if (is_ok(result)) {
    result = leave_status;
  }
  if (has_original_terminal_state_ &&
      ::tcsetattr(input_fd_, TCSANOW, &original_terminal_state_) != 0) {
    Logger<ERROR> << "Could not restore terminal attributes: "
                  << std::strerror(errno);
    if (is_ok(result)) {
      result = Status::TERMINAL_CONFIG_FAILED;
    }
  }

  active_                      = false;
  has_original_terminal_state_ = false;
  active_modes_                = 0;
  screen_final_bytes_.clear();
  Logger<INFO> << "Released terminal session";
  return result;
}

Status TerminalSession::write(std::string_view bytes) noexcept {
  if (!active_) {
    Logger<ERROR> << "Cannot write through an inactive terminal session";
    return Status::NOT_ACTIVE;
  }
  return write_all(bytes);
}

Status TerminalSession::read(Decoder& decoder, std::vector<Event>& events,
                             std::size_t& bytes_read, bool& end_of_input) {
  bytes_read   = 0;
  end_of_input = false;
  if (!active_) {
    Logger<ERROR> << "Cannot read through an inactive terminal session";
    return Status::NOT_ACTIVE;
  }

  char bytes[4096];
  while (true) {
    const ssize_t count = ::read(input_fd_, bytes, sizeof(bytes));
    if (count > 0) {
      bytes_read = static_cast<std::size_t>(count);
      return decoder.feed(std::string_view{bytes, bytes_read}, events);
    }
    if (count == 0) {
      end_of_input = true;
      Logger<DEBUG> << "Reached end of terminal input";
      return decoder.finish(events);
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return Status::OK;
    }
    Logger<ERROR> << "Could not read terminal input: " << std::strerror(errno);
    return Status::TERMINAL_READ_FAILED;
  }
}

Status TerminalSession::get_size(TerminalSize& size) const noexcept {
  size = {};
  struct winsize dimensions {};
  if (::ioctl(output_fd_, TIOCGWINSZ, &dimensions) != 0 &&
      (input_fd_ == output_fd_ ||
       ::ioctl(input_fd_, TIOCGWINSZ, &dimensions) != 0)) {
    Logger<ERROR> << "Could not query terminal dimensions: "
                  << std::strerror(errno);
    return Status::TERMINAL_QUERY_FAILED;
  }
  if (dimensions.ws_col == 0U || dimensions.ws_row == 0U) {
    Logger<ERROR> << "Terminal reported zero character-cell dimensions";
    return Status::TERMINAL_QUERY_FAILED;
  }
  size.width        = dimensions.ws_col;
  size.height       = dimensions.ws_row;
  size.pixel_width  = dimensions.ws_xpixel;
  size.pixel_height = dimensions.ws_ypixel;
  return Status::OK;
}

Status TerminalSession::set_clipboard(ClipboardSelection selection,
                                      std::string_view data) noexcept {
  if (!active_) {
    return Status::NOT_ACTIVE;
  }
  std::string sequence;
  const Status status = build_clipboard_write(selection, data, sequence);
  return is_ok(status) ? write_all(sequence) : status;
}

Status TerminalSession::query_clipboard(ClipboardSelection selection) noexcept {
  if (!active_) {
    return Status::NOT_ACTIVE;
  }
  std::string sequence;
  build_clipboard_query(selection, sequence);
  return write_all(sequence);
}

Status TerminalSession::query_keyboard_protocol() noexcept {
  if (!active_) {
    return Status::NOT_ACTIVE;
  }
  return write_all(kitty_keyboard_query());
}

Status TerminalSession::bind_screen_channels(ipc::Directory& directory) {
  if (screen_command_subscription_.active()) {
    return screen_directory_ == &directory ? Status::OK
                                           : Status::INVALID_ARGUMENT;
  }
  ipc::Subscription subscription;
  const ipc::Status status = directory.subscribe(
      msg::kScreenCommandChannel,
      [this](ipc::Channel::Bytes payload) noexcept {
        receive_screen_command(payload);
      },
      subscription);
  if (!ipc::is_ok(status)) {
    Logger<ERROR> << "Could not subscribe TerminalSession to '"
                  << msg::kScreenCommandChannel
                  << "': " << ipc::status_message(status);
    return Status::CHANNEL_SETUP_FAILED;
  }
  if (directory.get_channel(msg::kScreenResizeEventChannel) == nullptr) {
    Logger<ERROR> << "Resize event channel is not registered: '"
                  << msg::kScreenResizeEventChannel << "'";
    subscription.reset();
    return Status::CHANNEL_SETUP_FAILED;
  }
  screen_directory_            = &directory;
  screen_command_subscription_ = std::move(subscription);
  Logger<INFO> << "Bound terminal session to asynchronous screen channels";
  return Status::OK;
}

void TerminalSession::unbind_screen_channels() noexcept {
  screen_command_subscription_.reset();
  screen_directory_ = nullptr;
}

void TerminalSession::receive_screen_command(
    ipc::Channel::Bytes payload) noexcept {
  msg::ScreenCommand command;
  const msg::Status status =
      screen_command_codec_.deserialize(payload, command);
  if (!msg::is_ok(status)) {
    Logger<ERROR> << "Discarded malformed screen command: "
                  << msg::status_message(status);
    return;
  }
  execute_screen_command(command);
}

void TerminalSession::execute_screen_command(
    const msg::ScreenCommand& command) noexcept {
  if (const auto* take = std::get_if<msg::ScreenTakeCommand>(&command.data)) {
    const Status status = this->take();
    if (!is_ok(status) && status != Status::ALREADY_ACTIVE) {
      fatal_async_failure("take", status);
    }
    screen_final_bytes_ = take->final_bytes;
    if (!take->initial_bytes.empty()) {
      const Status write_status = write(take->initial_bytes);
      if (!is_ok(write_status)) {
        fatal_async_failure("initial presentation", write_status);
      }
    }
    publish_size_if_changed();
    return;
  }
  if (const auto* release_command =
          std::get_if<msg::ScreenReleaseCommand>(&command.data)) {
    static_cast<void>(release_command);
    const Status status = release();
    last_published_size_.reset();
    if (!is_ok(status)) {
      fatal_async_failure("release", status);
    }
    return;
  }

  if (const auto* clipboard =
          std::get_if<msg::ScreenSetClipboardCommand>(&command.data)) {
    if (!active()) {
      fatal_async_failure("set clipboard", Status::NOT_ACTIVE);
    }
    const ClipboardSelection selection =
        clipboard->selection == msg::ScreenClipboardSelection::PRIMARY
            ? ClipboardSelection::PRIMARY
            : ClipboardSelection::CLIPBOARD;
    const Status status = set_clipboard(selection, clipboard->text);
    if (!is_ok(status)) {
      fatal_async_failure("set clipboard", status);
    }
    return;
  }

  if (!active()) {
    fatal_async_failure("present", Status::NOT_ACTIVE);
  }
  publish_size_if_changed();
  const auto& present = std::get<msg::ScreenPresentCommand>(command.data);
  const Status status = write(present.bytes);
  if (!is_ok(status)) {
    fatal_async_failure("present", status);
  }
}

void TerminalSession::publish_size_if_changed() noexcept {
  if (!active() || screen_directory_ == nullptr) {
    return;
  }
  TerminalSize size;
  const Status status = get_size(size);
  if (!is_ok(status)) {
    // Geometry is environmental state. A later presentation retries it.
    return;
  }
  if (last_published_size_.has_value() && *last_published_size_ == size) {
    return;
  }
  constexpr std::size_t kMaximumWireDimension =
      std::numeric_limits<std::uint32_t>::max();
  if (size.width > kMaximumWireDimension ||
      size.height > kMaximumWireDimension ||
      size.pixel_width > kMaximumWireDimension ||
      size.pixel_height > kMaximumWireDimension) {
    fatal_async_failure("resize publication", Status::TERMINAL_QUERY_FAILED);
  }

  const msg::ScreenResizeEvent event{
      .width        = static_cast<std::uint32_t>(size.width),
      .height       = static_cast<std::uint32_t>(size.height),
      .pixel_width  = static_cast<std::uint32_t>(size.pixel_width),
      .pixel_height = static_cast<std::uint32_t>(size.pixel_height),
  };
  std::vector<std::uint8_t> payload;
  const msg::Status encode_status =
      resize_event_codec_.serialize(event, payload);
  if (!msg::is_ok(encode_status)) {
    Logger<ERROR> << "Could not encode resize event: "
                  << msg::status_message(encode_status);
    fatal_async_failure("resize encoding", Status::CHANNEL_SETUP_FAILED);
  }
  const ipc::TransferResult transfer =
      screen_directory_->transmit(msg::kScreenResizeEventChannel, payload);
  if (!ipc::is_ok(transfer.status) || transfer.bytes != payload.size()) {
    Logger<ERROR> << "Could not publish resize event: "
                  << ipc::status_message(transfer.status);
    fatal_async_failure("resize delivery", Status::CHANNEL_SETUP_FAILED);
  }
  last_published_size_ = size;
  Logger<DEBUG> << "Published terminal geometry " << size.width << 'x'
                << size.height;
}

Status TerminalSession::write_all(std::string_view bytes) noexcept {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        ::write(output_fd_, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    Logger<ERROR> << "Could not write terminal output: "
                  << (count == 0 ? "zero-byte write" : std::strerror(errno));
    return Status::TERMINAL_WRITE_FAILED;
  }
  return Status::OK;
}

void TerminalSession::build_enter_sequence(std::string& output) {
  const auto enable = [&output, this](Mode mode, ActiveMode active_mode) {
    output.append(mode_sequence(mode, true));
    active_modes_ |= active_mode;
  };

  enable(Mode::ALTERNATE_SCREEN, ACTIVE_ALTERNATE_SCREEN);
  output.append(mode_sequence(Mode::CURSOR_VISIBLE, false));
  active_modes_ |= ACTIVE_HIDDEN_CURSOR;
  output.append(mode_sequence(Mode::AUTO_WRAP, false));
  active_modes_ |= ACTIVE_DISABLED_WRAP;
  enable(Mode::BRACKETED_PASTE, ACTIVE_BRACKETED_PASTE);
  enable(Mode::FOCUS_REPORTING, ACTIVE_FOCUS);
  enable(Mode::MOUSE_DRAG_TRACKING, ACTIVE_MOUSE_DRAG);
  enable(Mode::SGR_MOUSE, ACTIVE_SGR_MOUSE);

  std::string keyboard_sequence;
  if (is_ok(build_kitty_keyboard_push(kKeyboardEnhancements,
                                      keyboard_sequence))) {
    output.append(keyboard_sequence);
    active_modes_ |= ACTIVE_KITTY_KEYBOARD;
  }
}

void TerminalSession::build_leave_sequence(std::string& output) const {
  if ((active_modes_ & ACTIVE_KITTY_KEYBOARD) != 0U) {
    output.append(kitty_keyboard_pop());
  }
  if ((active_modes_ & ACTIVE_SGR_MOUSE) != 0U) {
    output.append(mode_sequence(Mode::SGR_MOUSE, false));
  }
  if ((active_modes_ & ACTIVE_MOUSE_DRAG) != 0U) {
    output.append(mode_sequence(Mode::MOUSE_DRAG_TRACKING, false));
  }
  if ((active_modes_ & ACTIVE_FOCUS) != 0U) {
    output.append(mode_sequence(Mode::FOCUS_REPORTING, false));
  }
  if ((active_modes_ & ACTIVE_BRACKETED_PASTE) != 0U) {
    output.append(mode_sequence(Mode::BRACKETED_PASTE, false));
  }
  if ((active_modes_ & ACTIVE_DISABLED_WRAP) != 0U) {
    output.append(mode_sequence(Mode::AUTO_WRAP, true));
  }
  if ((active_modes_ & ACTIVE_HIDDEN_CURSOR) != 0U) {
    output.append(mode_sequence(Mode::CURSOR_VISIBLE, true));
  }
  if ((active_modes_ & ACTIVE_ALTERNATE_SCREEN) != 0U) {
    output.append(mode_sequence(Mode::ALTERNATE_SCREEN, false));
  }
}

}  // namespace terminal
}  // namespace puc
