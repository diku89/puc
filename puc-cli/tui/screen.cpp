/**
 * @file screen.cpp
 * @brief Asynchronous Canvas presentation over bounded IPC channels.
 */

#include "puc-cli/tui/screen.hpp"

#include <unistd.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "msgs/screen_msgs.hpp"
#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/selection.hpp"
#include "puc-cli/tui/zbuf.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Screen");
/** @endcond */

namespace puc::tui {
namespace {

/** Clear the PUC-owned alternate screen and move to its first cell. */
constexpr std::string_view kClearAndHome = "\x1b[2J\x1b[H";

/** ANSI control sequence that moves the cursor to row 1, column 1. */
constexpr std::string_view kHomeCursor = "\x1b[H";

/** Reset styling before PUC relinquishes presentation control. */
constexpr std::string_view kResetStyle = "\x1b[0m";

/** Maximum Screen commands retained while terminal delivery is behind. */
constexpr std::size_t kScreenCommandDepth = 3U;

/** Geometry is state, so only the newest pending observation is useful. */
constexpr std::size_t kResizeEventDepth = 1U;

/** Test whether a terminal cell lies inside a half-open frame rectangle. */
bool contains(const Canvas::Rect& rect,
              const terminal::CellPosition& position) noexcept {
  return position.x >= rect.x && position.y >= rect.y &&
         position.x - rect.x < rect.width && position.y - rect.y < rect.height;
}

/** Convert one unsigned absolute coordinate to a signed frame-local value. */
Status relative_coordinate(std::size_t absolute, std::size_t origin,
                           std::int64_t& output) noexcept {
  constexpr std::size_t kMaximum =
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max());
  if (absolute >= origin) {
    const std::size_t difference = absolute - origin;
    if (difference > kMaximum) {
      return Status::DIMENSION_OVERFLOW;
    }
    output = static_cast<std::int64_t>(difference);
    return Status::OK;
  }

