/**
 * @file text_editor_utils.cpp
 * @brief Reusable Unicode text-buffer mechanics for editor-backed frames.
 */

#include "puc-cli/tui/text_input/text_editor_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace puc::tui {
namespace {

/** Return whether a byte is a UTF-8 continuation byte. */
constexpr bool is_continuation(unsigned char byte) noexcept {
  return (byte & 0xc0U) == 0x80U;
}

/**
 * Decode every complete scalar at the start of `bytes`.
 *
 * Invalid encodings become U+FFFD. When `finish` is false, a potentially valid
 * incomplete suffix remains in `bytes` for a later paste chunk.
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

}  // namespace

namespace text_editor {

void append_utf8(char32_t character, std::string& output) {
  std::uint32_t codepoint = static_cast<std::uint32_t>(character);
  if (codepoint > 0x10ffffU || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
    codepoint = 0xfffdU;
  }
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

std::u32string decode_utf8(std::string_view text) {
  std::string bytes{text};
  return consume_utf8(bytes, true);
}

std::string encode_utf8(std::u32string_view text) {
  std::string output;
  for (const char32_t character : text) {
    append_utf8(character, output);
  }
  return output;
}

bool is_word_character(char32_t character) noexcept {
  if (character > 0x7fU) {
    return true;
  }
  const auto byte = static_cast<unsigned char>(character);
  return std::isalnum(byte) != 0 || byte == '_' || byte == '-';
}

}  // namespace text_editor

/** Hidden logical text buffer and reusable editing operations. */
class TextEditor::Impl {
 public:
  /** One display row produced by wrapping one logical line. */
  struct VisualRow {
    std::size_t line   = 0U; /**< Logical source line. */
    std::size_t first  = 0U; /**< First source scalar on this row. */
    std::size_t length = 0U; /**< Source scalars on this row. */
  };

  /** Rollback state for one streaming bracketed paste. */
  struct PasteTransaction {
    std::vector<std::u32string> lines; /**< Exact pre-paste contents. */
    TextCursor cursor;                 /**< Exact pre-paste caret. */
    std::optional<TextSelectionRange> selection; /**< Prior selection. */
    std::optional<TextCursor> keyboard_anchor;   /**< Prior Shift anchor. */
    std::size_t scroll_row     = 0U;             /**< Prior viewport origin. */
    std::size_t desired_column = 0U;             /**< Prior sticky column. */
    bool has_desired_column    = false; /**< Prior sticky-column state. */
    std::string pending_utf8;  /**< Incomplete UTF-8 suffix across chunks. */
    bool preceding_cr = false; /**< Whether the preceding scalar was CR. */
  };

  /** Normalize and retain the reusable editor policy. */
  explicit Impl(TextEditorOptions supplied) : options(supplied) {
    options.tab_width      = std::max<std::size_t>(options.tab_width, 1U);
    options.fallback_width = std::max<std::size_t>(options.fallback_width, 1U);
  }

  /** Return whether any user-created logical input exists. */
  bool has_input() const noexcept {
    return lines.size() > 1U || !lines.front().empty();
  }

  /** Return the active positive wrap width. */
  std::size_t editable_width() const noexcept {
    return viewport_width == 0U ? options.fallback_width : viewport_width;
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
        return std::nullopt;
      }

