/**
 * @file integrated_term_frame.cpp
 * @brief Persistent libtmt surface and lifecycle implementation.
 */

#include "puc-cli/tui/frames/integrated_term_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include "tmt.h"
}

namespace puc::tui {
namespace {

/** Unicode alternate-character-set glyphs in libtmt's documented order. */
constexpr wchar_t kTerminalAcs[] = L"→←↑↓■◆▒°±▒┘┐┌└┼⎺───⎽├┤┴┬│≤≥π≠£•";

/** Convert libtmt's platform wchar_t into a Canvas Unicode scalar. */
constexpr char32_t terminal_character(wchar_t character) noexcept {
  const auto scalar = static_cast<std::uint32_t>(character);
  if (scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
    return U'\ufffd';
  }
  return static_cast<char32_t>(scalar);
}

/** Resolve one libtmt ANSI color through the active PUC palette. */
std::uint32_t terminal_color(tmt_color_t color_value,
                             std::uint32_t default_color,
                             const Theme::Colors& colors) noexcept {
  switch (color_value) {
    case TMT_COLOR_DEFAULT:
      return default_color;
    case TMT_COLOR_BLACK:
      return colors.background;
    case TMT_COLOR_RED:
      return colors.text_error;
    case TMT_COLOR_GREEN:
      return colors.text_success;
    case TMT_COLOR_YELLOW:
      return colors.text_warning;
    case TMT_COLOR_BLUE:
      return colors.primary;
    case TMT_COLOR_MAGENTA:
      return colors.tertiary;
    case TMT_COLOR_CYAN:
      return colors.text_info;
    case TMT_COLOR_WHITE:
      return colors.text;
    case TMT_COLOR_MAX:
      return default_color;
  }
  return default_color;
}

}  // namespace

/** Synchronized libtmt object, pending bytes, and process-owner lifecycle. */
class IntegratedTermFrame::Impl {
 public:
  /** Store the initial terminal rendering policy. */
  explicit Impl(IntegratedTermFrameStyle supplied_style)
      : style(supplied_style) {}

  /** Receive replies and cursor visibility synchronously from libtmt. */
  static void callback(tmt_msg_t message, TMT*, const void* result,
                       void* context) {
    auto& impl = *static_cast<Impl*>(context);
    if (message == TMT_MSG_ANSWER && result != nullptr) {
      impl.responses.append(static_cast<const char*>(result));
    } else if (message == TMT_MSG_CURSOR && result != nullptr) {
      impl.cursor_visible = *static_cast<const char*>(result) == 't';
    }
  }

  /** Allocate or resize the persistent terminal to exact dimensions. */
  bool ensure_terminal(std::size_t requested_rows,
                       std::size_t requested_columns) {
    if (requested_rows < 2U || requested_columns < 2U) {
      return false;
    }
    if (terminal == nullptr) {
      terminal = tmt_open(requested_rows, requested_columns, &Impl::callback,
                          this, kTerminalAcs);
      if (terminal == nullptr) {
        return false;
      }
      rows    = requested_rows;
      columns = requested_columns;
      if (!pending_output.empty()) {
        tmt_write(terminal, pending_output.data(), pending_output.size());
        pending_output.clear();
      }
      return true;
    }
    if (rows == requested_rows && columns == requested_columns) {
      return true;
    }
    if (!tmt_resize(terminal, requested_rows, requested_columns)) {
      return false;
    }
    rows    = requested_rows;
    columns = requested_columns;
    return true;
  }

  /** Destroy the emulated screen and all stale display data. */
  void destroy_terminal() {
    if (terminal != nullptr) {
      tmt_close(terminal);
      terminal = nullptr;
    }
    rows           = 0U;
    columns        = 0U;
    cursor_visible = true;
    pending_output.clear();
    responses.clear();
  }

  /** Request a distinct external process owner. */
  void begin_new_session(bool preserve_pending_output) {
    std::string pending;
    if (preserve_pending_output) {
      pending = std::move(pending_output);
    }
    destroy_terminal();
    pending_output = std::move(pending);
    session_active = true;
    generation     = generation == std::numeric_limits<std::size_t>::max()
                         ? 1U
                         : generation + 1U;
  }

