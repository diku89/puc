/**
 * @file screen.cpp
 * @brief POSIX terminal control and ANSI true-color Canvas presentation.
 */

#include "puc-cli/tui/screen.hpp"

#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Screen");
/** @endcond */

namespace puc {
namespace tui {
namespace {

/** Enter the alternate screen, hide the cursor, disable wrap, clear, and home.
 */
constexpr std::string_view kTakeTerminal =
    "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J\x1b[H";

/** Reset styling and restore wrap, cursor visibility, and the primary screen.
 */
constexpr std::string_view kReleaseTerminal =
    "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";

/** ANSI control sequence that moves the cursor to row 1, column 1. */
constexpr std::string_view kHomeCursor = "\x1b[H";

/** ANSI Select Graphic Rendition reset sequence. */
constexpr std::string_view kResetStyle = "\x1b[0m";

/**
 * Write an entire byte sequence, retrying calls interrupted by a signal.
 *
 * @param[in] file_descriptor Destination descriptor.
 * @param[in] bytes Bytes that must all be written.
 * @return Status::OK or Status::TERMINAL_WRITE_FAILED.
 */
Status write_all(int file_descriptor, std::string_view bytes) noexcept {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        ::write(file_descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      Logger<ERROR> << "Terminal write failed: " << std::strerror(errno);
      return Status::TERMINAL_WRITE_FAILED;
    }
    offset += static_cast<size_t>(written);
  }
  return Status::OK;
}

/**
 * Query cell and optional pixel dimensions with `TIOCGWINSZ`.
 *
 * The output descriptor is tried first because it is normally the presentation
 * terminal. If it fails and the descriptors differ, the input descriptor is
 * used as a fallback. Reported total pixel dimensions are converted to the
 * reduced ratio `(pixel_width / columns) : (pixel_height / rows)` without
 * integer division. Missing pixel dimensions leave CellDimensions at `{1, 2}`.
 *
 * @param[in] output_fd Preferred terminal descriptor.
 * @param[in] input_fd Fallback terminal descriptor.
 * @param[out] width Terminal columns, or zero on error.
 * @param[out] height Terminal rows, or zero on error.
 * @param[out] cell_dimensions Reduced relative cell dimensions.
 * @return Status::OK, Status::TERMINAL_QUERY_FAILED, or
 *         Status::INVALID_DIMENSIONS.
 */
Status query_dimensions(int output_fd, int input_fd, size_t& width,
                        size_t& height,
                        CellDimensions& cell_dimensions) noexcept {
  cell_dimensions = CellDimensions{};
  struct winsize dimensions {};
  int result = ::ioctl(output_fd, TIOCGWINSZ, &dimensions);
  if (result != 0 && input_fd != output_fd) {
    result = ::ioctl(input_fd, TIOCGWINSZ, &dimensions);
  }
  if (result != 0) {
    width  = 0;
    height = 0;
    return Status::TERMINAL_QUERY_FAILED;
  }
  if (dimensions.ws_col == 0 || dimensions.ws_row == 0) {
    width  = 0;
    height = 0;
    return Status::INVALID_DIMENSIONS;
  }

  width  = dimensions.ws_col;
  height = dimensions.ws_row;
  if (dimensions.ws_xpixel != 0 && dimensions.ws_ypixel != 0) {
    const size_t relative_width =
        static_cast<size_t>(dimensions.ws_xpixel) * height;
    const size_t relative_height =
        static_cast<size_t>(dimensions.ws_ypixel) * width;
    const size_t common_factor = std::gcd(relative_width, relative_height);
    cell_dimensions.width      = relative_width / common_factor;
    cell_dimensions.height     = relative_height / common_factor;
  }
  return Status::OK;
}

/**
 * Produce a monotonic timestamp for a Screen-generated event.
 *
 * @return Nanoseconds since the steady clock's unspecified epoch.
 */
uint64_t event_timestamp() noexcept {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

/**
 * Append an unsigned decimal integer without locale-sensitive formatting.
 *
 * @param[in,out] output String receiving decimal digits.
 * @param[in] value Integer to encode.
 * @return `true` when the local conversion buffer was large enough.
 */
bool append_unsigned(std::string& output, uint64_t value) {
  char digits[16];
  const auto result = std::to_chars(digits, digits + sizeof(digits), value);
  if (result.ec != std::errc{}) {
    return false;
  }
  output.append(digits, result.ptr);
  return true;
}

/**
 * Append ANSI 24-bit foreground and background color sequences.
 *
 * @param[in,out] output String receiving the escape sequences.
 * @param[in] foreground Packed `0xRRGGBB` foreground color.
 * @param[in] background Packed `0xRRGGBB` background color.
 * @return `true` on successful numeric conversion.
 */
bool append_color(std::string& output, uint32_t foreground,
                  uint32_t background) {
  output.append("\x1b[38;2;");
  if (!append_unsigned(output, (foreground >> 16U) & 0xffU)) {
    return false;
  }
  output.push_back(';');
  if (!append_unsigned(output, (foreground >> 8U) & 0xffU)) {
    return false;
  }
  output.push_back(';');
  if (!append_unsigned(output, foreground & 0xffU)) {
    return false;
  }
  output.append("m\x1b[48;2;");
  if (!append_unsigned(output, (background >> 16U) & 0xffU)) {
    return false;
  }
  output.push_back(';');
  if (!append_unsigned(output, (background >> 8U) & 0xffU)) {
    return false;
  }
  output.push_back(';');
  if (!append_unsigned(output, background & 0xffU)) {
    return false;
  }
  output.push_back('m');
  return true;
}

/**
 * Encode one safe terminal character as UTF-8.
 *
 * Control characters, surrogate values, and out-of-range code points are
 * replaced with U+FFFD so cell contents cannot inject terminal controls.
 *
 * @param[in,out] output String receiving one UTF-8 sequence.
 * @param[in] character Intended Unicode scalar value.
 */
void append_utf8(std::string& output, char32_t character) {
  uint32_t codepoint = static_cast<uint32_t>(character);
  if (codepoint < 0x20U || codepoint == 0x7fU || codepoint > 0x10ffffU ||
      (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
    codepoint = 0xfffdU;
  }

  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
    return;
  }
  if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    return;
  }
  if (codepoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    return;
  }

  output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
  output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
  output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
  output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
}

/**
 * Serialize the complete published Canvas into ANSI terminal output.
 *
 * Rows after the first use absolute cursor addressing so rendering the last
 * column cannot depend on terminal wrap behavior. Color escapes are emitted
 * only when foreground or background changes from the preceding cell.
 *
 * @param[in] canvas Published cells and dimensions to serialize.
 * @param[out] output Receives a complete home-to-reset byte sequence.
 * @return Status::OK or Status::DIMENSION_OVERFLOW if output-size arithmetic
 *         or numeric conversion cannot be represented.
 */
Status render_canvas(const Canvas& canvas, std::string& output) {
  constexpr size_t kMaximumBytesPerCell     = 48;
  const std::span<const Canvas::Cell> cells = canvas.get_drawable_buffer();
  if (cells.size() > (std::numeric_limits<size_t>::max() - kHomeCursor.size() -
                      kResetStyle.size()) /
                         kMaximumBytesPerCell) {
    return Status::DIMENSION_OVERFLOW;
  }

  output.clear();
  output.reserve(kHomeCursor.size() + kResetStyle.size() +
                 cells.size() * kMaximumBytesPerCell);
  output.append(kHomeCursor);

  const auto [width, height] = canvas.get_dimensions();
  bool has_color             = false;
  uint32_t foreground        = 0;
  uint32_t background        = 0;

  for (size_t y = 0; y < height; ++y) {
    if (y != 0) {
      output.append("\x1b[");
      if (!append_unsigned(output, y + 1)) {
        return Status::DIMENSION_OVERFLOW;
      }
      output.append(";1H");
    }

    for (size_t x = 0; x < width; ++x) {
      const Canvas::Cell& cell = cells[y * width + x];
      if (!has_color || cell.foreground_color != foreground ||
          cell.background_color != background) {
        if (!append_color(output, cell.foreground_color,
                          cell.background_color)) {
          return Status::DIMENSION_OVERFLOW;
        }
        foreground = cell.foreground_color;
        background = cell.background_color;
        has_color  = true;
      }
      append_utf8(output, cell.character);
    }
  }

  output.append(kResetStyle);
  return Status::OK;
}

}  // namespace

Screen::Screen(std::shared_ptr<EventBuffer> buffer)
    : Screen(std::move(buffer), STDIN_FILENO, STDOUT_FILENO) {}

Screen::Screen(std::shared_ptr<EventBuffer> buffer, int input_fd, int output_fd)
    : event_buffer_(std::move(buffer)),
      input_fd_(input_fd),
      output_fd_(output_fd) {}

Screen::~Screen() {
  const Status status = release();
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not release terminal during screen destruction: "
                  << status_message(status);
  }
}

