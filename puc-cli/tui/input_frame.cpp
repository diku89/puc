/**
 * @file input_frame.cpp
 * @brief Composition and mode orchestration for the input-frame stack.
 */

#include "puc-cli/tui/input_frame.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "puc-cli/tui/annotated_text_frame.hpp"
#include "puc-cli/tui/bounding_frame.hpp"
#include "puc-cli/tui/cmd_frame.hpp"
#include "puc-cli/tui/integrated_term_frame.hpp"
#include "puc-cli/tui/text_input_frame.hpp"

namespace puc::tui {
namespace {

/** Return whether a named key only reports modifier state. */
constexpr bool is_modifier_key(terminal::NamedKey key) noexcept {
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

/** Return the normal or command outer bounding configuration. */
BoundingFrameConfiguration editor_bounds(Theme::ColorTypes border) {
  return BoundingFrameConfiguration{
      .outer_margins     = {.top = 1U, .bottom = 1U},
      .inner_margins     = {},
      .outside_box_color = Theme::ColorTypes::BACKGROUND,
      .inside_color      = Theme::ColorTypes::SECONDARY,
      .border_color      = border,
      .size_constraints =
          FrameSizeConstraints{
              .minimum_width             = InputFrame::kMinimumWidth,
              .minimum_height            = InputFrame::kMinimumHeight,
              .require_full_canvas_width = true,
          },
  };
}

/** Return terminal-mode bounds with the legacy three-cell left inset. */
BoundingFrameConfiguration terminal_bounds() {
  return BoundingFrameConfiguration{
      .outer_margins     = {.top = 1U, .bottom = 1U},
      .inner_margins     = {.left = 3U},
      .outside_box_color = Theme::ColorTypes::BACKGROUND,
      .inside_color      = Theme::ColorTypes::SECONDARY,
      .border_color      = Theme::ColorTypes::TERTIARY,
      .size_constraints =
          FrameSizeConstraints{
              .minimum_width             = InputFrame::kMinimumWidth,
              .minimum_height            = InputFrame::kTerminalMinimumHeight,
              .require_full_canvas_width = true,
          },
  };
}

/** Write one UTF-32 row into a bounded rectangle. */
Status write_text_row(Canvas& canvas, std::size_t x, std::size_t y,
                      std::size_t maximum_width, std::u32string_view text,
                      std::uint32_t foreground, std::uint32_t background) {
  const std::size_t count = std::min(maximum_width, text.size());
  if (count == 0U) {
    return Status::OK;
  }
  std::vector<Canvas::Cell> cells;
  cells.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    cells.push_back(Canvas::Cell{
        .character        = text[index],
        .foreground_color = foreground,
        .background_color = background,
    });
  }
  std::span<Canvas::Cell> row{cells};
  std::span<std::span<Canvas::Cell>> rows{&row, 1U};
  return canvas.write_cells(
      Canvas::Rect{.x = x, .y = y, .width = count, .height = 1U}, rows);
}

}  // namespace

/** Synchronized mode coordinator and its independently reusable child frames.
 */
class InputFrame::Impl {
 public:
  Impl()
      : normal_input(std::make_shared<TextInputFrame>("normal input")),
        normal_annotated(std::make_shared<AnnotatedTextFrame>(
            "annotated normal input", normal_input,
            AnnotatedTextConfiguration{
                .show_line_numbers           = true,
                .minimum_line_number_columns = 2U,
                .status_columns              = 0U,
                .separator_columns           = 1U,
                .minimum_gutter_width        = 0U,
                .background_color            = Theme::ColorTypes::SECONDARY,
                .line_number_color           = Theme::ColorTypes::TEXT_MUTED,
            })),
        command(std::make_shared<CmdFrame>()),
        terminal(std::make_shared<IntegratedTermFrame>()),
        normal_bounding(std::make_shared<BoundingFrame>(
            "normal input bounds", normal_annotated,
            editor_bounds(Theme::ColorTypes::TEXT))),
        command_bounding(std::make_shared<BoundingFrame>(
            "command input bounds", command,
            editor_bounds(Theme::ColorTypes::TEXT_SUCCESS))),
        terminal_bounding(std::make_shared<BoundingFrame>(
            "integrated terminal bounds", terminal, terminal_bounds())) {}

