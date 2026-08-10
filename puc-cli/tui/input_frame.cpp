/**
 * @file input_frame.cpp
 * @brief Editing, selection, scrolling, and rendering for InputFrame.
 */

#include "puc-cli/tui/input_frame.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

extern "C" {
#include "tmt.h"
}

namespace puc::tui {
namespace {

/** Number of spaces between tab stops in the logical input buffer. */
constexpr std::size_t kTabWidth = 4U;
/** First frame-local row occupied by the editable inner input. */
constexpr std::size_t kInnerTop = 2U;
/** Fixed gutter used by the terminal surface, including its final spacer. */
constexpr std::size_t kTerminalGutterWidth = 4U;
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

/** Return whether a named key only changes modifier state. */
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

/** Return whether `left` precedes `right` in logical reading order. */
constexpr bool position_less(const InputCursor& left,
                             const InputCursor& right) noexcept {
  return left.line < right.line ||
         (left.line == right.line && left.column < right.column);
}

/** Return whether a Unicode scalar is treated as part of an editor word. */
bool is_word_character(char32_t character) noexcept {
  if (character > 0x7fU) {
    return true;
  }
  const auto byte = static_cast<unsigned char>(character);
  return std::isalnum(byte) != 0 || byte == '_' || byte == '-';
}

/** Append one Unicode scalar to a UTF-8 string. */
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

/** Return whether a byte is a UTF-8 continuation byte. */
constexpr bool is_continuation(unsigned char byte) noexcept {
  return (byte & 0xc0U) == 0x80U;
}

/**
 * Decode every complete scalar at the start of `bytes`.
 *
 * Invalid encodings become U+FFFD. When `finish` is false, a potentially valid
 * incomplete suffix remains in `bytes` for the next paste chunk.
 */
std::u32string consume_utf8(std::string& bytes, bool finish) {
  std::u32string output;
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto first        = static_cast<unsigned char>(bytes[offset]);
    std::size_t length      = 0U;
    std::uint32_t codepoint = 0U;
    std::uint32_t minimum   = 0U;
    if (first <= 0x7fU) {
      length    = 1U;
      codepoint = first;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      length    = 2U;
      codepoint = first & 0x1fU;
      minimum   = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length    = 3U;
      codepoint = first & 0x0fU;
      minimum   = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length    = 4U;
      codepoint = first & 0x07U;
      minimum   = 0x10000U;
    } else {
      output.push_back(U'\ufffd');
      ++offset;
      continue;
    }

    if (bytes.size() - offset < length) {
      if (!finish) {
        break;
      }
      output.push_back(U'\ufffd');
      ++offset;
      continue;
    }

    bool valid = true;
    for (std::size_t index = 1U; index < length; ++index) {
      const auto continuation =
          static_cast<unsigned char>(bytes[offset + index]);
      if (!is_continuation(continuation)) {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    if (!valid || codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      output.push_back(U'\ufffd');
      ++offset;
      continue;
    }
    output.push_back(static_cast<char32_t>(codepoint));
    offset += length;
  }
  bytes.erase(0U, offset);
  return output;
}

/** Decode a complete UTF-8 value, replacing malformed input. */
std::u32string decode_utf8(std::string_view text) {
  std::string bytes{text};
  return consume_utf8(bytes, true);
}

/** Construct one fully attributed Canvas cell. */
Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) noexcept {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Write a row-major cell grid through Canvas's span interface. */
Status write_grid(Canvas& canvas, const Canvas::Rect& rect,
                  std::vector<std::vector<Canvas::Cell>>& cells) {
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  const std::span<std::span<Canvas::Cell>> grid{rows};
  return canvas.write_cells(rect, grid);
}

}  // namespace

/** Hidden, synchronized state and editor operations. */
class InputFrame::Impl {
 public:
  /** Half-open logical selection range. */
  struct SelectionRange {
    InputCursor first; /**< First included caret position. */
    InputCursor last;  /**< First excluded caret position. */
  };

  /** All state preserved independently for one editor mode. */
  struct Buffer {
    std::vector<std::u32string> lines{1U};   /**< Newline-delimited contents. */
    InputCursor cursor;                      /**< Active insertion position. */
    std::optional<SelectionRange> selection; /**< Selected logical range. */
    std::optional<InputCursor>
        keyboard_anchor;             /**< Origin of a Shift-navigation range. */
    std::size_t scroll_row     = 0U; /**< First visible wrapped row. */
    std::size_t desired_column = 0U; /**< Sticky column for vertical motion. */
    bool has_desired_column = false; /**< Whether desired_column is active. */
  };

  /** One display row produced by wrapping one logical line. */
  struct VisualRow {
    std::size_t line           = 0U; /**< Logical source line. */
    std::size_t first          = 0U; /**< First source scalar on this row. */
    std::size_t length         = 0U; /**< Source scalars on this row. */
    std::size_t display_number = 0U; /**< Logical line number or zero. */
  };

  /** Geometry retained from the most recent successful draw. */
  struct Viewport {
    Canvas::Rect rect{};           /**< Absolute outer frame rectangle. */
    std::size_t gutter_width = 4U; /**< Border-inclusive left margin. */
    std::size_t width        = 0U; /**< Editable columns. */
    std::size_t height       = 0U; /**< Editable rows. */
    bool valid = false;            /**< Whether geometry can be hit-tested. */
  };

  /** Rollback state for one streaming bracketed paste. */
  struct PasteTransaction {
    InputMode mode = InputMode::NORMAL; /**< Buffer captured at BEGIN. */
    Buffer before;                      /**< Exact pre-paste buffer state. */
    std::string
        pending_utf8; /**< Incomplete UTF-8 suffix across DATA events. */
    bool preceding_cr = false; /**< Whether the preceding scalar was CR. */
  };

  /** Close the C virtual-terminal object after all synchronized users stop. */
  ~Impl() {
    if (terminal != nullptr) {
      tmt_close(terminal);
    }
  }

  /** Return the mutable buffer selected by mode. */
  Buffer& active_buffer() noexcept {
    return mode == InputMode::COMMAND ? command : normal;
  }

  /** Return the read-only buffer selected by mode. */
  const Buffer& active_buffer() const noexcept {
    return mode == InputMode::COMMAND ? command : normal;
  }

  /** Receive replies and cursor-visibility changes synchronously from libtmt.
   */
  static void terminal_callback(tmt_msg_t message, TMT*, const void* result,
                                void* context) {
    auto& impl = *static_cast<Impl*>(context);
    if (message == TMT_MSG_ANSWER && result != nullptr) {
      impl.terminal_responses.append(static_cast<const char*>(result));
    } else if (message == TMT_MSG_CURSOR && result != nullptr) {
      impl.terminal_cursor_visible = *static_cast<const char*>(result) == 't';
    }
  }