Status Screen::take(std::shared_ptr<EventBuffer> buffer) noexcept {
  if (!buffer) {
    Logger<ERROR> << "Cannot take terminal without an event buffer";
    return Status::INVALID_ARGUMENT;
  }
  if (terminal_taken_) {
    event_buffer_ = std::move(buffer);
    Logger<DEBUG> << "Terminal is already owned by this screen";
    return Status::OK;
  }
  if (::isatty(input_fd_) == 0 || ::isatty(output_fd_) == 0) {
    Logger<ERROR> << "Cannot take terminal: "
                  << status_message(Status::TERMINAL_NOT_AVAILABLE);
    return Status::TERMINAL_NOT_AVAILABLE;
  }

  size_t width  = 0;
  size_t height = 0;
  CellDimensions cell_dimensions;
  Status status =
      query_dimensions(output_fd_, input_fd_, width, height, cell_dimensions);
  if (!is_ok(status)) {
    Logger<ERROR> << "Cannot take terminal: " << status_message(status);
    return status;
  }

  if (::tcgetattr(input_fd_, &original_terminal_state_) != 0) {
    Logger<ERROR> << "Could not read terminal settings: "
                  << std::strerror(errno);
    return Status::TERMINAL_CONFIG_FAILED;
  }
  has_original_terminal_state_ = true;

  struct termios raw_terminal_state = original_terminal_state_;
  ::cfmakeraw(&raw_terminal_state);
  raw_terminal_state.c_lflag |= original_terminal_state_.c_lflag & ISIG;
  if (::tcsetattr(input_fd_, TCSANOW, &raw_terminal_state) != 0) {
    Logger<ERROR> << "Could not enable raw terminal mode: "
                  << std::strerror(errno);
    has_original_terminal_state_ = false;
    return Status::TERMINAL_CONFIG_FAILED;
  }

  status = write_all(output_fd_, kTakeTerminal);
  if (!is_ok(status)) {
    static_cast<void>(
        ::tcsetattr(input_fd_, TCSANOW, &original_terminal_state_));
    has_original_terminal_state_ = false;
    return status;
  }

  event_buffer_   = std::move(buffer);
  terminal_taken_ = true;
  last_width_     = width;
  last_height_    = height;
  Logger<INFO> << "Took control of " << width << 'x' << height
               << " terminal with " << cell_dimensions.width << ':'
               << cell_dimensions.height << " cell dimensions";
  return Status::OK;
}