  /** Return whether the active editor has a paste transaction. */
  bool paste_in_progress() const {
    if (mode == InputMode::COMMAND) {
      return command->snapshot().paste_in_progress;
    }
    return mode == InputMode::NORMAL &&
           normal_input->snapshot().paste_in_progress;
  }

  /** Route one event to the active editable child. */
  Status route_editor_event(const terminal::Event& event) {
    return mode == InputMode::COMMAND ? command->handle_event(event)
                                      : normal_input->handle_event(event);
  }

  /** Return the state of the active or preserved normal editor. */
  TextEditorSnapshot active_editor_snapshot() const {
    return mode == InputMode::COMMAND ? command->snapshot()
                                      : normal_input->snapshot();
  }

  /** Return the active complete decorated view. */
  std::shared_ptr<BoundingFrame> active_bounding() const noexcept {
    switch (mode) {
      case InputMode::NORMAL:
        return normal_bounding;
      case InputMode::COMMAND:
        return command_bounding;
      case InputMode::TERMINAL:
        return terminal_bounding;
    }
    return normal_bounding;
  }

  /** Enter a fresh disposable command editor. */
  void enter_command_mode() {
    command->clear();
    mode = InputMode::COMMAND;
    terminal_visible.store(false, std::memory_order_relaxed);
    escape_started.reset();
  }

  /** Show the terminal and request a PTY owner when necessary. */
  void enter_terminal_mode() {
    terminal->activate_session();
    mode = InputMode::TERMINAL;
    terminal_visible.store(true, std::memory_order_relaxed);
    escape_started.reset();
  }

  /** Apply a high-level mode or editor command. */
  Status apply_command(terminal::Command input_command) {
    if (input_command == terminal::Command::ENTER_COMMAND_MODE) {
      enter_command_mode();
      return Status::OK;
    }
    if (input_command == terminal::Command::ENTER_TERMINAL_MODE) {
      enter_terminal_mode();
      return Status::OK;
    }
    if (mode == InputMode::TERMINAL ||
        input_command == terminal::Command::COPY ||
        input_command == terminal::Command::SELECT_ALL) {
      return Status::OK;
    }
    return route_editor_event(
        terminal::Event{terminal::CommandEvent{.command = input_command}});
  }

  /** Recognize one Escape or a decoder-normalized double Escape. */
  void handle_escape(Clock::time_point now, bool decoder_double_escape) {
    if (mode == InputMode::TERMINAL) {
      return;
    }
    const bool within_interval = escape_started.has_value() &&
                                 now >= *escape_started &&
                                 now - *escape_started < kDoubleEscapeInterval;
    if (decoder_double_escape || within_interval) {
      if (mode == InputMode::COMMAND) {
        mode = InputMode::NORMAL;
        terminal_visible.store(false, std::memory_order_relaxed);
      } else {
        normal_input->clear();
      }
      escape_started.reset();
      return;
    }
    escape_started = now;
  }

  /** Insert committed text while recognizing Escape mode fallbacks. */
  Status handle_committed_text(std::string_view text, Clock::time_point now) {
    const bool escape_chord =
        mode == InputMode::NORMAL && escape_started.has_value() &&
        now >= *escape_started &&
        now - *escape_started < kDoubleEscapeInterval && !text.empty() &&
        (text.front() == ':' || text.front() == '>');
    if (escape_chord) {
      if (text.front() == ':') {
        enter_command_mode();
      } else {
        enter_terminal_mode();
      }
      text.remove_prefix(1U);
    } else {
      escape_started.reset();
    }
    if (mode != InputMode::TERMINAL && !text.empty()) {
      return route_editor_event(
          terminal::Event{terminal::TextEvent{.utf8 = std::string{text}}});
    }
    return Status::OK;
  }