  /** Allocate or resize the persistent terminal to exact inner dimensions. */
  bool ensure_terminal(std::size_t rows, std::size_t columns) {
    if (rows < 2U || columns < 2U) {
      return false;
    }
    if (terminal == nullptr) {
      terminal =
          tmt_open(rows, columns, &Impl::terminal_callback, this, kTerminalAcs);
      if (terminal == nullptr) {
        return false;
      }
      terminal_rows    = rows;
      terminal_columns = columns;
      if (!pending_terminal_output.empty()) {
        tmt_write(terminal, pending_terminal_output.data(),
                  pending_terminal_output.size());
        pending_terminal_output.clear();
      }
      return true;
    }
    if (terminal_rows == rows && terminal_columns == columns) {
      return true;
    }
    if (!tmt_resize(terminal, rows, columns)) {
      return false;
    }
    terminal_rows    = rows;
    terminal_columns = columns;
    return true;
  }

  /** Feed terminal output now, or retain it until dimensions are available. */
  void write_terminal(std::string_view output) {
    if (terminal == nullptr) {
      pending_terminal_output.append(output);
      return;
    }
    tmt_write(terminal, output.data(), output.size());
  }

  /** Reset both allocated and not-yet-allocated terminal contents. */
  void clear_terminal() {
    pending_terminal_output.clear();
    if (terminal != nullptr) {
      tmt_reset(terminal);
    }
  }

  /** Release the emulated screen while leaving no stale session data. */
  void destroy_terminal() {
    if (terminal != nullptr) {
      tmt_close(terminal);
      terminal = nullptr;
    }
    terminal_rows           = 0U;
    terminal_columns        = 0U;
    terminal_cursor_visible = true;
    pending_terminal_output.clear();
    terminal_responses.clear();
  }

  /** Request a distinct PTY owner and optionally preserve first-use output. */
  void begin_new_terminal(bool preserve_pending_output) {
    std::string pending;
    if (preserve_pending_output) {
      pending = std::move(pending_terminal_output);
    }
    destroy_terminal();
    pending_terminal_output = std::move(pending);
    terminal_session_active = true;
    terminal_generation =
        terminal_generation == std::numeric_limits<std::size_t>::max()
            ? 1U
            : terminal_generation + 1U;
  }

  /** End the PTY lifecycle and leave a visible terminal for normal input. */
  void close_terminal() {
    destroy_terminal();
    terminal_session_active = false;
    if (mode == InputMode::TERMINAL) {
      mode = InputMode::NORMAL;
      terminal_visible.store(false, std::memory_order_relaxed);
      ensure_cursor_visible(normal);
    }
    escape_started.reset();
    paste.reset();
  }

  /** Serialize a buffer without exposing its UTF-32 representation. */
  static std::string text(const Buffer& buffer) {
    std::string output;
    for (std::size_t line = 0U; line < buffer.lines.size(); ++line) {
      for (const char32_t character : buffer.lines[line]) {
        append_utf8(character, output);
      }
      if (line + 1U < buffer.lines.size()) {
        output.push_back('\n');
      }
    }
    return output;
  }

  /** Return whether the buffer contains any user-created logical line. */
  static bool has_input(const Buffer& buffer) noexcept {
    return buffer.lines.size() > 1U || !buffer.lines.front().empty();
  }

  /** Return the line-number gutter required by the logical buffer. */
  static std::size_t gutter_width(const Buffer& buffer) noexcept {
    const std::size_t numbered = has_input(buffer) ? buffer.lines.size() : 0U;
    return numbered >= 100U ? 5U : 4U;
  }

  /** Return the editable width implied by current or minimum geometry. */
  std::size_t editable_width(const Buffer& buffer) const noexcept {
    const std::size_t gutter = gutter_width(buffer);
    if (viewport.valid && viewport.rect.width > gutter + 1U) {
      return viewport.rect.width - gutter - 1U;
    }
    return kMinimumWidth - gutter - 1U;
  }

  /** Find the separator before the first word that overflows a visual row. */
  static std::optional<std::size_t> materialized_wrap_break(
      const std::u32string& line, std::size_t width) {
    if (width == 0U) {
      return std::nullopt;
    }

    std::size_t row_first = 0U;
    while (line.size() - row_first > width) {
      const std::size_t overflow = row_first + width;
      std::size_t word_character = overflow;
      while (word_character < line.size() && line[word_character] == U' ') {
        ++word_character;
      }
      if (word_character == line.size()) {
        // A trailing separator alone does not prove that another word needs a
        // row. Wait until at least its first character has been inserted.
        return std::nullopt;
      }

      std::size_t word_first = word_character;
      while (word_first > row_first && line[word_first - 1U] != U' ') {
        --word_first;
      }
      if (word_first > row_first) {
        return word_first - 1U;
      }

      // This row begins inside one overlong token. Hard-wrap its segment and
      // keep looking without creating another logical line number.
      row_first += width;
    }
    return std::nullopt;
  }

  /** Replace word-boundary soft wraps with logical newline boundaries. */
  void materialize_word_wraps(Buffer& buffer, std::size_t first_line) const {
    std::size_t line_index = first_line;
    std::size_t final_line = first_line;
    while (line_index <= final_line) {
      std::u32string& line = buffer.lines[line_index];
      const std::optional<std::size_t> separator =
          materialized_wrap_break(line, editable_width(buffer));
      if (!separator.has_value()) {
        ++line_index;
        continue;
      }

      std::u32string suffix = line.substr(*separator + 1U);
      line.erase(*separator);
      buffer.lines.insert(
          buffer.lines.begin() + static_cast<std::ptrdiff_t>(line_index + 1U),
          std::move(suffix));
      if (buffer.cursor.line == line_index &&
          buffer.cursor.column > *separator) {
        ++buffer.cursor.line;
        buffer.cursor.column -= *separator + 1U;
      } else if (buffer.cursor.line > line_index) {
        ++buffer.cursor.line;
      }
      ++final_line;
      ++line_index;
    }
  }

  /** Wrap all logical lines for one positive inner width. */
  static std::vector<VisualRow> visual_rows(const Buffer& buffer,
                                            std::size_t width) {
    std::vector<VisualRow> rows;
    if (width == 0U) {
      return rows;
    }

    const bool number_lines = has_input(buffer);
    for (std::size_t line_index = 0U; line_index < buffer.lines.size();
         ++line_index) {
      const std::u32string& line       = buffer.lines[line_index];
      const std::size_t display_number = number_lines ? line_index + 1U : 0U;
      if (line.empty()) {
        rows.push_back(VisualRow{
            .line           = line_index,
            .display_number = display_number,
        });
        continue;
      }

      std::size_t first = 0U;
      while (first < line.size()) {
        const std::size_t remaining = line.size() - first;
        std::size_t length          = std::min(width, remaining);
        const std::size_t hard_end  = first + width;
        if (remaining > width && line[hard_end - 1U] != U' ' &&
            line[hard_end] != U' ') {
          // Keep the next word intact whenever whitespace exists in the
          // candidate row. The separating whitespace remains at the end of
          // this row, so wrapping never changes the logical buffer or copied
          // text. A word wider than the viewport is necessarily hard-wrapped.
          for (std::size_t candidate = hard_end; candidate > first;
               --candidate) {
            if (line[candidate - 1U] == U' ') {
              length = candidate - first;
              break;
            }
          }
        }
        rows.push_back(VisualRow{
            .line           = line_index,
            .first          = first,
            .length         = length,
            .display_number = first == 0U ? display_number : 0U,
        });
        first += length;
      }
      if (line_index == buffer.cursor.line &&
          buffer.cursor.column == line.size() && !rows.empty() &&
          rows.back().line == line_index && rows.back().length == width) {
        rows.push_back(VisualRow{
            .line  = line_index,
            .first = line.size(),
        });
      }
    }
    return rows;
  }