      std::size_t word_first = word_character;
      while (word_first > row_first && line[word_first - 1U] != U' ') {
        --word_first;
      }
      if (word_first > row_first) {
        return word_first - 1U;
      }
      row_first += width;
    }
    return std::nullopt;
  }

  /** Replace word-boundary soft wraps with logical newline boundaries. */
  void materialize_word_wraps(std::size_t first_line) {
    std::size_t line_index = first_line;
    std::size_t final_line = first_line;
    while (line_index <= final_line) {
      std::u32string& line = lines[line_index];
      const std::optional<std::size_t> separator =
          materialized_wrap_break(line, editable_width());
      if (!separator.has_value()) {
        ++line_index;
        continue;
      }

      std::u32string suffix = line.substr(*separator + 1U);
      line.erase(*separator);
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(line_index + 1U),
                   std::move(suffix));
      if (cursor.line == line_index && cursor.column > *separator) {
        ++cursor.line;
        cursor.column -= *separator + 1U;
      } else if (cursor.line > line_index) {
        ++cursor.line;
      }
      ++final_line;
      ++line_index;
    }
  }

  /** Wrap all logical lines for one positive width. */
  std::vector<VisualRow> visual_rows(std::size_t width) const {
    std::vector<VisualRow> rows;
    if (width == 0U) {
      return rows;
    }
    for (std::size_t line_index = 0U; line_index < lines.size(); ++line_index) {
      const std::u32string& line = lines[line_index];
      if (line.empty()) {
        rows.push_back(VisualRow{.line = line_index});
        continue;
      }

      std::size_t first = 0U;
      while (first < line.size()) {
        const std::size_t remaining = line.size() - first;
        std::size_t length          = std::min(width, remaining);
        const std::size_t hard_end  = first + width;
        if (remaining > width && line[hard_end - 1U] != U' ' &&
            line[hard_end] != U' ') {
          for (std::size_t candidate = hard_end; candidate > first;
               --candidate) {
            if (line[candidate - 1U] == U' ') {
              length = candidate - first;
              break;
            }
          }
        }
        rows.push_back(VisualRow{
            .line   = line_index,
            .first  = first,
            .length = length,
        });
        first += length;
      }
      if (line_index == cursor.line && cursor.column == line.size() &&
          !rows.empty() && rows.back().line == line_index &&
          rows.back().length == width) {
        rows.push_back(VisualRow{.line = line_index, .first = line.size()});
      }
    }
    return rows;
  }

  /** Find the wrapped row and column containing the caret. */
  std::pair<std::size_t, std::size_t> visual_cursor(
      const std::vector<VisualRow>& rows, std::size_t width) const noexcept {
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      const VisualRow& row = rows[index];
      if (row.line != cursor.line) {
        continue;
      }
      if (row.length == 0U && row.first == cursor.column) {
        return {index, 0U};
      }
      if (cursor.column >= row.first &&
          cursor.column < row.first + row.length) {
        return {index, cursor.column - row.first};
      }
      if (row.length < width && cursor.column == row.first + row.length) {
        return {index, row.length};
      }
    }
    return {rows.empty() ? 0U : rows.size() - 1U, 0U};
  }

  /** Restrict scroll origin to the current wrapped content. */
  void clamp_scroll() {
    if (viewport_width == 0U) {
      scroll_row = 0U;
      return;
    }
    const std::vector<VisualRow> rows = visual_rows(viewport_width);
    const std::size_t maximum =
        rows.size() > viewport_height ? rows.size() - viewport_height : 0U;
    scroll_row = std::min(scroll_row, maximum);
  }

  /** Scroll just enough to keep the caret visible. */
  void ensure_cursor_visible() {
    if (viewport_width == 0U || viewport_height == 0U) {
      return;
    }
    const std::vector<VisualRow> rows = visual_rows(viewport_width);
    if (rows.empty()) {
      scroll_row = 0U;
      return;
    }
    const std::size_t row = visual_cursor(rows, viewport_width).first;
    if (row < scroll_row) {
      scroll_row = row;
    } else if (row >= scroll_row + viewport_height) {
      scroll_row = row - viewport_height + 1U;
    }
    const std::size_t maximum =
        rows.size() > viewport_height ? rows.size() - viewport_height : 0U;
    scroll_row = std::min(scroll_row, maximum);
  }

  /** Return a caret one scalar after a mapped character. */
  TextCursor after_character(TextCursor position) const noexcept {
    if (position.line < lines.size() &&
        position.column < lines[position.line].size()) {
      ++position.column;
    }
    return position;
  }

  /** Delete the active range and place the caret at its start. */
  bool delete_selection() {
    if (!selection.has_value()) {
      return false;
    }
    const TextSelectionRange range = *selection;
    if (range.first.line == range.last.line) {
      std::u32string& line = lines[range.first.line];
      line.erase(range.first.column, range.last.column - range.first.column);
    } else {
      std::u32string merged =
          lines[range.first.line].substr(0U, range.first.column);
      merged.append(lines[range.last.line].substr(range.last.column));
      lines[range.first.line] = std::move(merged);
      lines.erase(
          lines.begin() + static_cast<std::ptrdiff_t>(range.first.line + 1U),
          lines.begin() + static_cast<std::ptrdiff_t>(range.last.line + 1U));
    }
    cursor = range.first;
    selection.reset();
    keyboard_anchor.reset();
    has_desired_column = false;
    return true;
  }

  /** Insert normalized Unicode scalars at the caret. */
  void insert(std::u32string_view characters, bool& preceding_cr) {
    static_cast<void>(delete_selection());
    for (const char32_t character : characters) {
      if (character == U'\n' && preceding_cr) {
        preceding_cr = false;
        continue;
      }
      if (character == U'\r' || character == U'\n') {
        std::u32string suffix = lines[cursor.line].substr(cursor.column);
        lines[cursor.line].erase(cursor.column);
        lines.insert(
            lines.begin() + static_cast<std::ptrdiff_t>(cursor.line + 1U),
            std::move(suffix));
        ++cursor.line;
        cursor.column = 0U;
        preceding_cr  = character == U'\r';
        continue;
      }
      preceding_cr = false;
      if (character == U'\t') {
        const std::size_t insertion_line = cursor.line;
        const std::size_t count =
            options.tab_width - (cursor.column % options.tab_width);
        lines[cursor.line].insert(cursor.column, count, U' ');
        cursor.column += count;
        materialize_word_wraps(insertion_line);
        continue;
      }
      if (character < 0x20U || character == 0x7fU) {
        continue;
      }
      lines[cursor.line].insert(lines[cursor.line].begin() +
                                    static_cast<std::ptrdiff_t>(cursor.column),
                                character);
      ++cursor.column;
      materialize_word_wraps(cursor.line);
    }
    selection.reset();
    keyboard_anchor.reset();
    has_desired_column = false;
    ensure_cursor_visible();
  }

  /** Remove the scalar before the caret or join the preceding line. */
  void backspace() {
    if (delete_selection()) {
      ensure_cursor_visible();
      return;
    }
    if (cursor.column > 0U) {
      std::u32string& line = lines[cursor.line];
      line.erase(cursor.column - 1U, 1U);
      --cursor.column;
    } else if (cursor.line > 0U) {
      const std::size_t preceding_line = cursor.line - 1U;
      const std::size_t preceding_size = lines[preceding_line].size();
      lines[preceding_line].append(lines[cursor.line]);
      lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(cursor.line));
      cursor = {.line = preceding_line, .column = preceding_size};
    }
    keyboard_anchor.reset();
    has_desired_column = false;
    ensure_cursor_visible();
  }

  /** Remove the scalar under the caret or join the following line. */
  void delete_forward() {
    if (delete_selection()) {
      ensure_cursor_visible();
      return;
    }
    std::u32string& line = lines[cursor.line];
    if (cursor.column < line.size()) {
      line.erase(cursor.column, 1U);
    } else if (cursor.line + 1U < lines.size()) {
      line.append(lines[cursor.line + 1U]);
      lines.erase(lines.begin() +
                  static_cast<std::ptrdiff_t>(cursor.line + 1U));
    }
    keyboard_anchor.reset();
    has_desired_column = false;
    ensure_cursor_visible();
  }

  /** Update a Shift-navigation range after moving the caret. */
  void finish_navigation(TextCursor old_cursor, bool extend) {
    if (!extend) {
      selection.reset();
      keyboard_anchor.reset();
      return;
    }
    if (!keyboard_anchor.has_value()) {
      keyboard_anchor = old_cursor;
    }
    const TextCursor anchor = *keyboard_anchor;
    if (anchor == cursor) {
      selection.reset();
    } else if (text_editor::cursor_less(cursor, anchor)) {
      selection = TextSelectionRange{.first = cursor, .last = anchor};
    } else {
      selection = TextSelectionRange{.first = anchor, .last = cursor};
    }
  }

  /** Move one scalar horizontally across logical newline boundaries. */
  void move_horizontal(bool right, bool extend) {
    const TextCursor old_cursor = cursor;
    if (right) {
      if (cursor.column < lines[cursor.line].size()) {
        ++cursor.column;
      } else if (cursor.line + 1U < lines.size()) {
        ++cursor.line;
        cursor.column = 0U;
      }
    } else if (cursor.column > 0U) {
      --cursor.column;
    } else if (cursor.line > 0U) {
      --cursor.line;
      cursor.column = lines[cursor.line].size();
    }
    has_desired_column = false;
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move to the start of the preceding word. */
  void move_word_left(bool extend = false) {
    const TextCursor old_cursor = cursor;
    if (cursor.column == 0U) {
      if (cursor.line == 0U) {
        finish_navigation(old_cursor, extend);
        return;
      }
      --cursor.line;
      cursor.column = lines[cursor.line].size();
    }
    std::u32string& line = lines[cursor.line];
    while (cursor.column > 0U &&
           !text_editor::is_word_character(line[cursor.column - 1U])) {
      --cursor.column;
    }
    while (cursor.column > 0U &&
           text_editor::is_word_character(line[cursor.column - 1U])) {
      --cursor.column;
    }
    has_desired_column = false;
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move to the start of the following word. */
  void move_word_right(bool extend = false) {
    const TextCursor old_cursor = cursor;
    std::u32string* line        = &lines[cursor.line];
    if (cursor.column == line->size() && cursor.line + 1U < lines.size()) {
      ++cursor.line;
      cursor.column = 0U;
      line          = &lines[cursor.line];
    }
    while (cursor.column < line->size() &&
           text_editor::is_word_character((*line)[cursor.column])) {
      ++cursor.column;
    }
    while (cursor.column < line->size() &&
           !text_editor::is_word_character((*line)[cursor.column])) {
      ++cursor.column;
    }
    has_desired_column = false;
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move vertically through wrapped rows with a sticky desired column. */
  void move_vertical(std::int64_t row_delta, bool extend) {
    const std::size_t width           = editable_width();
    const std::vector<VisualRow> rows = visual_rows(width);
    if (rows.empty()) {
      return;
    }
    const TextCursor old_cursor              = cursor;
    const auto [current_row, current_column] = visual_cursor(rows, width);
    if (!has_desired_column) {
      desired_column     = current_column;
      has_desired_column = true;
    }
    std::size_t target = current_row;
    if (row_delta < 0) {
      const std::uint64_t distance =
          static_cast<std::uint64_t>(-(row_delta + 1)) + 1U;
      target =
          distance > target ? 0U : target - static_cast<std::size_t>(distance);
    } else {
      const std::uint64_t distance = static_cast<std::uint64_t>(row_delta);
      const std::size_t remaining  = rows.size() - 1U - target;
      target += static_cast<std::size_t>(
          std::min<std::uint64_t>(distance, remaining));
    }
    const VisualRow& row = rows[target];
    cursor               = {
        .line   = row.line,
        .column = row.first + std::min(desired_column, row.length),
    };
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move to one wrapped-row boundary. */
  void move_row_boundary(bool end, bool extend) {
    const TextCursor old_cursor       = cursor;
    const std::size_t width           = editable_width();
    const std::vector<VisualRow> rows = visual_rows(width);
    if (!rows.empty()) {
      const VisualRow& row = rows[visual_cursor(rows, width).first];
      cursor               = {
          .line   = row.line,
          .column = end ? row.first + row.length : row.first,
      };
    }
    has_desired_column = false;
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move to the first or final caret in the buffer. */
  void move_buffer_boundary(bool end, bool extend) {
    const TextCursor old_cursor = cursor;
    cursor             = end ? TextCursor{.line   = lines.size() - 1U,
                                          .column = lines.back().size()}
                             : TextCursor{};
    has_desired_column = false;
    finish_navigation(old_cursor, extend);
    ensure_cursor_visible();
  }

  /** Move by one visible viewport. */
  void move_page(bool down, bool extend = false) {
    const std::size_t page   = std::max<std::size_t>(viewport_height, 1U);
    const std::int64_t delta = static_cast<std::int64_t>(std::min<std::size_t>(
        page,
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
    move_vertical(down ? delta : -delta, extend);
  }

  /** Map a local cell to a visible source character, clamping drag extents. */
  std::optional<TextCursor> map_character(SelectionPosition position) const {
    if (viewport_width == 0U || viewport_height == 0U) {
      return std::nullopt;
    }
    const std::vector<VisualRow> rows = visual_rows(viewport_width);
    if (rows.empty()) {
      return std::nullopt;
    }

    std::size_t visual_index = scroll_row;
    if (position.y < 0) {
      const std::uint64_t distance =
          static_cast<std::uint64_t>(-(position.y + 1)) + 1U;
      visual_index = distance > visual_index
                         ? 0U
                         : visual_index - static_cast<std::size_t>(distance);
    } else {
      const std::uint64_t distance = static_cast<std::uint64_t>(position.y);
      const std::size_t remaining  = rows.size() - 1U - visual_index;
      visual_index += static_cast<std::size_t>(
          std::min<std::uint64_t>(distance, remaining));
    }

    const VisualRow& row = rows[visual_index];
    if (row.length == 0U) {
      return std::nullopt;
    }
    std::size_t column =
        position.x > 0 ? static_cast<std::size_t>(position.x) : 0U;
    column = std::min(column, row.length - 1U);
    return TextCursor{.line = row.line, .column = row.first + column};
  }

  /** Map an in-viewport local cell to a caret boundary. */
  std::optional<TextCursor> map_caret(SelectionPosition position) const {
    if (viewport_width == 0U || viewport_height == 0U || position.x < 0 ||
        position.y < 0 ||
        position.x >= static_cast<std::int64_t>(viewport_width) ||
        position.y >= static_cast<std::int64_t>(viewport_height)) {
      return std::nullopt;
    }
    const std::vector<VisualRow> rows = visual_rows(viewport_width);
    const std::size_t visual_index =
        scroll_row + static_cast<std::size_t>(position.y);
    if (visual_index >= rows.size()) {
      return std::nullopt;
    }
    const VisualRow& row     = rows[visual_index];
    const std::size_t offset = static_cast<std::size_t>(position.x);
    return TextCursor{
        .line   = row.line,
        .column = row.first + std::min(offset, row.length),
    };
  }

  /** Return whether a source scalar lies inside the active range. */
  bool is_selected(TextCursor position) const noexcept {
    return selection.has_value() &&
           !text_editor::cursor_less(position, selection->first) &&
           text_editor::cursor_less(position, selection->last);
  }

  TextEditorOptions options;             /**< Editing policy. */
  std::vector<std::u32string> lines{1U}; /**< Newline-delimited text. */
  TextCursor cursor;                     /**< Active insertion position. */
  std::optional<TextSelectionRange> selection; /**< Active logical range. */
  std::optional<TextCursor> keyboard_anchor;   /**< Shift-navigation origin. */
  std::size_t scroll_row      = 0U;    /**< First visible wrapped row. */
  std::size_t desired_column  = 0U;    /**< Sticky vertical-motion column. */
  bool has_desired_column     = false; /**< Whether desired_column is active. */
  std::size_t viewport_width  = 0U;    /**< Latest render width. */
  std::size_t viewport_height = 0U;    /**< Latest render height. */
  std::optional<PasteTransaction> paste; /**< Active transactional paste. */
};

TextEditor::TextEditor(TextEditorOptions options)
    : impl_(std::make_unique<Impl>(options)) {}

TextEditor::~TextEditor() = default;

void TextEditor::set_viewport(std::size_t width, std::size_t height) {
  const bool changed =
      impl_->viewport_width != width || impl_->viewport_height != height;
  impl_->viewport_width  = width;
  impl_->viewport_height = height;
  impl_->clamp_scroll();
  if (changed) {
    impl_->ensure_cursor_visible();
  }
}

void TextEditor::set_fallback_width(std::size_t width) noexcept {
  impl_->options.fallback_width = std::max<std::size_t>(width, 1U);
}

TextEditorSnapshot TextEditor::snapshot() const {
  return TextEditorSnapshot{
      .text              = text(),
      .cursor            = impl_->cursor,
      .scroll_row        = impl_->scroll_row,
      .paste_in_progress = impl_->paste.has_value(),
      .selection         = impl_->selection,
  };
}

std::string TextEditor::text() const {
  std::string output;
  for (std::size_t line = 0U; line < impl_->lines.size(); ++line) {
    output.append(text_editor::encode_utf8(impl_->lines[line]));
    if (line + 1U < impl_->lines.size()) {
      output.push_back('\n');
    }
  }
  return output;
}

std::size_t TextEditor::logical_line_count() const noexcept {
  return impl_->has_input() ? impl_->lines.size() : 0U;
}

std::size_t TextEditor::visual_row_count(std::size_t width) const {
  return impl_->visual_rows(width).size();
}

TextEditorRenderState TextEditor::render_state() const {
  TextEditorRenderState output;
  if (impl_->viewport_width == 0U || impl_->viewport_height == 0U) {
    return output;
  }
  const std::vector<Impl::VisualRow> rows =
      impl_->visual_rows(impl_->viewport_width);
  const auto [cursor_row, cursor_column] =
      impl_->visual_cursor(rows, impl_->viewport_width);
  const std::size_t available =
      impl_->scroll_row < rows.size() ? rows.size() - impl_->scroll_row : 0U;
  const std::size_t count = std::min(impl_->viewport_height, available);
  output.rows.reserve(count);
  for (std::size_t visible = 0U; visible < count; ++visible) {
    const Impl::VisualRow& row = rows[impl_->scroll_row + visible];
    TextEditorRenderRow rendered{
        .logical_line     = row.line,
        .first_character  = row.first,
        .first_visual_row = row.first == 0U,
    };
    rendered.cells.reserve(row.length);
    for (std::size_t column = 0U; column < row.length; ++column) {
      const TextCursor logical{.line = row.line, .column = row.first + column};
      rendered.cells.push_back(TextEditorRenderCell{
          .character = impl_->lines[row.line][logical.column],
          .selected  = impl_->is_selected(logical),
      });
    }
    output.rows.push_back(std::move(rendered));
  }
  if (cursor_row >= impl_->scroll_row &&
      cursor_row < impl_->scroll_row + impl_->viewport_height) {
    output.cursor_row    = cursor_row - impl_->scroll_row;
    output.cursor_column = cursor_column;
  }
  return output;
}

void TextEditor::clear() { impl_ = std::make_unique<Impl>(impl_->options); }

void TextEditor::insert_utf8(std::string_view text) {
  bool preceding_cr = false;
  impl_->insert(text_editor::decode_utf8(text), preceding_cr);
}

void TextEditor::handle_key(const terminal::KeyEvent& event) {
  if (event.action == terminal::KeyAction::RELEASE ||
      impl_->paste.has_value()) {
    return;
  }
  const auto* named = std::get_if<terminal::NamedKey>(&event.key.value);
  if (named != nullptr && is_modifier_key(*named)) {
    return;
  }
  const bool extend  = event.modifiers.contains(terminal::Modifier::SHIFT);
  const bool option  = event.modifiers.contains(terminal::Modifier::ALT);
  const bool command = event.modifiers.contains(terminal::Modifier::SUPER) ||
                       event.modifiers.contains(terminal::Modifier::META);
  if (named != nullptr) {
    switch (*named) {
      case terminal::NamedKey::ENTER:
      case terminal::NamedKey::KEYPAD_ENTER: {
        if (impl_->options.reserve_plain_enter && !extend) {
          return;
        }
        bool preceding_cr = false;
        impl_->insert(U"\n", preceding_cr);
        return;
      }
      case terminal::NamedKey::TAB: {
        bool preceding_cr = false;
        impl_->insert(U"\t", preceding_cr);
        return;
      }
      case terminal::NamedKey::BACKSPACE:
        impl_->backspace();
        return;
      case terminal::NamedKey::DELETE_KEY:
      case terminal::NamedKey::KEYPAD_DELETE:
        impl_->delete_forward();
        return;
      case terminal::NamedKey::LEFT:
      case terminal::NamedKey::KEYPAD_LEFT:
        if (command) {
          impl_->move_row_boundary(false, extend);
        } else if (option) {
          impl_->move_word_left(extend);
        } else {
          impl_->move_horizontal(false, extend);
        }
        return;
      case terminal::NamedKey::RIGHT:
      case terminal::NamedKey::KEYPAD_RIGHT:
        if (command) {
          impl_->move_row_boundary(true, extend);
        } else if (option) {
          impl_->move_word_right(extend);
        } else {
          impl_->move_horizontal(true, extend);
        }
        return;
      case terminal::NamedKey::UP:
      case terminal::NamedKey::KEYPAD_UP:
        if (command) {
          impl_->move_buffer_boundary(false, extend);
        } else if (option) {
          impl_->move_page(false, extend);
        } else {
          impl_->move_vertical(-1, extend);
        }
        return;
      case terminal::NamedKey::DOWN:
      case terminal::NamedKey::KEYPAD_DOWN:
        if (command) {
          impl_->move_buffer_boundary(true, extend);
        } else if (option) {
          impl_->move_page(true, extend);
        } else {
          impl_->move_vertical(1, extend);
        }
        return;
      case terminal::NamedKey::HOME:
      case terminal::NamedKey::KEYPAD_HOME:
        impl_->move_row_boundary(false, extend);
        return;
      case terminal::NamedKey::END:
      case terminal::NamedKey::KEYPAD_END:
        impl_->move_row_boundary(true, extend);
        return;
      case terminal::NamedKey::PAGE_UP:
      case terminal::NamedKey::KEYPAD_PAGE_UP:
        impl_->move_page(false);
        return;
      case terminal::NamedKey::PAGE_DOWN:
      case terminal::NamedKey::KEYPAD_PAGE_DOWN:
        impl_->move_page(true);
        return;
      default:
        return;
    }
  }

  if (event.modifiers.contains(terminal::Modifier::CONTROL) ||
      event.modifiers.contains(terminal::Modifier::SUPER) ||
      event.modifiers.contains(terminal::Modifier::HYPER) ||
      event.modifiers.contains(terminal::Modifier::META)) {
    return;
  }
  if (!event.text.empty()) {
    insert_utf8(event.text);
    return;
  }
  const auto* character = std::get_if<char32_t>(&event.key.value);
  if (character == nullptr) {
    return;
  }
  std::string utf8;
  text_editor::append_utf8(event.shifted_key.value_or(*character), utf8);
  insert_utf8(utf8);
}

void TextEditor::handle_text(const terminal::TextEvent& event) {
  if (!impl_->paste.has_value()) {
    insert_utf8(event.utf8);
  }
}

Status TextEditor::handle_paste(const terminal::PasteEvent& event) {
  switch (event.phase) {
    case terminal::PastePhase::BEGIN:
      if (impl_->paste.has_value()) {
        return Status::INVALID_ARGUMENT;
      }
      impl_->paste = Impl::PasteTransaction{
          .lines              = impl_->lines,
          .cursor             = impl_->cursor,
          .selection          = impl_->selection,
          .keyboard_anchor    = impl_->keyboard_anchor,
          .scroll_row         = impl_->scroll_row,
          .desired_column     = impl_->desired_column,
          .has_desired_column = impl_->has_desired_column,
      };
      return Status::OK;

    case terminal::PastePhase::DATA:
      if (!impl_->paste.has_value()) {
        return Status::INVALID_ARGUMENT;
      }
      impl_->paste->pending_utf8.append(event.data);
      impl_->insert(consume_utf8(impl_->paste->pending_utf8, false),
                    impl_->paste->preceding_cr);
      return Status::OK;

    case terminal::PastePhase::END:
      if (!impl_->paste.has_value()) {
        return Status::INVALID_ARGUMENT;
      }
      impl_->insert(consume_utf8(impl_->paste->pending_utf8, true),
                    impl_->paste->preceding_cr);
      impl_->paste.reset();
      return Status::OK;

    case terminal::PastePhase::CANCEL:
      if (!impl_->paste.has_value()) {
        return Status::OK;
      }
      impl_->lines              = std::move(impl_->paste->lines);
      impl_->cursor             = impl_->paste->cursor;
      impl_->selection          = impl_->paste->selection;
      impl_->keyboard_anchor    = impl_->paste->keyboard_anchor;
      impl_->scroll_row         = impl_->paste->scroll_row;
      impl_->desired_column     = impl_->paste->desired_column;
      impl_->has_desired_column = impl_->paste->has_desired_column;
      impl_->paste.reset();
      return Status::OK;
  }
  return Status::INVALID_ARGUMENT;
}

void TextEditor::apply_command(terminal::Command command) {
  switch (command) {
    case terminal::Command::COPY:
    case terminal::Command::ENTER_COMMAND_MODE:
    case terminal::Command::ENTER_TERMINAL_MODE:
      return;
    case terminal::Command::SELECT_ALL:
      if (impl_->has_input()) {
        impl_->selection = TextSelectionRange{
            .first = {},
            .last  = {.line   = impl_->lines.size() - 1U,
                      .column = impl_->lines.back().size()},
        };
        impl_->keyboard_anchor.reset();
      }
      return;
    case terminal::Command::MOVE_WORD_LEFT:
      impl_->move_word_left();
      return;
    case terminal::Command::MOVE_WORD_RIGHT:
      impl_->move_word_right();
      return;
    case terminal::Command::MOVE_ROW_START:
      impl_->move_row_boundary(false, false);
      return;
    case terminal::Command::MOVE_ROW_END:
      impl_->move_row_boundary(true, false);
      return;
    case terminal::Command::MOVE_BUFFER_START:
      impl_->move_buffer_boundary(false, false);
      return;
    case terminal::Command::MOVE_BUFFER_END:
      impl_->move_buffer_boundary(true, false);
      return;
    case terminal::Command::MOVE_PAGE_UP:
      impl_->move_page(false);
      return;
    case terminal::Command::MOVE_PAGE_DOWN:
      impl_->move_page(true);
      return;
  }
}

void TextEditor::scroll_vertical(std::int64_t delta) {
  if (impl_->viewport_width == 0U || delta == 0) {
    return;
  }
  const std::vector<Impl::VisualRow> rows =
      impl_->visual_rows(impl_->viewport_width);
  const std::size_t maximum = rows.size() > impl_->viewport_height
                                  ? rows.size() - impl_->viewport_height
                                  : 0U;
  if (delta > 0) {
    const std::uint64_t amount = static_cast<std::uint64_t>(delta);
    impl_->scroll_row =
        amount > impl_->scroll_row
            ? 0U
            : impl_->scroll_row - static_cast<std::size_t>(amount);
  } else {
    const std::uint64_t amount = static_cast<std::uint64_t>(-(delta + 1)) + 1U;
    impl_->scroll_row += static_cast<std::size_t>(
        std::min<std::uint64_t>(amount, maximum - impl_->scroll_row));
  }
}

Status TextEditor::update_selection(const SelectionEvent& event) {
  if (event.type == SelectionEventType::RESET) {
    reset_selection();
    return Status::OK;
  }
  if (event.type == SelectionEventType::SELECT_ALL) {
    if (!impl_->has_input()) {
      return Status::NO_SELECTION;
    }
    impl_->selection = TextSelectionRange{
        .first = {},
        .last  = {.line   = impl_->lines.size() - 1U,
                  .column = impl_->lines.back().size()},
    };
    impl_->keyboard_anchor.reset();
    return Status::OK;
  }

  const std::optional<TextCursor> extent = impl_->map_character(event.extent);
  if (!extent.has_value()) {
    return Status::NO_SELECTION;
  }
  if (event.type == SelectionEventType::SELECT_WORD) {
    const std::u32string& line = impl_->lines[extent->line];
    std::size_t first          = extent->column;
    std::size_t last           = extent->column + 1U;
    if (text_editor::is_word_character(line[extent->column])) {
      while (first > 0U && text_editor::is_word_character(line[first - 1U])) {
        --first;
      }
      while (last < line.size() && text_editor::is_word_character(line[last])) {
        ++last;
      }
    }
    impl_->selection = TextSelectionRange{
        .first = {.line = extent->line, .column = first},
        .last  = {.line = extent->line, .column = last},
    };
    impl_->keyboard_anchor.reset();
    return Status::OK;
  }
  if (event.type == SelectionEventType::SELECT_LINE) {
    impl_->selection = TextSelectionRange{
        .first = {.line = extent->line, .column = 0U},
        .last  = {.line   = extent->line,
                  .column = impl_->lines[extent->line].size()},
    };
    impl_->keyboard_anchor.reset();
    return Status::OK;
  }

  const std::optional<TextCursor> anchor = impl_->map_character(event.anchor);
  if (!anchor.has_value()) {
    return Status::NO_SELECTION;
  }
  if (text_editor::cursor_less(*extent, *anchor)) {
    impl_->selection = TextSelectionRange{
        .first = *extent,
        .last  = impl_->after_character(*anchor),
    };
  } else {
    impl_->selection = TextSelectionRange{
        .first = *anchor,
        .last  = impl_->after_character(*extent),
    };
  }
  impl_->keyboard_anchor.reset();
  return Status::OK;
}

Status TextEditor::selected_text(std::string& output) const {
  output.clear();
  if (!impl_->selection.has_value()) {
    return Status::NO_SELECTION;
  }
  const TextSelectionRange range = *impl_->selection;
  for (std::size_t line = range.first.line; line <= range.last.line; ++line) {
    const std::size_t first =
        line == range.first.line ? range.first.column : 0U;
    const std::size_t last =
        line == range.last.line ? range.last.column : impl_->lines[line].size();
    output.append(text_editor::encode_utf8(
        std::u32string_view{impl_->lines[line]}.substr(first, last - first)));
    if (line < range.last.line) {
      output.push_back('\n');
    }
  }
  return Status::OK;
}

void TextEditor::reset_selection() noexcept {
  impl_->selection.reset();
  impl_->keyboard_anchor.reset();
}

Status TextEditor::place_cursor(SelectionPosition position) {
  const std::optional<TextCursor> mapped = impl_->map_caret(position);
  if (!mapped.has_value()) {
    return Status::OK;
  }
  impl_->cursor = *mapped;
  reset_selection();
  impl_->has_desired_column = false;
  impl_->ensure_cursor_visible();
  return Status::OK;
}

}  // namespace puc::tui