  /** Apply one decoded key with Escape chords handled above the editor model.
   */
  Status handle_key(const terminal::KeyEvent& event, Clock::time_point now) {
    if (event.action == terminal::KeyAction::RELEASE || paste_in_progress()) {
      return Status::OK;
    }
    const auto* named = std::get_if<terminal::NamedKey>(&event.key.value);
    if (named != nullptr && *named == terminal::NamedKey::ESCAPE) {
      handle_escape(now, event.modifiers.contains(terminal::Modifier::ALT));
      return Status::OK;
    }
    if (named != nullptr && is_modifier_key(*named)) {
      return Status::OK;
    }
    if (mode == InputMode::TERMINAL) {
      escape_started.reset();
      return Status::OK;
    }
    if (named != nullptr) {
      escape_started.reset();
      return route_editor_event(terminal::Event{event});
    }
    if (!event.text.empty()) {
      return handle_committed_text(event.text, now);
    }
    const auto* character = std::get_if<char32_t>(&event.key.value);
    if (character == nullptr) {
      escape_started.reset();
      return Status::OK;
    }
    if (event.modifiers.contains(terminal::Modifier::CONTROL) ||
        event.modifiers.contains(terminal::Modifier::SUPER) ||
        event.modifiers.contains(terminal::Modifier::HYPER) ||
        event.modifiers.contains(terminal::Modifier::META)) {
      escape_started.reset();
      return route_editor_event(terminal::Event{event});
    }
    std::string utf8;
    text_editor::append_utf8(event.shifted_key.value_or(*character), utf8);
    return handle_committed_text(utf8, now);
  }

  mutable std::shared_mutex mutex; /**< Synchronizes mode and Escape state. */
  std::shared_ptr<TextInputFrame> normal_input; /**< Preserved normal editor. */
  std::shared_ptr<AnnotatedTextFrame>
      normal_annotated;              /**< Numbered normal editor. */
  std::shared_ptr<CmdFrame> command; /**< Disposable green command view. */
  std::shared_ptr<IntegratedTermFrame> terminal; /**< Persistent libtmt view. */
  std::shared_ptr<BoundingFrame> normal_bounding;   /**< White normal box. */
  std::shared_ptr<BoundingFrame> command_bounding;  /**< Green command box. */
  std::shared_ptr<BoundingFrame> terminal_bounding; /**< Purple terminal box. */
  InputMode mode = InputMode::NORMAL;        /**< Active composed view. */
  std::atomic_bool terminal_visible = false; /**< Lock-free capability state. */
  std::optional<Clock::time_point>
      escape_started;       /**< Time at which the first Escape arrived. */
  std::string notification; /**< UTF-8 notification-margin contents. */
};

InputFrame::InputFrame(std::string name)
    : Frame(std::move(name)), impl_(std::make_unique<Impl>()) {}

InputFrame::~InputFrame() = default;

std::size_t InputFrame::maximum_height(std::size_t screen_height) noexcept {
  return std::min(screen_height,
                  std::max(kBaselineMaximumHeight, screen_height / 5U));
}

std::size_t InputFrame::terminal_height(std::size_t screen_height) noexcept {
  const std::size_t sixty_percent =
      (screen_height / 5U) * 3U + ((screen_height % 5U) * 3U) / 5U;
  return std::min(screen_height,
                  std::max(kTerminalMinimumHeight, sixty_percent));
}

std::size_t InputFrame::minimum_height() const noexcept {
  return impl_->terminal_visible.load(std::memory_order_relaxed)
             ? kTerminalMinimumHeight
             : kMinimumHeight;
}

std::size_t InputFrame::preferred_height(std::size_t screen_width,
                                         std::size_t screen_height) const {
  if (screen_height == 0U) {
    return 0U;
  }
  const std::shared_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    return terminal_height(screen_height);
  }

  const std::size_t bounded_width = screen_width > 2U ? screen_width - 2U : 0U;
  std::size_t content_rows        = 0U;
  if (impl_->mode == InputMode::COMMAND) {
    content_rows = impl_->command->preferred_rows(bounded_width);
  } else {
    const std::size_t gutter = impl_->normal_annotated->gutter_width();
    const std::size_t text_width =
        bounded_width > gutter ? bounded_width - gutter : 1U;
    content_rows = impl_->normal_input->preferred_rows(text_width);
  }
  const std::size_t desired =
      content_rows > std::numeric_limits<std::size_t>::max() - 4U
          ? std::numeric_limits<std::size_t>::max()
          : content_rows + 4U;
  const std::size_t maximum = maximum_height(screen_height);
  if (maximum < kMinimumHeight) {
    return maximum;
  }
  return std::min(maximum, std::max(kMinimumHeight, desired));
}