  /** Find the wrapped row and column containing the active caret. */
  static std::pair<std::size_t, std::size_t> visual_cursor(
      const Buffer& buffer, const std::vector<VisualRow>& rows,
      std::size_t width) noexcept {
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      const VisualRow& row = rows[index];
      if (row.line != buffer.cursor.line) {
        continue;
      }
      if (row.length == 0U && row.first == buffer.cursor.column) {
        return {index, 0U};
      }
      if (buffer.cursor.column >= row.first &&
          buffer.cursor.column < row.first + row.length) {
        return {index, buffer.cursor.column - row.first};
      }
      if (row.length < width &&
          buffer.cursor.column == row.first + row.length) {
        return {index, row.length};
      }
    }
    return {rows.empty() ? 0U : rows.size() - 1U, 0U};
  }

  /** Restrict one buffer's scroll origin to its current wrapped content. */
  void clamp_scroll(Buffer& buffer) const {
    if (!viewport.valid || viewport.width == 0U) {
      buffer.scroll_row = 0U;
      return;
    }
    const std::vector<VisualRow> rows = visual_rows(buffer, viewport.width);
    const std::size_t maximum =
        rows.size() > viewport.height ? rows.size() - viewport.height : 0U;
    buffer.scroll_row = std::min(buffer.scroll_row, maximum);
  }

  /** Scroll just enough to keep the active caret visible. */
  void ensure_cursor_visible(Buffer& buffer) const {
    if (!viewport.valid || viewport.width == 0U || viewport.height == 0U) {
      return;
    }
    const std::vector<VisualRow> rows = visual_rows(buffer, viewport.width);
    if (rows.empty()) {
      buffer.scroll_row = 0U;
      return;
    }
    const std::size_t row = visual_cursor(buffer, rows, viewport.width).first;
    if (row < buffer.scroll_row) {
      buffer.scroll_row = row;
    } else if (row >= buffer.scroll_row + viewport.height) {
      buffer.scroll_row = row - viewport.height + 1U;
    }
    const std::size_t maximum =
        rows.size() > viewport.height ? rows.size() - viewport.height : 0U;
    buffer.scroll_row = std::min(buffer.scroll_row, maximum);
  }

  /** Clear editable contents and derived interaction state. */
  static void clear_buffer(Buffer& buffer) { buffer = Buffer{}; }

  /** Return a caret one scalar after a mapped character. */
  static InputCursor after_character(const Buffer& buffer,
                                     InputCursor position) noexcept {
    if (position.line < buffer.lines.size() &&
        position.column < buffer.lines[position.line].size()) {
      ++position.column;
    }
    return position;
  }

  /** Delete the active half-open range and put the caret at its start. */
  static bool delete_selection(Buffer& buffer) {
    if (!buffer.selection.has_value()) {
      return false;
    }
    const SelectionRange range = *buffer.selection;
    if (range.first.line == range.last.line) {
      std::u32string& line = buffer.lines[range.first.line];
      line.erase(range.first.column, range.last.column - range.first.column);
    } else {
      std::u32string merged =
          buffer.lines[range.first.line].substr(0U, range.first.column);
      merged.append(buffer.lines[range.last.line].substr(range.last.column));
      buffer.lines[range.first.line] = std::move(merged);
      buffer.lines.erase(buffer.lines.begin() +
                             static_cast<std::ptrdiff_t>(range.first.line + 1U),
                         buffer.lines.begin() +
                             static_cast<std::ptrdiff_t>(range.last.line + 1U));
    }
    buffer.cursor = range.first;
    buffer.selection.reset();
    buffer.keyboard_anchor.reset();
    buffer.has_desired_column = false;
    return true;
  }

  /** Insert normalized Unicode scalars at the active caret. */
  void insert(Buffer& buffer, std::u32string_view characters,
              bool& preceding_cr) const {
    static_cast<void>(delete_selection(buffer));
    for (const char32_t character : characters) {
      if (character == U'\n' && preceding_cr) {
        preceding_cr = false;
        continue;
      }
      if (character == U'\r' || character == U'\n') {
        std::u32string suffix =
            buffer.lines[buffer.cursor.line].substr(buffer.cursor.column);
        buffer.lines[buffer.cursor.line].erase(buffer.cursor.column);
        buffer.lines.insert(buffer.lines.begin() + static_cast<std::ptrdiff_t>(
                                                       buffer.cursor.line + 1U),
                            std::move(suffix));
        ++buffer.cursor.line;
        buffer.cursor.column = 0U;
        preceding_cr         = character == U'\r';
        continue;
      }
      preceding_cr = false;
      if (character == U'\t') {
        const std::size_t insertion_line = buffer.cursor.line;
        const std::size_t count =
            kTabWidth - (buffer.cursor.column % kTabWidth);
        buffer.lines[buffer.cursor.line].insert(buffer.cursor.column, count,
                                                U' ');
        buffer.cursor.column += count;
        materialize_word_wraps(buffer, insertion_line);
        continue;
      }
      if (character < 0x20U || character == 0x7fU) {
        continue;
      }
      buffer.lines[buffer.cursor.line].insert(
          buffer.lines[buffer.cursor.line].begin() +
              static_cast<std::ptrdiff_t>(buffer.cursor.column),
          character);
      ++buffer.cursor.column;
      materialize_word_wraps(buffer, buffer.cursor.line);
    }
    buffer.selection.reset();
    buffer.keyboard_anchor.reset();
    buffer.has_desired_column = false;
    ensure_cursor_visible(buffer);
  }

  /** Remove the scalar before the caret or join with the preceding line. */
  void backspace(Buffer& buffer) const {
    if (delete_selection(buffer)) {
      ensure_cursor_visible(buffer);
      return;
    }
    if (buffer.cursor.column > 0U) {
      std::u32string& line = buffer.lines[buffer.cursor.line];
      line.erase(buffer.cursor.column - 1U, 1U);
      --buffer.cursor.column;
    } else if (buffer.cursor.line > 0U) {
      const std::size_t preceding_line = buffer.cursor.line - 1U;
      const std::size_t preceding_size = buffer.lines[preceding_line].size();
      buffer.lines[preceding_line].append(buffer.lines[buffer.cursor.line]);
      buffer.lines.erase(buffer.lines.begin() +
                         static_cast<std::ptrdiff_t>(buffer.cursor.line));
      buffer.cursor = {.line = preceding_line, .column = preceding_size};
    }
    buffer.keyboard_anchor.reset();
    buffer.has_desired_column = false;
    ensure_cursor_visible(buffer);
  }