  const std::size_t difference = origin - absolute;
  if (difference > kMaximum) {
    return Status::DIMENSION_OVERFLOW;
  }
  output = -static_cast<std::int64_t>(difference);
  return Status::OK;
}

/** Convert an absolute terminal position to signed frame-local coordinates. */
Status local_position(const terminal::CellPosition& position,
                      const Canvas::Rect& rect,
                      SelectionPosition& output) noexcept {
  Status status = relative_coordinate(position.x, rect.x, output.x);
  if (!is_ok(status)) {
    return status;
  }
  return relative_coordinate(position.y, rect.y, output.y);
}

/** Return the terminal modes selected and owned by Screen. */
msg::ScreenSessionOptions screen_session_options() noexcept {
  return msg::ScreenSessionOptions{
      .preserve_signals     = true,
      .alternate_screen     = true,
      .hide_cursor          = true,
      .disable_auto_wrap    = true,
      .bracketed_paste      = false,
      .focus_reporting      = false,
      .mouse                = msg::ScreenMouseTracking::NONE,
      .kitty_keyboard_flags = 0U,
  };
}

/** Append an unsigned decimal integer without locale-sensitive formatting. */
bool append_unsigned(std::string& output, std::uint64_t value) {
  char digits[16];
  const auto result = std::to_chars(digits, digits + sizeof(digits), value);
  if (result.ec != std::errc{}) {
    return false;
  }
  output.append(digits, result.ptr);
  return true;
}

/** Append ANSI 24-bit foreground and background color sequences. */
bool append_color(std::string& output, std::uint32_t foreground,
                  std::uint32_t background) {
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

/** Encode one safe terminal character as UTF-8. */
void append_utf8(std::string& output, char32_t character) {
  std::uint32_t codepoint = static_cast<std::uint32_t>(character);
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

/** Serialize the complete published Canvas into ANSI terminal output. */
Status render_canvas(const Canvas& canvas, std::string& output) {
  constexpr std::size_t kMaximumBytesPerCell = 48U;
  const std::span<const Canvas::Cell> cells  = canvas.get_drawable_buffer();
  if (cells.size() > (std::numeric_limits<std::size_t>::max() -
                      kHomeCursor.size() - kResetStyle.size()) /
                         kMaximumBytesPerCell) {
    return Status::DIMENSION_OVERFLOW;
  }

  output.clear();
  output.reserve(kHomeCursor.size() + kResetStyle.size() +
                 cells.size() * kMaximumBytesPerCell);
  output.append(kHomeCursor);

  const auto [width, height] = canvas.get_dimensions();
  bool has_color             = false;
  std::uint32_t foreground   = 0U;
  std::uint32_t background   = 0U;

  for (std::size_t y = 0U; y < height; ++y) {
    if (y != 0U) {
      output.append("\x1b[");
      if (!append_unsigned(output, y + 1U)) {
        return Status::DIMENSION_OVERFLOW;
      }
      output.append(";1H");
    }

    for (std::size_t x = 0U; x < width; ++x) {
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

Screen::Screen(multithreading::JobQueue& workers)
    : Screen(STDIN_FILENO, STDOUT_FILENO, workers) {}

Screen::Screen(int input_fd, int output_fd, multithreading::JobQueue& workers)
    : owned_terminal_session_(
          std::make_unique<terminal::TerminalSession>(input_fd, output_fd)),
      terminal_session_(owned_terminal_session_.get()),
      owned_directory_(std::make_unique<ipc::Directory>(workers)),
      directory_(owned_directory_.get()),
      owns_channels_(true) {
  setup_status_ = setup_channels(true);
}

Screen::Screen(ipc::Directory& directory, terminal::TerminalSession& session)
    : terminal_session_(&session), directory_(&directory) {
  setup_status_ = setup_channels(false);
}

Screen::~Screen() {
  const Status status = release();
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not enqueue release during Screen destruction: "
                  << status_message(status);
  }

  // Quiesce the command channel while the Directory and its downstream resize
  // route are still alive. TerminalSession may publish one final observation
  // from a command that was already executing.
  if (owns_channels_ && directory_ != nullptr) {
    const ipc::Status close_status =
        directory_->close_channel(msg::kScreenCommandChannel);
    if (!ipc::is_ok(close_status) &&
        close_status != ipc::Status::CHANNEL_NOT_FOUND) {
      Logger<ERROR> << "Could not quiesce Screen commands: "
                    << ipc::status_message(close_status);
    }
    const ipc::Status resize_close_status =
        directory_->close_channel(msg::kScreenResizeEventChannel);
    if (!ipc::is_ok(resize_close_status) &&
        resize_close_status != ipc::Status::CHANNEL_NOT_FOUND) {
      Logger<ERROR> << "Could not quiesce Screen resize events: "
                    << ipc::status_message(resize_close_status);
    }
  }
  if (owns_channels_ && terminal_session_ != nullptr) {
    terminal_session_->unbind_screen_channels();
  }
  resize_subscription_.reset();
  command_channel_.reset();
  resize_channel_.reset();
  directory_ = nullptr;
  owned_directory_.reset();
}

Status Screen::setup_channels(bool create_channels) {
  if (directory_ == nullptr || terminal_session_ == nullptr) {
    return Status::CHANNEL_SETUP_FAILED;
  }

  ipc::ChannelId command_id = 0U;
  ipc::ChannelId resize_id  = 0U;
  ipc::Status ipc_status    = ipc::Status::OK;
  if (create_channels) {
    command_channel_ = std::make_shared<ipc::SmemChannel>(
        std::string{msg::kScreenCommandChannel},
        ipc::kDefaultMaximumMessageBytes,
        ipc::ChannelOptions{.channel_max_depth = kScreenCommandDepth});
    ipc_status = directory_->open_channel(command_channel_, command_id);
    if (!ipc::is_ok(ipc_status)) {
      Logger<ERROR> << "Could not open Screen command channel: "
                    << ipc::status_message(ipc_status);
      return Status::CHANNEL_SETUP_FAILED;
    }

    resize_channel_ = std::make_shared<ipc::SmemChannel>(
        std::string{msg::kScreenResizeEventChannel}, 16U,
        ipc::ChannelOptions{.channel_max_depth = kResizeEventDepth});
    ipc_status = directory_->open_channel(resize_channel_, resize_id);
    if (!ipc::is_ok(ipc_status)) {
      Logger<ERROR> << "Could not open Screen resize channel: "
                    << ipc::status_message(ipc_status);
      return Status::CHANNEL_SETUP_FAILED;
    }
  } else {
    command_channel_ = directory_->get_channel(msg::kScreenCommandChannel);
    resize_channel_  = directory_->get_channel(msg::kScreenResizeEventChannel);
    if (command_channel_ == nullptr || resize_channel_ == nullptr ||
        !ipc::is_ok(directory_->get_channel_id(msg::kScreenCommandChannel,
                                               command_id)) ||
        !ipc::is_ok(directory_->get_channel_id(msg::kScreenResizeEventChannel,
                                               resize_id))) {
      command_channel_.reset();
      resize_channel_.reset();
      Logger<ERROR> << "Lifecycle-owned Screen channels are not registered";
      return Status::CHANNEL_SETUP_FAILED;
    }
  }

  ipc_status = directory_->subscribe(
      resize_id,
      [this](ipc::Channel::Bytes payload) noexcept {
        receive_resize_event(payload);
      },
      resize_subscription_);
  if (!ipc::is_ok(ipc_status)) {
    Logger<ERROR> << "Could not subscribe Screen to resize events: "
                  << ipc::status_message(ipc_status);
    return Status::CHANNEL_SETUP_FAILED;
  }

  const terminal::Status terminal_status =
      terminal_session_->bind_screen_channels(*directory_);
  if (!terminal::is_ok(terminal_status)) {
    Logger<ERROR> << "Could not bind terminal session: "
                  << terminal::status_message(terminal_status);
    return Status::CHANNEL_SETUP_FAILED;
  }
  Logger<INFO> << "Configured asynchronous Screen channels " << command_id
               << " and " << resize_id;
  return Status::OK;
}

Status Screen::send_command(const msg::ScreenCommand& command) noexcept {
  if (!is_ok(setup_status_) || directory_ == nullptr) {
    return Status::CHANNEL_SETUP_FAILED;
  }
  std::vector<std::uint8_t> payload;
  const msg::Status encode_status = command_codec_.serialize(command, payload);
  if (!msg::is_ok(encode_status)) {
    Logger<ERROR> << "Could not encode Screen command: "
                  << msg::status_message(encode_status);
    return Status::MESSAGE_ENCODING_FAILED;
  }
  const ipc::TransferResult transfer =
      directory_->transmit(msg::kScreenCommandChannel, payload);
  if (!ipc::is_ok(transfer.status) || transfer.bytes != payload.size()) {
    Logger<ERROR> << "Screen command was not accepted: "
                  << ipc::status_message(transfer.status);
    return Status::ASYNC_DISPATCH_FAILED;
  }
  return Status::OK;
}

Status Screen::take() noexcept { return take(screen_session_options()); }

Status Screen::take(const msg::ScreenSessionOptions& options) noexcept {
  if (!is_ok(setup_status_)) {
    return setup_status_;
  }
  {
    const std::lock_guard lock(state_mutex_);
    if (terminal_requested_) {
      return Status::OK;
    }
    terminal_requested_ = true;
    latest_size_.reset();
  }

  const msg::ScreenCommand command{
      .data =
          msg::ScreenTakeCommand{
              .options       = options,
              .initial_bytes = std::string{kClearAndHome},
              .final_bytes   = std::string{kResetStyle},
          },
  };
  const Status status = send_command(command);
  if (!is_ok(status)) {
    const std::lock_guard lock(state_mutex_);
    terminal_requested_ = false;
  }
  return status;
}

terminal::Status Screen::read_input(terminal::Decoder& decoder,
                                    std::vector<terminal::Event>& events,
                                    std::size_t& bytes_read,
                                    bool& end_of_input) {
  return terminal_session_->read(decoder, events, bytes_read, end_of_input);
}

Status Screen::handle_mouse_event(
    const terminal::MouseEvent& event, const ZBuffer& z_buffer,
    const std::map<std::string, Canvas::Rect>& frame_layouts) {
  const std::lock_guard lock(selection_mutex_);

  const auto find_frame_rect = [&](std::string_view frame_id,
                                   const std::shared_ptr<Frame>& frame,
                                   Canvas::Rect& rect) {
    for (const ZBuffer::Entry& entry : z_buffer.frames()) {
      if (entry.frame_id != frame_id || entry.frame != frame) {
        continue;
      }
      const auto found = frame_layouts.find(entry.frame_id);
      if (found == frame_layouts.end()) {
        return false;
      }
      rect = found->second;
      return true;
    }
    return false;
  };

  if (event.action == terminal::MouseAction::PRESS) {
    if (event.button != terminal::MouseButton::LEFT) {
      return Status::OK;
    }

    const Status reset_status = selection_state_machine_.reset();
    if (!is_ok(reset_status)) {
      return reset_status;
    }
    pointer_selection_gesture_.reset();

    const ZBuffer::Entry* hit_frame = nullptr;
    const Canvas::Rect* hit_rect    = nullptr;
    for (auto entry = z_buffer.frames().rbegin();
         entry != z_buffer.frames().rend(); ++entry) {
      const auto rect = frame_layouts.find(entry->frame_id);
      if (rect != frame_layouts.end() &&
          contains(rect->second, event.position)) {
        hit_frame = &*entry;
        hit_rect  = &rect->second;
        break;
      }
    }

    if (hit_frame == nullptr || hit_rect == nullptr ||
        !hit_frame->frame->is_selectable()) {
      clear_click_history();
      Logger<DEBUG> << "Selection press hit no selectable frame";
      return Status::OK;
    }

    SelectionPosition anchor;
    const Status coordinate_status =
        local_position(event.position, *hit_rect, anchor);
    if (!is_ok(coordinate_status)) {
      clear_click_history();
      return coordinate_status;
    }
    pointer_selection_gesture_ = PointerSelectionGesture{
        .frame_id = hit_frame->frame_id,
        .frame    = hit_frame->frame,
        .rect     = *hit_rect,
        .press    = event.position,
        .anchor   = anchor,
    };
    return Status::OK;
  }

  if (event.action == terminal::MouseAction::DRAG) {
    if (pointer_selection_gesture_ == std::nullopt ||
        event.button != terminal::MouseButton::LEFT) {
      return Status::OK;
    }

    PointerSelectionGesture& gesture = *pointer_selection_gesture_;
    if (!find_frame_rect(gesture.frame_id, gesture.frame, gesture.rect)) {
      Logger<INFO> << "Captured selection frame left the active layout";
      const Status status = selection_state_machine_.reset();
      pointer_selection_gesture_.reset();
      clear_click_history();
      return status;
    }

    SelectionPosition extent;
    const Status coordinate_status =
        local_position(event.position, gesture.rect, extent);
    if (!is_ok(coordinate_status)) {
      return coordinate_status;
    }
    const Status status = selection_state_machine_.apply(
        gesture.frame_id, gesture.frame,
        SelectionEvent{
            .type   = SelectionEventType::SELECT_AND_EXTEND,
            .anchor = gesture.anchor,
            .extent = extent,
        });
    if (is_ok(status)) {
      gesture.extended = true;
      clear_click_history();
    }
    return status;
  }

  if (event.action != terminal::MouseAction::RELEASE ||
      pointer_selection_gesture_ == std::nullopt) {
    return Status::OK;
  }

  PointerSelectionGesture gesture = *pointer_selection_gesture_;
  pointer_selection_gesture_.reset();
  if (!find_frame_rect(gesture.frame_id, gesture.frame, gesture.rect)) {
    Logger<INFO> << "Released selection after its frame left the active layout";
    clear_click_history();
    return selection_state_machine_.reset();
  }

  SelectionPosition extent;
  const Status coordinate_status =
      local_position(event.position, gesture.rect, extent);
  if (!is_ok(coordinate_status)) {
    clear_click_history();
    return coordinate_status;
  }

  const bool moved = event.position != gesture.press;
  if (gesture.extended || moved) {
    clear_click_history();
    if (!gesture.extended) {
      const Status start_status = selection_state_machine_.apply(
          gesture.frame_id, gesture.frame,
          SelectionEvent{
              .type   = SelectionEventType::SELECT_AND_EXTEND,
              .anchor = gesture.anchor,
              .extent = extent,
          });
      if (!is_ok(start_status)) {
        return start_status;
      }
    }
    return selection_state_machine_.apply(
        gesture.frame_id, gesture.frame,
        SelectionEvent{
            .type   = SelectionEventType::END_SELECT_AND_EXTEND,
            .anchor = gesture.anchor,
            .extent = extent,
        });
  }

  std::size_t click_count = 1U;
  if (click_history_.has_value() &&
      click_history_->frame_id == gesture.frame_id &&
      click_history_->frame == gesture.frame &&
      click_history_->position == event.position &&
      click_history_->modifiers == event.modifiers) {
    click_count = click_history_->count + 1U;
  }

  if (click_count == 1U) {
    if (gesture.frame->accepts_cursor_placement()) {
      const Status status = gesture.frame->place_cursor(extent);
      if (!is_ok(status)) {
        clear_click_history();
        return status;
      }
    }
    click_history_ = ClickHistory{
        .frame_id  = gesture.frame_id,
        .frame     = gesture.frame,
        .position  = event.position,
        .modifiers = event.modifiers,
        .count     = click_count,
    };
    arm_click_timeout();
    return Status::OK;
  }

  const SelectionEventType type = click_count == 2U
                                      ? SelectionEventType::SELECT_WORD
                                      : SelectionEventType::SELECT_LINE;
  const Status status           = selection_state_machine_.apply(
      gesture.frame_id, gesture.frame,
      SelectionEvent{.type = type, .anchor = extent, .extent = extent});
  if (!is_ok(status) || click_count >= 3U) {
    clear_click_history();
  } else {
    click_history_ = ClickHistory{
        .frame_id  = gesture.frame_id,
        .frame     = gesture.frame,
        .position  = event.position,
        .modifiers = event.modifiers,
        .count     = click_count,
    };
    arm_click_timeout();
  }
  return status;
}

Status Screen::select_all(std::string_view frame_id,
                          const std::shared_ptr<Frame>& frame) {
  const std::lock_guard lock(selection_mutex_);
  pointer_selection_gesture_.reset();
  clear_click_history();
  return selection_state_machine_.apply(
      frame_id, frame, SelectionEvent{.type = SelectionEventType::SELECT_ALL});
}

void Screen::clear_click_history() noexcept {
  click_history_.reset();
  selection_timeout_.reset();
}

void Screen::arm_click_timeout() noexcept {
  ++next_click_timeout_generation_;
  if (next_click_timeout_generation_ == 0U) {
    ++next_click_timeout_generation_;
  }
  selection_timeout_ = terminal::TimeoutInput{
      .generation = next_click_timeout_generation_,
  };
}

std::optional<terminal::TimeoutInput> Screen::pending_selection_timeout()
    const {
  const std::lock_guard lock(selection_mutex_);
  return selection_timeout_;
}

Status Screen::handle_selection_timeout(terminal::TimeoutInput input) noexcept {
  const std::lock_guard lock(selection_mutex_);
  if (selection_timeout_.has_value() && input.generation != 0U &&
      input == *selection_timeout_) {
    clear_click_history();
  }
  return Status::OK;
}

Status Screen::reset_selection() {
  const std::lock_guard lock(selection_mutex_);
  const Status status = selection_state_machine_.reset();
  if (is_ok(status)) {
    pointer_selection_gesture_.reset();
    clear_click_history();
  }
  return status;
}

SelectionPhase Screen::selection_phase() const noexcept {
  const std::lock_guard lock(selection_mutex_);
  return selection_state_machine_.phase();
}

std::optional<std::string> Screen::selected_frame_id() const {
  const std::lock_guard lock(selection_mutex_);
  return selection_state_machine_.active_frame_id();
}

Status Screen::selected_text(std::string& output) const {
  const std::lock_guard lock(selection_mutex_);
  return selection_state_machine_.selected_text(output);
}

Status Screen::copy_selection(
    msg::ScreenClipboardSelection selection) noexcept {
  std::string text;
  {
    const std::lock_guard lock(selection_mutex_);
    const Status status = selection_state_machine_.selected_text(text);
    if (!is_ok(status)) {
      return status;
    }
  }
  return send_command(msg::ScreenCommand{
      .data =
          msg::ScreenSetClipboardCommand{
              .selection = selection,
              .text      = std::move(text),
          },
  });
}

Status Screen::release() noexcept {
  {
    const std::lock_guard lock(state_mutex_);
    if (!terminal_requested_) {
      return Status::OK;
    }
    terminal_requested_ = false;
    latest_size_.reset();
  }

  const Status status =
      send_command(msg::ScreenCommand{.data = msg::ScreenReleaseCommand{}});
  if (!is_ok(status)) {
    const std::lock_guard lock(state_mutex_);
    terminal_requested_ = true;
  }
  return status;
}

Status Screen::get_dimensions(std::size_t& width,
                              std::size_t& height) const noexcept {
  CellDimensions ignored;
  return get_dimensions(width, height, ignored);
}

Status Screen::get_dimensions(std::size_t& width, std::size_t& height,
                              CellDimensions& cell_dimensions) const noexcept {
  msg::ScreenResizeEvent size;
  {
    const std::lock_guard lock(state_mutex_);
    if (!latest_size_.has_value()) {
      width           = 0U;
      height          = 0U;
      cell_dimensions = kDefaultCellDimensions;
      return Status::TERMINAL_QUERY_FAILED;
    }
    size = *latest_size_;
  }

  width           = size.width;
  height          = size.height;
  cell_dimensions = kDefaultCellDimensions;
  if (size.pixel_width != 0U && size.pixel_height != 0U) {
    const std::size_t relative_width =
        static_cast<std::size_t>(size.pixel_width) * height;
    const std::size_t relative_height =
        static_cast<std::size_t>(size.pixel_height) * width;
    const std::size_t common_factor = std::gcd(relative_width, relative_height);
    cell_dimensions.width           = relative_width / common_factor;
    cell_dimensions.height          = relative_height / common_factor;
  }
  return Status::OK;
}

Status Screen::set_canvas(std::shared_ptr<Canvas> canvas) noexcept {
  if (canvas == nullptr) {
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

Status Screen::draw() noexcept {
  if (!is_taken()) {
    Logger<ERROR> << "Cannot draw screen without requested terminal ownership";
    return Status::TERMINAL_NOT_AVAILABLE;
  }
  if (canvas_ == nullptr) {
    Logger<ERROR> << "Cannot draw screen: "
                  << status_message(Status::CANVAS_NOT_SET);
    return Status::CANVAS_NOT_SET;
  }

  std::string output;
  const Status render_status = render_canvas(*canvas_, output);
  if (!is_ok(render_status)) {
    Logger<ERROR> << "Could not render canvas: "
                  << status_message(render_status);
    return render_status;
  }
  const Status status = send_command(msg::ScreenCommand{
      .data = msg::ScreenPresentCommand{.bytes = std::move(output)},
  });
  if (is_ok(status)) {
    Logger<DEBUG> << "Enqueued canvas with "
                  << canvas_->get_drawable_buffer().size() << " cells";
  }
  return status;
}

void Screen::receive_resize_event(ipc::Channel::Bytes payload) noexcept {
  msg::ScreenResizeEvent event;
  const msg::Status status = resize_codec_.deserialize(payload, event);
  if (!msg::is_ok(status)) {
    Logger<ERROR> << "Discarded malformed resize event: "
                  << msg::status_message(status);
    return;
  }
  const std::lock_guard lock(state_mutex_);
  if (terminal_requested_) {
    latest_size_ = event;
    Logger<INFO> << "Observed terminal geometry " << event.width << 'x'
                 << event.height;
  }
}

bool Screen::is_taken() const noexcept {
  const std::lock_guard lock(state_mutex_);
  return terminal_requested_;
}

std::size_t Screen::pending_commands() const noexcept {
  return command_channel_ == nullptr ? 0U
                                     : command_channel_->pending_messages();
}

std::uint64_t Screen::dropped_commands() const noexcept {
  return command_channel_ == nullptr ? 0U
                                     : command_channel_->dropped_messages();
}

}  // namespace puc::tui