Status InputFrame::handle_event(const terminal::Event& event,
                                Clock::time_point now) {
  const std::unique_lock lock(impl_->mutex);
  if (const auto* key = std::get_if<terminal::KeyEvent>(&event)) {
    return impl_->handle_key(*key, now);
  }
  if (const auto* text = std::get_if<terminal::TextEvent>(&event)) {
    if (!impl_->paste_in_progress()) {
      return impl_->handle_committed_text(text->utf8, now);
    }
    return Status::OK;
  }
  if (const auto* paste = std::get_if<terminal::PasteEvent>(&event)) {
    if (impl_->mode == InputMode::TERMINAL) {
      impl_->escape_started.reset();
      return Status::OK;
    }
    if (paste->phase == terminal::PastePhase::BEGIN) {
      impl_->escape_started.reset();
    }
    return impl_->route_editor_event(event);
  }
  if (std::holds_alternative<terminal::ScrollEvent>(event)) {
    impl_->escape_started.reset();
    return impl_->mode == InputMode::TERMINAL
               ? Status::OK
               : impl_->route_editor_event(event);
  }
  if (const auto* command = std::get_if<terminal::CommandEvent>(&event)) {
    if (!impl_->paste_in_progress()) {
      impl_->escape_started.reset();
      return impl_->apply_command(command->command);
    }
  }
  return Status::OK;
}

Status InputFrame::handle_event(const terminal::Event& event) {
  return handle_event(event, Clock::now());
}

void InputFrame::advance_time(Clock::time_point now) {
  const std::unique_lock lock(impl_->mutex);
  if (impl_->escape_started.has_value() && now >= *impl_->escape_started &&
      now - *impl_->escape_started >= kDoubleEscapeInterval) {
    impl_->escape_started.reset();
  }
}

void InputFrame::set_notification(std::string notification) {
  const std::unique_lock lock(impl_->mutex);
  impl_->notification = std::move(notification);
}

Status InputFrame::write_terminal(std::string_view output) {
  return impl_->terminal->write(output);
}

std::string InputFrame::take_terminal_responses() {
  return impl_->terminal->take_responses();
}

void InputFrame::reset_terminal() { impl_->terminal->reset(); }

void InputFrame::start_new_terminal() {
  const std::unique_lock lock(impl_->mutex);
  impl_->terminal->start_new_session();
  impl_->mode = InputMode::TERMINAL;
  impl_->terminal_visible.store(true, std::memory_order_relaxed);
  impl_->escape_started.reset();
}

void InputFrame::close_terminal() {
  const std::unique_lock lock(impl_->mutex);
  impl_->terminal->close_session();
  if (impl_->mode == InputMode::TERMINAL) {
    impl_->mode = InputMode::NORMAL;
    impl_->terminal_visible.store(false, std::memory_order_relaxed);
  }
  impl_->escape_started.reset();
}

void InputFrame::clear() {
  const std::unique_lock lock(impl_->mutex);
  switch (impl_->mode) {
    case InputMode::NORMAL:
      impl_->normal_input->clear();
      break;
    case InputMode::COMMAND:
      impl_->command->clear();
      break;
    case InputMode::TERMINAL:
      impl_->terminal->reset();
      break;
  }
  impl_->escape_started.reset();
}