  mutable std::shared_mutex mutex; /**< Synchronizes libtmt and lifecycle. */
  IntegratedTermFrameStyle style;  /**< Default terminal semantic colors. */
  TMT* terminal       = nullptr;   /**< Persistent libtmt object. */
  std::size_t rows    = 0U;        /**< Current libtmt rows. */
  std::size_t columns = 0U;        /**< Current libtmt columns. */
  bool cursor_visible = true;      /**< Last DECTCEM visibility state. */
  std::string pending_output;      /**< Bytes queued before allocation. */
  std::string responses;           /**< Replies waiting for the PTY owner. */
  bool session_active    = false;  /**< Whether an owner should live. */
  std::size_t generation = 0U;     /**< Requested session identity. */
};

IntegratedTermFrame::IntegratedTermFrame(std::string name,
                                         IntegratedTermFrameStyle style)
    : Frame(std::move(name)), impl_(std::make_unique<Impl>(style)) {}

IntegratedTermFrame::~IntegratedTermFrame() {
  const std::unique_lock lock(impl_->mutex);
  impl_->destroy_terminal();
}

Status IntegratedTermFrame::write(std::string_view output) {
  const std::unique_lock lock(impl_->mutex);
  if (impl_->terminal == nullptr) {
    impl_->pending_output.append(output);
  } else {
    tmt_write(impl_->terminal, output.data(), output.size());
  }
  return Status::OK;
}

std::string IntegratedTermFrame::take_responses() {
  const std::unique_lock lock(impl_->mutex);
  std::string responses = std::move(impl_->responses);
  impl_->responses.clear();
  return responses;
}

void IntegratedTermFrame::reset() {
  const std::unique_lock lock(impl_->mutex);
  impl_->pending_output.clear();
  if (impl_->terminal != nullptr) {
    tmt_reset(impl_->terminal);
  }
}

void IntegratedTermFrame::activate_session() {
  const std::unique_lock lock(impl_->mutex);
  if (!impl_->session_active) {
    impl_->begin_new_session(impl_->generation == 0U);
  }
}

void IntegratedTermFrame::start_new_session() {
  const std::unique_lock lock(impl_->mutex);
  impl_->begin_new_session(false);
}

void IntegratedTermFrame::close_session() {
  const std::unique_lock lock(impl_->mutex);
  impl_->destroy_terminal();
  impl_->session_active = false;
}

IntegratedTermFrameSnapshot IntegratedTermFrame::snapshot() const {
  const std::shared_lock lock(impl_->mutex);
  return IntegratedTermFrameSnapshot{
      .rows           = impl_->rows,
      .columns        = impl_->columns,
      .cursor_visible = impl_->cursor_visible,
      .session_active = impl_->session_active,
      .generation     = impl_->generation,
  };
}

Status IntegratedTermFrame::draw(const Theme& theme, Canvas& canvas,
                                 const Canvas::Rect& rect) {
  if (rect.width < 2U || rect.height < 2U) {
    return Status::INVALID_DIMENSIONS;
  }
  const std::unique_lock lock(impl_->mutex);
  if (!impl_->ensure_terminal(rect.height, rect.width)) {
    return Status::TERMINAL_CONFIG_FAILED;
  }

  const Theme::Colors colors = theme.get_colors();
  const std::uint32_t default_foreground =
      theme.get_color(impl_->style.text_color);
  const std::uint32_t default_background =
      theme.get_color(impl_->style.background_color);
  const std::uint32_t cursor_color = theme.get_color(impl_->style.cursor_color);
  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height, std::vector<Canvas::Cell>(
                       rect.width, Canvas::Cell{
                                       .character        = U' ',
                                       .foreground_color = default_foreground,
                                       .background_color = default_background,
                                   }));

  const TMTSCREEN* screen = tmt_screen(impl_->terminal);
  for (std::size_t row = 0U; row < screen->nline; ++row) {
    for (std::size_t column = 0U; column < screen->ncol; ++column) {
      const TMTCHAR& source = screen->lines[row]->chars[column];
      std::uint32_t foreground =
          terminal_color(source.a.fg, default_foreground, colors);
      std::uint32_t background =
          terminal_color(source.a.bg, default_background, colors);
      if (source.a.bold && source.a.fg == TMT_COLOR_DEFAULT &&
          colors.text_emphasis != 0U) {
        foreground = colors.text_emphasis;
      }
      if (source.a.dim && colors.text_muted != 0U) {
        foreground = colors.text_muted;
      }
      if (source.a.reverse) {
        std::swap(foreground, background);
      }
      if (source.a.invisible) {
        foreground = background;
      }
      cells[row][column] = Canvas::Cell{
          .character        = terminal_character(source.c),
          .foreground_color = foreground,
          .background_color = background,
      };
    }
  }

  const TMTPOINT* cursor = tmt_cursor(impl_->terminal);
  if (impl_->cursor_visible && cursor->r < rect.height &&
      cursor->c < rect.width) {
    Canvas::Cell& cursor_cell    = cells[cursor->r][cursor->c];
    cursor_cell.foreground_color = cursor_cell.background_color;
    cursor_cell.background_color = cursor_color;
  }
  tmt_clean(impl_->terminal);

  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
}

}  // namespace puc::tui