Status Screen::release() noexcept {
  if (!terminal_taken_) {
    return Status::OK;
  }

  Status result = write_all(output_fd_, kReleaseTerminal);
  if (has_original_terminal_state_ &&
      ::tcsetattr(input_fd_, TCSANOW, &original_terminal_state_) != 0) {
    Logger<ERROR> << "Could not restore terminal settings: "
                  << std::strerror(errno);
    if (is_ok(result)) {
      result = Status::TERMINAL_CONFIG_FAILED;
    }
  }

  terminal_taken_              = false;
  has_original_terminal_state_ = false;
  last_width_                  = 0;
  last_height_                 = 0;
  Logger<INFO> << "Released terminal";
  return result;
}

Status Screen::get_dimensions(size_t& width, size_t& height) const noexcept {
  CellDimensions cell_dimensions;
  return get_dimensions(width, height, cell_dimensions);
}

Status Screen::get_dimensions(size_t& width, size_t& height,
                              CellDimensions& cell_dimensions) const noexcept {
  const Status status =
      query_dimensions(output_fd_, input_fd_, width, height, cell_dimensions);
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not query terminal dimensions: "
                  << status_message(status);
  }
  return status;
}

Status Screen::set_canvas(std::shared_ptr<Canvas> canvas) noexcept {
  if (!canvas) {
    canvas_.reset();
    Logger<ERROR> << status_message(Status::CANVAS_NOT_SET);
    return Status::CANVAS_NOT_SET;
  }
  if (!is_ok(canvas->get_status())) {
    const Status status = canvas->get_status();
    Logger<ERROR> << "Cannot attach invalid canvas: " << status_message(status);
    return status;
  }

  const auto [width, height] = canvas->get_dimensions();
  canvas_                    = std::move(canvas);
  Logger<DEBUG> << "Attached " << width << 'x' << height << " canvas";
  return Status::OK;
}