InputFrameSnapshot InputFrame::snapshot() const {
  const std::shared_lock lock(impl_->mutex);
  const TextEditorSnapshot normal  = impl_->normal_input->snapshot();
  const TextEditorSnapshot command = impl_->command->snapshot();
  const TextEditorSnapshot& active =
      impl_->mode == InputMode::COMMAND ? command : normal;
  const IntegratedTermFrameSnapshot terminal = impl_->terminal->snapshot();
  return InputFrameSnapshot{
      .mode                    = impl_->mode,
      .input_text              = normal.text,
      .command_text            = command.text,
      .cursor                  = active.cursor,
      .scroll_row              = active.scroll_row,
      .escape_armed            = impl_->escape_started.has_value(),
      .paste_in_progress       = active.paste_in_progress,
      .terminal_rows           = terminal.rows,
      .terminal_columns        = terminal.columns,
      .terminal_cursor_visible = terminal.cursor_visible,
      .terminal_session_active = terminal.session_active,
      .terminal_generation     = terminal.generation,
  };
}

Status InputFrame::draw(const Theme& theme, Canvas& canvas,
                        const Canvas::Rect& rect) {
  const auto [screen_width, screen_height] = canvas.get_dimensions();
  if (rect.x != 0U || rect.width != screen_width ||
      rect.width < kMinimumWidth || rect.height < kMinimumHeight) {
    return Status::INVALID_DIMENSIONS;
  }
  if (rect.y > screen_height || rect.height > screen_height - rect.y) {
    return Status::RECT_OUT_OF_BOUNDS;
  }

  const std::unique_lock lock(impl_->mutex);
  const bool terminal_mode = impl_->mode == InputMode::TERMINAL;
  const std::size_t minimum =
      terminal_mode ? kTerminalMinimumHeight : kMinimumHeight;
  const std::size_t maximum = terminal_mode ? terminal_height(screen_height)
                                            : maximum_height(screen_height);
  if (rect.height < minimum || rect.height > maximum) {
    return Status::INVALID_DIMENSIONS;
  }
  std::shared_ptr<BoundingFrame> active = impl_->active_bounding();
  active->set_size_constraints(FrameSizeConstraints{
      .minimum_width             = kMinimumWidth,
      .minimum_height            = minimum,
      .maximum_height            = maximum,
      .require_full_canvas_width = true,
  });
  Status status = active->draw(theme, canvas, rect);
  if (!is_ok(status)) {
    return status;
  }

  const Theme::Colors colors = theme.get_colors();
  if (impl_->escape_started.has_value()) {
    const std::string_view prompt =
        impl_->mode == InputMode::COMMAND ? kExitCommandPrompt : kClearPrompt;
    status = write_text_row(canvas, rect.x + 2U, rect.y + rect.height - 2U,
                            rect.width > 4U ? rect.width - 4U : 0U,
                            text_editor::decode_utf8(prompt), colors.text_muted,
                            colors.background);
    if (!is_ok(status)) {
      return status;
    }
  }

  return write_text_row(canvas, rect.x, rect.y + rect.height - 1U, rect.width,
                        text_editor::decode_utf8(impl_->notification),
                        colors.text_muted, colors.background);
}

bool InputFrame::is_selectable() const noexcept {
  return !impl_->terminal_visible.load(std::memory_order_relaxed);
}

Status InputFrame::update_selection(const SelectionEvent& event) {
  const std::unique_lock lock(impl_->mutex);
  impl_->escape_started.reset();
  if (impl_->mode == InputMode::TERMINAL) {
    if (event.type != SelectionEventType::RESET) {
      return Status::FRAME_NOT_SELECTABLE;
    }
    const Status normal  = impl_->normal_input->update_selection(event);
    const Status command = impl_->command->update_selection(event);
    return !is_ok(normal) ? normal : command;
  }
  return impl_->active_bounding()->update_selection(event);
}

Status InputFrame::selected_text(std::string& output) const {
  const std::shared_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    output.clear();
    return Status::FRAME_NOT_SELECTABLE;
  }
  return impl_->active_bounding()->selected_text(output);
}

bool InputFrame::accepts_cursor_placement() const noexcept {
  return !impl_->terminal_visible.load(std::memory_order_relaxed);
}

Status InputFrame::place_cursor(SelectionPosition position) {
  const std::unique_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    return Status::FRAME_NOT_SELECTABLE;
  }
  impl_->escape_started.reset();
  return impl_->active_bounding()->place_cursor(position);
}

}  // namespace puc::tui