  /** Remove the scalar under the caret or join with the following line. */
  void delete_forward(Buffer& buffer) const {
    if (delete_selection(buffer)) {
      ensure_cursor_visible(buffer);
      return;
    }
    std::u32string& line = buffer.lines[buffer.cursor.line];
    if (buffer.cursor.column < line.size()) {
      line.erase(buffer.cursor.column, 1U);
    } else if (buffer.cursor.line + 1U < buffer.lines.size()) {
      line.append(buffer.lines[buffer.cursor.line + 1U]);
      buffer.lines.erase(buffer.lines.begin() +
                         static_cast<std::ptrdiff_t>(buffer.cursor.line + 1U));
    }
    buffer.keyboard_anchor.reset();
    buffer.has_desired_column = false;
    ensure_cursor_visible(buffer);
  }

  /** Update or clear a Shift-navigation selection after moving the caret. */
  static void finish_navigation(Buffer& buffer, InputCursor old_cursor,
                                bool extend) {
    if (!extend) {
      buffer.selection.reset();
      buffer.keyboard_anchor.reset();
      return;
    }
    if (!buffer.keyboard_anchor.has_value()) {
      buffer.keyboard_anchor = old_cursor;
    }
    const InputCursor anchor = *buffer.keyboard_anchor;
    if (anchor == buffer.cursor) {
      buffer.selection.reset();
    } else if (position_less(buffer.cursor, anchor)) {
      buffer.selection = SelectionRange{.first = buffer.cursor, .last = anchor};
    } else {
      buffer.selection = SelectionRange{.first = anchor, .last = buffer.cursor};
    }
  }