Status Screen::push_event(const std::shared_ptr<EventBuffer>& buffer,
                          const Event& event) noexcept {
  if (!buffer) {
    Logger<ERROR> << "Cannot push event to a null buffer";
    return Status::INVALID_ARGUMENT;
  }

  const std::lock_guard lock(buffer->lock);
  if (buffer->size == EventBuffer::kMaxEvents) {
    Logger<WARN> << status_message(Status::EVENT_BUFFER_FULL);
    return Status::EVENT_BUFFER_FULL;
  }

  buffer->events[buffer->write_index] = event;
  buffer->write_index = (buffer->write_index + 1) % EventBuffer::kMaxEvents;
  ++buffer->size;
  Logger<DEBUG> << "Queued screen event " << event.id;
  return Status::OK;
}

std::optional<Screen::Event> Screen::pop_event() noexcept {
  if (!event_buffer_) {
    Logger<WARN> << "Cannot pop event from a null buffer";
    return std::nullopt;
  }

  const std::lock_guard lock(event_buffer_->lock);
  if (event_buffer_->size == 0) {
    return std::nullopt;
  }

  Event event = event_buffer_->events[event_buffer_->read_index];
  event_buffer_->read_index =
      (event_buffer_->read_index + 1) % EventBuffer::kMaxEvents;
  --event_buffer_->size;
  return event;
}

size_t Screen::pending_events() const noexcept {
  if (!event_buffer_) {
    return 0;
  }

  const std::lock_guard lock(event_buffer_->lock);
  return event_buffer_->size;
}

Status Screen::draw() noexcept {
  if (!terminal_taken_) {
    Logger<ERROR> << "Cannot draw screen: "
                  << status_message(Status::TERMINAL_NOT_AVAILABLE);
    return Status::TERMINAL_NOT_AVAILABLE;
  }
  if (!canvas_) {
    Logger<ERROR> << "Cannot draw screen: "
                  << status_message(Status::CANVAS_NOT_SET);
    return Status::CANVAS_NOT_SET;
  }

  size_t width  = 0;
  size_t height = 0;
  Status status = get_dimensions(width, height);
  if (!is_ok(status)) {
    return status;
  }

  Status event_status = Status::OK;
  if (width != last_width_ || height != last_height_) {
    Event resize_event{
        .timestamp = event_timestamp(),
        .id        = next_event_id_++,
        .type      = EventType::RESIZE,
    };
    resize_event.data.resize.width  = width;
    resize_event.data.resize.height = height;
    event_status                    = push_event(event_buffer_, resize_event);
    last_width_                     = width;
    last_height_                    = height;
    Logger<INFO> << "Terminal resized to " << width << 'x' << height;
  }

  std::string output;
  status = render_canvas(*canvas_, output);
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not render canvas: " << status_message(status);
    return status;
  }
  status = write_all(output_fd_, output);
  if (!is_ok(status)) {
    return status;
  }

  Logger<DEBUG> << "Presented canvas with "
                << canvas_->get_drawable_buffer().size() << " cells";
  return event_status;
}

bool Screen::is_taken() const noexcept { return terminal_taken_; }

}  // namespace tui
}  // namespace puc