  /** Move one scalar horizontally, crossing logical newline boundaries. */
  void move_horizontal(Buffer& buffer, bool right, bool extend) const {
    const InputCursor old_cursor = buffer.cursor;
    if (right) {
      if (buffer.cursor.column < buffer.lines[buffer.cursor.line].size()) {
        ++buffer.cursor.column;
      } else if (buffer.cursor.line + 1U < buffer.lines.size()) {
        ++buffer.cursor.line;
        buffer.cursor.column = 0U;
      }
    } else if (buffer.cursor.column > 0U) {
      --buffer.cursor.column;
    } else if (buffer.cursor.line > 0U) {
      --buffer.cursor.line;
      buffer.cursor.column = buffer.lines[buffer.cursor.line].size();
    }
    buffer.has_desired_column = false;
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move to the start of the preceding word. */
  void move_word_left(Buffer& buffer, bool extend = false) const {
    const InputCursor old_cursor = buffer.cursor;
    if (buffer.cursor.column == 0U) {
      if (buffer.cursor.line == 0U) {
        finish_navigation(buffer, old_cursor, extend);
        return;
      }
      --buffer.cursor.line;
      buffer.cursor.column = buffer.lines[buffer.cursor.line].size();
    }
    std::u32string& line = buffer.lines[buffer.cursor.line];
    while (buffer.cursor.column > 0U &&
           !is_word_character(line[buffer.cursor.column - 1U])) {
      --buffer.cursor.column;
    }
    while (buffer.cursor.column > 0U &&
           is_word_character(line[buffer.cursor.column - 1U])) {
      --buffer.cursor.column;
    }
    buffer.has_desired_column = false;
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move to the start of the following word. */
  void move_word_right(Buffer& buffer, bool extend = false) const {
    const InputCursor old_cursor = buffer.cursor;
    std::u32string* line         = &buffer.lines[buffer.cursor.line];
    if (buffer.cursor.column == line->size() &&
        buffer.cursor.line + 1U < buffer.lines.size()) {
      ++buffer.cursor.line;
      buffer.cursor.column = 0U;
      line                 = &buffer.lines[buffer.cursor.line];
    }
    while (buffer.cursor.column < line->size() &&
           is_word_character((*line)[buffer.cursor.column])) {
      ++buffer.cursor.column;
    }
    while (buffer.cursor.column < line->size() &&
           !is_word_character((*line)[buffer.cursor.column])) {
      ++buffer.cursor.column;
    }
    buffer.has_desired_column = false;
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move vertically through wrapped rows while retaining the desired column.
   */
  void move_vertical(Buffer& buffer, std::int64_t row_delta,
                     bool extend) const {
    const std::size_t width           = editable_width(buffer);
    const std::vector<VisualRow> rows = visual_rows(buffer, width);
    if (rows.empty()) {
      return;
    }
    const InputCursor old_cursor = buffer.cursor;
    const auto [current_row, current_column] =
        visual_cursor(buffer, rows, width);
    if (!buffer.has_desired_column) {
      buffer.desired_column     = current_column;
      buffer.has_desired_column = true;
    }
    std::size_t target = current_row;
    if (row_delta < 0) {
      const std::uint64_t distance = static_cast<std::uint64_t>(-row_delta);
      target =
          distance > target ? 0U : target - static_cast<std::size_t>(distance);
    } else {
      const std::uint64_t distance = static_cast<std::uint64_t>(row_delta);
      const std::size_t remaining  = rows.size() - 1U - target;
      target += static_cast<std::size_t>(
          std::min<std::uint64_t>(distance, remaining));
    }
    const VisualRow& row = rows[target];
    buffer.cursor        = {
               .line   = row.line,
               .column = row.first + std::min(buffer.desired_column, row.length),
    };
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move to one logical row boundary. */
  void move_row_boundary(Buffer& buffer, bool end, bool extend) const {
    const InputCursor old_cursor      = buffer.cursor;
    const std::size_t width           = editable_width(buffer);
    const std::vector<VisualRow> rows = visual_rows(buffer, width);
    if (!rows.empty()) {
      const std::size_t row_index = visual_cursor(buffer, rows, width).first;
      const VisualRow& row        = rows[row_index];
      buffer.cursor               = {
                        .line   = row.line,
                        .column = end ? row.first + row.length : row.first,
      };
    }
    buffer.has_desired_column = false;
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move to the first or final caret in the active buffer. */
  void move_buffer_boundary(Buffer& buffer, bool end, bool extend) const {
    const InputCursor old_cursor = buffer.cursor;
    if (end) {
      buffer.cursor.line   = buffer.lines.size() - 1U;
      buffer.cursor.column = buffer.lines.back().size();
    } else {
      buffer.cursor = {};
    }
    buffer.has_desired_column = false;
    finish_navigation(buffer, old_cursor, extend);
    ensure_cursor_visible(buffer);
  }

  /** Move the caret by one visible viewport. */
  void move_page(Buffer& buffer, bool down, bool extend = false) const {
    const std::size_t page =
        viewport.valid ? std::max<std::size_t>(viewport.height, 1U) : 1U;
    const std::int64_t delta = static_cast<std::int64_t>(std::min<std::size_t>(
        page,
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
    move_vertical(buffer, down ? delta : -delta, extend);
  }

  /** Apply one configured semantic input command. */
  void apply_command(terminal::Command input_command) {
    if (input_command == terminal::Command::ENTER_COMMAND_MODE) {
      command = Buffer{};
      mode    = InputMode::COMMAND;
      terminal_visible.store(false, std::memory_order_relaxed);
      escape_started.reset();
      paste.reset();
      return;
    }
    if (input_command == terminal::Command::ENTER_TERMINAL_MODE) {
      if (!terminal_session_active) {
        begin_new_terminal(terminal_generation == 0U);
      }
      mode = InputMode::TERMINAL;
      terminal_visible.store(true, std::memory_order_relaxed);
      escape_started.reset();
      paste.reset();
      return;
    }
    if (mode == InputMode::TERMINAL) {
      return;
    }

    Buffer& buffer = active_buffer();
    switch (input_command) {
      case terminal::Command::COPY:
      case terminal::Command::SELECT_ALL:
        return;
      case terminal::Command::ENTER_COMMAND_MODE:
      case terminal::Command::ENTER_TERMINAL_MODE:
        return;
      case terminal::Command::MOVE_WORD_LEFT:
        move_word_left(buffer);
        return;
      case terminal::Command::MOVE_WORD_RIGHT:
        move_word_right(buffer);
        return;
      case terminal::Command::MOVE_ROW_START:
        move_row_boundary(buffer, false, false);
        return;
      case terminal::Command::MOVE_ROW_END:
        move_row_boundary(buffer, true, false);
        return;
      case terminal::Command::MOVE_BUFFER_START:
        move_buffer_boundary(buffer, false, false);
        return;
      case terminal::Command::MOVE_BUFFER_END:
        move_buffer_boundary(buffer, true, false);
        return;
      case terminal::Command::MOVE_PAGE_UP:
        move_page(buffer, false);
        return;
      case terminal::Command::MOVE_PAGE_DOWN:
        move_page(buffer, true);
        return;
    }
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
        ensure_cursor_visible(normal);
      } else {
        clear_buffer(normal);
      }
      escape_started.reset();
      paste.reset();
      return;
    }
    escape_started = now;
  }

  /** Insert committed text, recognizing armed Escape mode chords. */
  void handle_committed_text(std::string_view text_view,
                             Clock::time_point now) {
    const bool escape_chord =
        mode == InputMode::NORMAL && escape_started.has_value() &&
        now >= *escape_started &&
        now - *escape_started < kDoubleEscapeInterval && !text_view.empty() &&
        (text_view.front() == ':' || text_view.front() == '>');
    if (escape_chord) {
      const terminal::Command command =
          text_view.front() == ':' ? terminal::Command::ENTER_COMMAND_MODE
                                   : terminal::Command::ENTER_TERMINAL_MODE;
      apply_command(command);
      text_view.remove_prefix(1U);
    } else {
      escape_started.reset();
    }
    if (mode != InputMode::TERMINAL && !text_view.empty()) {
      bool preceding_cr = false;
      insert(active_buffer(), decode_utf8(text_view), preceding_cr);
    }
  }

  /** Apply a decoded key event. */
  void handle_key(const terminal::KeyEvent& event, Clock::time_point now) {
    if (event.action == terminal::KeyAction::RELEASE || paste.has_value()) {
      return;
    }

    const auto* named = std::get_if<terminal::NamedKey>(&event.key.value);
    if (named != nullptr && *named == terminal::NamedKey::ESCAPE) {
      handle_escape(now, event.modifiers.contains(terminal::Modifier::ALT));
      return;
    }
    if (named != nullptr && is_modifier_key(*named)) {
      // REPORT_ALL_KEYS emits modifier presses separately. They are context
      // for the following key, not an intervening editor action, so an armed
      // Escape chord must survive them.
      return;
    }

    if (mode == InputMode::TERMINAL) {
      escape_started.reset();
      return;
    }

    Buffer& buffer     = active_buffer();
    const bool extend  = event.modifiers.contains(terminal::Modifier::SHIFT);
    const bool option  = event.modifiers.contains(terminal::Modifier::ALT);
    const bool command = event.modifiers.contains(terminal::Modifier::SUPER) ||
                         event.modifiers.contains(terminal::Modifier::META);
    if (named != nullptr) {
      escape_started.reset();
      switch (*named) {
        case terminal::NamedKey::ENTER:
        case terminal::NamedKey::KEYPAD_ENTER: {
          if (!extend) {
            // Plain Enter is reserved for the future Send action. It must not
            // mutate the draft in the meantime.
            return;
          }
          bool preceding_cr = false;
          insert(buffer, U"\n", preceding_cr);
          return;
        }
        case terminal::NamedKey::TAB: {
          bool preceding_cr = false;
          insert(buffer, U"\t", preceding_cr);
          return;
        }
        case terminal::NamedKey::BACKSPACE:
          backspace(buffer);
          return;
        case terminal::NamedKey::DELETE_KEY:
        case terminal::NamedKey::KEYPAD_DELETE:
          delete_forward(buffer);
          return;
        case terminal::NamedKey::LEFT:
        case terminal::NamedKey::KEYPAD_LEFT:
          if (command) {
            move_row_boundary(buffer, false, extend);
          } else if (option) {
            move_word_left(buffer, extend);
          } else {
            move_horizontal(buffer, false, extend);
          }
          return;
        case terminal::NamedKey::RIGHT:
        case terminal::NamedKey::KEYPAD_RIGHT:
          if (command) {
            move_row_boundary(buffer, true, extend);
          } else if (option) {
            move_word_right(buffer, extend);
          } else {
            move_horizontal(buffer, true, extend);
          }
          return;
        case terminal::NamedKey::UP:
        case terminal::NamedKey::KEYPAD_UP:
          if (command) {
            move_buffer_boundary(buffer, false, extend);
          } else if (option) {
            move_page(buffer, false, extend);
          } else {
            move_vertical(buffer, -1, extend);
          }
          return;
        case terminal::NamedKey::DOWN:
        case terminal::NamedKey::KEYPAD_DOWN:
          if (command) {
            move_buffer_boundary(buffer, true, extend);
          } else if (option) {
            move_page(buffer, true, extend);
          } else {
            move_vertical(buffer, 1, extend);
          }
          return;
        case terminal::NamedKey::HOME:
        case terminal::NamedKey::KEYPAD_HOME:
          move_row_boundary(buffer, false, extend);
          return;
        case terminal::NamedKey::END:
        case terminal::NamedKey::KEYPAD_END:
          move_row_boundary(buffer, true, extend);
          return;
        case terminal::NamedKey::PAGE_UP:
        case terminal::NamedKey::KEYPAD_PAGE_UP:
          move_page(buffer, false);
          return;
        case terminal::NamedKey::PAGE_DOWN:
        case terminal::NamedKey::KEYPAD_PAGE_DOWN:
          move_page(buffer, true);
          return;
        default:
          return;
      }
    }

    if (event.modifiers.contains(terminal::Modifier::CONTROL) ||
        event.modifiers.contains(terminal::Modifier::SUPER) ||
        event.modifiers.contains(terminal::Modifier::HYPER) ||
        event.modifiers.contains(terminal::Modifier::META)) {
      escape_started.reset();
      return;
    }
    if (!event.text.empty()) {
      handle_committed_text(event.text, now);
      return;
    }
    const auto* character = std::get_if<char32_t>(&event.key.value);
    if (character == nullptr) {
      escape_started.reset();
      return;
    }
    const char32_t inserted = event.shifted_key.value_or(*character);
    std::string utf8;
    append_utf8(inserted, utf8);
    handle_committed_text(utf8, now);
  }

  /** Insert one committed text event, including Escape-colon fallback. */
  void handle_text(const terminal::TextEvent& event, Clock::time_point now) {
    if (paste.has_value()) {
      return;
    }
    handle_committed_text(event.utf8, now);
  }

  /** Apply one transactional bracketed-paste stage. */
  Status handle_paste(const terminal::PasteEvent& event) {
    if (mode == InputMode::TERMINAL) {
      escape_started.reset();
      return Status::OK;
    }
    switch (event.phase) {
      case terminal::PastePhase::BEGIN:
        if (paste.has_value()) {
          return Status::INVALID_ARGUMENT;
        }
        escape_started.reset();
        paste = PasteTransaction{.mode = mode, .before = active_buffer()};
        return Status::OK;

      case terminal::PastePhase::DATA:
        if (!paste.has_value() || paste->mode != mode) {
          return Status::INVALID_ARGUMENT;
        }
        paste->pending_utf8.append(event.data);
        insert(active_buffer(), consume_utf8(paste->pending_utf8, false),
               paste->preceding_cr);
        return Status::OK;

      case terminal::PastePhase::END:
        if (!paste.has_value() || paste->mode != mode) {
          return Status::INVALID_ARGUMENT;
        }
        insert(active_buffer(), consume_utf8(paste->pending_utf8, true),
               paste->preceding_cr);
        paste.reset();
        return Status::OK;

      case terminal::PastePhase::CANCEL:
        if (!paste.has_value()) {
          return Status::OK;
        }
        if (paste->mode == InputMode::NORMAL) {
          normal = std::move(paste->before);
        } else {
          command = std::move(paste->before);
        }
        paste.reset();
        return Status::OK;
    }
    return Status::INVALID_ARGUMENT;
  }

  /** Apply vertical wheel motion while deliberately ignoring horizontal input.
   */
  void handle_scroll(const terminal::ScrollEvent& event) {
    if (mode == InputMode::TERMINAL) {
      return;
    }
    if (!viewport.valid || event.position.x < viewport.rect.x ||
        event.position.y < viewport.rect.y ||
        event.position.x >= viewport.rect.x + viewport.rect.width ||
        event.position.y >= viewport.rect.y + viewport.rect.height ||
        event.delta_y == 0) {
      return;
    }
    Buffer& buffer                    = active_buffer();
    const std::vector<VisualRow> rows = visual_rows(buffer, viewport.width);
    const std::size_t maximum =
        rows.size() > viewport.height ? rows.size() - viewport.height : 0U;
    const std::int64_t delta = event.delta_y;
    if (delta > 0) {
      const std::uint64_t amount = static_cast<std::uint64_t>(delta);
      buffer.scroll_row =
          amount > buffer.scroll_row
              ? 0U
              : buffer.scroll_row - static_cast<std::size_t>(amount);
    } else {
      const std::uint64_t amount =
          static_cast<std::uint64_t>(-(delta + 1)) + 1U;
      buffer.scroll_row += static_cast<std::size_t>(
          std::min<std::uint64_t>(amount, maximum - buffer.scroll_row));
    }
  }

  /** Map a frame-local cell to a visible source character. */
  std::optional<InputCursor> map_character(SelectionPosition position) const {
    const Buffer& buffer = active_buffer();
    if (!viewport.valid || viewport.width == 0U || viewport.height == 0U) {
      return std::nullopt;
    }
    const std::vector<VisualRow> rows = visual_rows(buffer, viewport.width);
    if (rows.empty()) {
      return std::nullopt;
    }

    std::int64_t relative_y = position.y - static_cast<std::int64_t>(kInnerTop);
    std::size_t visual_index = buffer.scroll_row;
    if (relative_y < 0) {
      const std::uint64_t distance =
          static_cast<std::uint64_t>(-(relative_y + 1)) + 1U;
      visual_index = distance > visual_index
                         ? 0U
                         : visual_index - static_cast<std::size_t>(distance);
    } else {
      const std::uint64_t distance = static_cast<std::uint64_t>(relative_y);
      const std::size_t remaining  = rows.size() - 1U - visual_index;
      visual_index += static_cast<std::size_t>(
          std::min<std::uint64_t>(distance, remaining));
    }

    const VisualRow& row = rows[visual_index];
    if (row.length == 0U) {
      return std::nullopt;
    }
    const std::int64_t relative_x =
        position.x - static_cast<std::int64_t>(viewport.gutter_width);
    std::size_t column = 0U;
    if (relative_x > 0) {
      column = static_cast<std::size_t>(relative_x);
    }
    column = std::min(column, row.length - 1U);
    return InputCursor{.line = row.line, .column = row.first + column};
  }

  /** Map an in-viewport single click to a caret boundary. */
  std::optional<InputCursor> map_caret(SelectionPosition position) const {
    const Buffer& buffer = active_buffer();
    if (!viewport.valid || viewport.width == 0U || viewport.height == 0U ||
        position.x < static_cast<std::int64_t>(viewport.gutter_width) ||
        position.y < static_cast<std::int64_t>(kInnerTop) ||
        position.x >=
            static_cast<std::int64_t>(viewport.gutter_width + viewport.width) ||
        position.y >= static_cast<std::int64_t>(kInnerTop + viewport.height)) {
      return std::nullopt;
    }
    const std::vector<VisualRow> rows = visual_rows(buffer, viewport.width);
    const std::size_t visual_index =
        buffer.scroll_row +
        static_cast<std::size_t>(position.y -
                                 static_cast<std::int64_t>(kInnerTop));
    if (visual_index >= rows.size()) {
      return std::nullopt;
    }
    const VisualRow& row     = rows[visual_index];
    const std::size_t offset = static_cast<std::size_t>(
        position.x - static_cast<std::int64_t>(viewport.gutter_width));
    return InputCursor{
        .line   = row.line,
        .column = row.first + std::min(offset, row.length),
    };
  }

  /** Return whether one source scalar lies inside the active selection. */
  static bool is_selected(const Buffer& buffer, InputCursor position) noexcept {
    return buffer.selection.has_value() &&
           !position_less(position, buffer.selection->first) &&
           position_less(position, buffer.selection->last);
  }

  /** Copy the active range into a UTF-8 value. */
  static Status selected_text(const Buffer& buffer, std::string& output) {
    output.clear();
    if (!buffer.selection.has_value()) {
      return Status::NO_SELECTION;
    }
    const SelectionRange range = *buffer.selection;
    for (std::size_t line = range.first.line; line <= range.last.line; ++line) {
      const std::size_t first =
          line == range.first.line ? range.first.column : 0U;
      const std::size_t last = line == range.last.line
                                   ? range.last.column
                                   : buffer.lines[line].size();
      for (std::size_t column = first; column < last; ++column) {
        append_utf8(buffer.lines[line][column], output);
      }
      if (line < range.last.line) {
        output.push_back('\n');
      }
    }
    return Status::OK;
  }

  mutable std::shared_mutex mutex;    /**< Synchronizes input and rendering. */
  Buffer normal;                      /**< Preserved normal input buffer. */
  Buffer command;                     /**< Temporary command-mode buffer. */
  InputMode mode = InputMode::NORMAL; /**< Active buffer selector. */
  std::atomic_bool terminal_visible = false; /**< Lock-free Frame capability. */
  std::optional<Clock::time_point>
      escape_started; /**< Time at which the first Escape arrived. */
  std::optional<PasteTransaction> paste; /**< Active transactional paste. */
  std::string notification; /**< UTF-8 notification-bar contents. */
  Viewport viewport;        /**< Geometry from the most recent draw. */
  TMT* terminal = nullptr;  /**< Persistent libtmt virtual-terminal object. */
  std::size_t terminal_rows    = 0U;   /**< Current libtmt row count. */
  std::size_t terminal_columns = 0U;   /**< Current libtmt column count. */
  bool terminal_cursor_visible = true; /**< Last DECTCEM state from libtmt. */
  std::string pending_terminal_output; /**< Output queued before first draw. */
  std::string terminal_responses; /**< Replies waiting for the PTY owner. */
  bool terminal_session_active    = false; /**< Whether an owner should run. */
  std::size_t terminal_generation = 0U;    /**< Requested session identity. */
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
  const Impl::Buffer& buffer = impl_->active_buffer();
  const std::size_t gutter   = Impl::gutter_width(buffer);
  const std::size_t inner_width =
      screen_width > gutter + 1U ? screen_width - gutter - 1U : 1U;
  const std::size_t content_height =
      Impl::visual_rows(buffer, inner_width).size();
  const std::size_t desired =
      content_height > std::numeric_limits<std::size_t>::max() - 4U
          ? std::numeric_limits<std::size_t>::max()
          : content_height + 4U;
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
    impl_->handle_key(*key, now);
    return Status::OK;
  }
  if (const auto* text_event = std::get_if<terminal::TextEvent>(&event)) {
    impl_->handle_text(*text_event, now);
    return Status::OK;
  }
  if (const auto* paste_event = std::get_if<terminal::PasteEvent>(&event)) {
    return impl_->handle_paste(*paste_event);
  }
  if (const auto* scroll_event = std::get_if<terminal::ScrollEvent>(&event)) {
    impl_->escape_started.reset();
    impl_->handle_scroll(*scroll_event);
    return Status::OK;
  }
  if (const auto* command_event = std::get_if<terminal::CommandEvent>(&event)) {
    if (!impl_->paste.has_value()) {
      impl_->escape_started.reset();
      impl_->apply_command(command_event->command);
    }
    return Status::OK;
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
  const std::unique_lock lock(impl_->mutex);
  impl_->write_terminal(output);
  return Status::OK;
}

std::string InputFrame::take_terminal_responses() {
  const std::unique_lock lock(impl_->mutex);
  std::string responses = std::move(impl_->terminal_responses);
  impl_->terminal_responses.clear();
  return responses;
}

void InputFrame::reset_terminal() {
  const std::unique_lock lock(impl_->mutex);
  impl_->clear_terminal();
}

void InputFrame::start_new_terminal() {
  const std::unique_lock lock(impl_->mutex);
  impl_->begin_new_terminal(false);
  impl_->mode = InputMode::TERMINAL;
  impl_->terminal_visible.store(true, std::memory_order_relaxed);
  impl_->escape_started.reset();
  impl_->paste.reset();
}

void InputFrame::close_terminal() {
  const std::unique_lock lock(impl_->mutex);
  impl_->close_terminal();
}

void InputFrame::clear() {
  const std::unique_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    impl_->clear_terminal();
  } else {
    Impl::clear_buffer(impl_->active_buffer());
  }
  impl_->escape_started.reset();
  impl_->paste.reset();
}

InputFrameSnapshot InputFrame::snapshot() const {
  const std::shared_lock lock(impl_->mutex);
  const Impl::Buffer& active = impl_->active_buffer();
  return InputFrameSnapshot{
      .mode                    = impl_->mode,
      .input_text              = Impl::text(impl_->normal),
      .command_text            = Impl::text(impl_->command),
      .cursor                  = active.cursor,
      .scroll_row              = active.scroll_row,
      .escape_armed            = impl_->escape_started.has_value(),
      .paste_in_progress       = impl_->paste.has_value(),
      .terminal_rows           = impl_->terminal_rows,
      .terminal_columns        = impl_->terminal_columns,
      .terminal_cursor_visible = impl_->terminal_cursor_visible,
      .terminal_session_active = impl_->terminal_session_active,
      .terminal_generation     = impl_->terminal_generation,
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
  Impl::Buffer& buffer = impl_->active_buffer();
  const std::size_t gutter =
      terminal_mode ? kTerminalGutterWidth : Impl::gutter_width(buffer);
  const std::size_t inner_width  = rect.width - gutter - 1U;
  const std::size_t inner_height = rect.height - 4U;
  if (terminal_mode && !impl_->ensure_terminal(inner_height, inner_width)) {
    return Status::TERMINAL_CONFIG_FAILED;
  }
  const bool viewport_changed = !impl_->viewport.valid ||
                                impl_->viewport.gutter_width != gutter ||
                                impl_->viewport.width != inner_width ||
                                impl_->viewport.height != inner_height;
  impl_->viewport = Impl::Viewport{
      .rect         = rect,
      .gutter_width = gutter,
      .width        = inner_width,
      .height       = inner_height,
      .valid        = true,
  };
  if (!terminal_mode) {
    impl_->clamp_scroll(buffer);
    if (viewport_changed) {
      impl_->ensure_cursor_visible(buffer);
    }
  }

  const Theme::Colors colors     = theme.get_colors();
  const std::uint32_t border     = terminal_mode ? colors.tertiary
                                   : impl_->mode == InputMode::COMMAND
                                       ? colors.text_success
                                       : colors.text;
  const std::uint32_t input_text = impl_->mode == InputMode::COMMAND
                                       ? colors.text_success
                                       : colors.text_secondary;
  const std::uint32_t interior   = colors.secondary;
  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height, std::vector<Canvas::Cell>(
                       rect.width, cell(U' ', colors.text, colors.background)));

  const std::size_t box_top    = 1U;
  const std::size_t box_bottom = rect.height - 2U;
  const std::size_t box_right  = rect.width - 1U;
  for (std::size_t y = box_top + 1U; y < box_bottom; ++y) {
    for (std::size_t x = 1U; x < box_right; ++x) {
      cells[y][x] = cell(U' ', input_text, interior);
    }
  }
  for (std::size_t x = 1U; x < box_right; ++x) {
    cells[box_top][x]    = cell(U'─', border, colors.background);
    cells[box_bottom][x] = cell(U'─', border, colors.background);
  }
  for (std::size_t y = box_top + 1U; y < box_bottom; ++y) {
    cells[y][0U]        = cell(U'│', border, colors.background);
    cells[y][box_right] = cell(U'│', border, colors.background);
  }
  cells[box_top][0U]           = cell(U'┌', border, colors.background);
  cells[box_top][box_right]    = cell(U'┐', border, colors.background);
  cells[box_bottom][0U]        = cell(U'└', border, colors.background);
  cells[box_bottom][box_right] = cell(U'┘', border, colors.background);

  if (terminal_mode) {
    const TMTSCREEN* screen = tmt_screen(impl_->terminal);
    for (std::size_t row = 0U; row < screen->nline; ++row) {
      for (std::size_t column = 0U; column < screen->ncol; ++column) {
        const TMTCHAR& source = screen->lines[row]->chars[column];
        std::uint32_t foreground =
            terminal_color(source.a.fg, colors.text_secondary, colors);
        std::uint32_t background =
            terminal_color(source.a.bg, interior, colors);
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
        cells[kInnerTop + row][gutter + column] =
            cell(terminal_character(source.c), foreground, background);
      }
    }

    const TMTPOINT* cursor = tmt_cursor(impl_->terminal);
    if (impl_->terminal_cursor_visible && cursor->r < inner_height &&
        cursor->c < inner_width) {
      Canvas::Cell& cursor_cell =
          cells[kInnerTop + cursor->r][gutter + cursor->c];
      cursor_cell.foreground_color = cursor_cell.background_color;
      cursor_cell.background_color = colors.tertiary;
    }
    tmt_clean(impl_->terminal);
  } else {
    const std::vector<Impl::VisualRow> rows =
        Impl::visual_rows(buffer, inner_width);
    const auto [cursor_row, cursor_column] =
        Impl::visual_cursor(buffer, rows, inner_width);
    for (std::size_t visible = 0U; visible < inner_height; ++visible) {
      const std::size_t row_index = buffer.scroll_row + visible;
      if (row_index >= rows.size()) {
        break;
      }
      const Impl::VisualRow& row = rows[row_index];
      const std::size_t y        = kInnerTop + visible;
      if (row.display_number != 0U) {
        const std::string number = std::to_string(row.display_number);
        const std::size_t count  = std::min(number.size(), gutter - 2U);
        const std::size_t first  = gutter - count - 1U;
        for (std::size_t index = 0U; index < count; ++index) {
          cells[y][first + index] = cell(
              static_cast<unsigned char>(number[number.size() - count + index]),
              colors.text_muted, interior);
        }
      }

      for (std::size_t column = 0U; column < row.length; ++column) {
        const InputCursor logical{
            .line   = row.line,
            .column = row.first + column,
        };
        const bool selected = Impl::is_selected(buffer, logical);
        cells[y][gutter + column] =
            cell(buffer.lines[row.line][logical.column],
                 selected ? colors.highlight_text : input_text,
                 selected ? colors.highlight_background : interior);
      }
      if (row_index == cursor_row && cursor_column < inner_width) {
        Canvas::Cell& cursor_cell    = cells[y][gutter + cursor_column];
        cursor_cell.foreground_color = interior;
        cursor_cell.background_color = border;
      }
    }
  }

  if (impl_->escape_started.has_value()) {
    const std::string_view prompt =
        impl_->mode == InputMode::COMMAND ? kExitCommandPrompt : kClearPrompt;
    const std::size_t available = rect.width > 4U ? rect.width - 4U : 0U;
    const std::size_t count     = std::min(available, prompt.size());
    for (std::size_t index = 0U; index < count; ++index) {
      cells[box_bottom][2U + index] =
          cell(static_cast<unsigned char>(prompt[index]), colors.text_muted,
               colors.background);
    }
  }

  const std::u32string notification = decode_utf8(impl_->notification);
  const std::size_t notification_count =
      std::min(rect.width, notification.size());
  for (std::size_t index = 0U; index < notification_count; ++index) {
    cells[rect.height - 1U][index] =
        cell(notification[index], colors.text_muted, colors.background);
  }

  return write_grid(canvas, rect, cells);
}

bool InputFrame::is_selectable() const noexcept {
  return !impl_->terminal_visible.load(std::memory_order_relaxed);
}

Status InputFrame::update_selection(const SelectionEvent& event) {
  const std::unique_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    if (event.type == SelectionEventType::RESET) {
      impl_->normal.selection.reset();
      impl_->normal.keyboard_anchor.reset();
      impl_->command.selection.reset();
      impl_->command.keyboard_anchor.reset();
      return Status::OK;
    }
    return Status::FRAME_NOT_SELECTABLE;
  }
  Impl::Buffer& buffer = impl_->active_buffer();
  impl_->escape_started.reset();
  if (event.type == SelectionEventType::RESET) {
    buffer.selection.reset();
    buffer.keyboard_anchor.reset();
    return Status::OK;
  }
  if (event.type == SelectionEventType::SELECT_ALL) {
    if (!Impl::has_input(buffer)) {
      return Status::NO_SELECTION;
    }
    buffer.selection = Impl::SelectionRange{
        .first = {},
        .last  = {.line   = buffer.lines.size() - 1U,
                  .column = buffer.lines.back().size()},
    };
    buffer.keyboard_anchor.reset();
    return Status::OK;
  }

  const std::optional<InputCursor> extent = impl_->map_character(event.extent);
  if (!extent.has_value()) {
    return Status::NO_SELECTION;
  }
  if (event.type == SelectionEventType::SELECT_WORD) {
    const std::u32string& line = buffer.lines[extent->line];
    std::size_t first          = extent->column;
    std::size_t last           = extent->column + 1U;
    if (is_word_character(line[extent->column])) {
      while (first > 0U && is_word_character(line[first - 1U])) {
        --first;
      }
      while (last < line.size() && is_word_character(line[last])) {
        ++last;
      }
    }
    buffer.selection = Impl::SelectionRange{
        .first = {.line = extent->line, .column = first},
        .last  = {.line = extent->line, .column = last},
    };
    buffer.keyboard_anchor.reset();
    return Status::OK;
  }
  if (event.type == SelectionEventType::SELECT_LINE) {
    const std::u32string& line = buffer.lines[extent->line];
    buffer.selection           = Impl::SelectionRange{
                  .first = {.line = extent->line, .column = 0U},
                  .last  = {.line = extent->line, .column = line.size()},
    };
    buffer.keyboard_anchor.reset();
    return Status::OK;
  }

  const std::optional<InputCursor> anchor = impl_->map_character(event.anchor);
  if (!anchor.has_value()) {
    return Status::NO_SELECTION;
  }
  if (position_less(*extent, *anchor)) {
    buffer.selection = Impl::SelectionRange{
        .first = *extent,
        .last  = Impl::after_character(buffer, *anchor),
    };
  } else {
    buffer.selection = Impl::SelectionRange{
        .first = *anchor,
        .last  = Impl::after_character(buffer, *extent),
    };
  }
  buffer.keyboard_anchor.reset();
  return Status::OK;
}

Status InputFrame::selected_text(std::string& output) const {
  const std::shared_lock lock(impl_->mutex);
  if (impl_->mode == InputMode::TERMINAL) {
    output.clear();
    return Status::FRAME_NOT_SELECTABLE;
  }
  return Impl::selected_text(impl_->active_buffer(), output);
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
  const std::optional<InputCursor> mapped = impl_->map_caret(position);
  if (!mapped.has_value()) {
    return Status::OK;
  }
  Impl::Buffer& buffer = impl_->active_buffer();
  buffer.cursor        = *mapped;
  buffer.selection.reset();
  buffer.keyboard_anchor.reset();
  buffer.has_desired_column = false;
  impl_->ensure_cursor_visible(buffer);
  return Status::OK;
}

}  // namespace puc::tui
