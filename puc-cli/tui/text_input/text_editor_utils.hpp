#pragma once

/**
 * @file text_editor_utils.hpp
 * @brief Reusable UTF-8 text editing, navigation, wrapping, and selection.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/tui/rendering/selection.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/terminal/event.hpp"

namespace puc::tui {

/** Zero-based logical caret position in a newline-delimited text buffer. */
struct TextCursor {
  std::size_t line   = 0U; /**< Logical line index. */
  std::size_t column = 0U; /**< Unicode-scalar offset within the line. */

  /** Compare both logical coordinates. */
  constexpr bool operator==(const TextCursor&) const noexcept = default;
};

/** Half-open logical selection range. */
struct TextSelectionRange {
  TextCursor first; /**< First included caret position. */
  TextCursor last;  /**< First excluded caret position. */

  /** Compare both range endpoints. */
  constexpr bool operator==(const TextSelectionRange&) const noexcept = default;
};

/** Construction policy shared by reusable editor-backed frames. */
struct TextEditorOptions {
  std::size_t tab_width      = 4U;  /**< Spaces between tab stops. */
  std::size_t fallback_width = 35U; /**< Wrap width before the first layout. */
  bool reserve_plain_enter = true;  /**< Leave unmodified Enter to the owner. */
};

/** Read-only logical state of one text editor. */
struct TextEditorSnapshot {
  std::string text;               /**< Newline-delimited UTF-8 contents. */
  TextCursor cursor;              /**< Active insertion position. */
  std::size_t scroll_row = 0U;    /**< First visible wrapped row. */
  bool paste_in_progress = false; /**< Whether a paste transaction is open. */
  std::optional<TextSelectionRange> selection; /**< Active logical range. */
};

/** One renderable Unicode scalar and its selection state. */
struct TextEditorRenderCell {
  char32_t character = U' ';  /**< Source Unicode scalar. */
  bool selected      = false; /**< Whether the scalar is selected. */
};

/** One visible row created by wrapping a logical source line. */
struct TextEditorRenderRow {
  std::size_t logical_line    = 0U;   /**< Zero-based source line. */
  std::size_t first_character = 0U;   /**< Source offset of the first cell. */
  bool first_visual_row       = true; /**< Whether annotations belong here. */
  std::vector<TextEditorRenderCell> cells; /**< Visible source characters. */
};

/** Consistent render snapshot for the current editor viewport. */
struct TextEditorRenderState {
  std::vector<TextEditorRenderRow> rows; /**< Visible wrapped rows. */
  std::optional<std::size_t> cursor_row; /**< Viewport-relative caret row. */
  std::size_t cursor_column = 0U;        /**< Viewport-relative caret column. */
};

/** UTF-8 and lexical helpers shared by editor-like views. */
namespace text_editor {

/** Append one valid Unicode scalar as UTF-8, replacing invalid scalars. */
void append_utf8(char32_t character, std::string& output);

/** Decode complete UTF-8, replacing malformed input with U+FFFD. */
std::u32string decode_utf8(std::string_view text);

/** Encode Unicode scalars as UTF-8. */
std::string encode_utf8(std::u32string_view text);

/** Return whether a scalar participates in editor word navigation. */
bool is_word_character(char32_t character) noexcept;

/** Return whether `left` precedes `right` in logical reading order. */
constexpr bool cursor_less(const TextCursor& left,
                           const TextCursor& right) noexcept {
  return left.line < right.line ||
         (left.line == right.line && left.column < right.column);
}

}  // namespace text_editor

/**
 * Reusable text-buffer state machine independent of rendering and mode UI.
 *
 * The model owns Unicode-scalar text, a caret, selection, vertical scroll,
 * transactional paste state, and wrapped-row navigation. It deliberately does
 * not own a mutex or clipboard/terminal I/O: a containing Frame synchronizes
 * access and moves selected UTF-8 through Screen's clipboard integration.
 */
class TextEditor {
 public:
  /** Construct an empty editor with the supplied policy. */
  explicit TextEditor(TextEditorOptions options = {});

  TextEditor(const TextEditor&)            = delete;
  TextEditor& operator=(const TextEditor&) = delete;
  TextEditor(TextEditor&&)                 = delete;
  TextEditor& operator=(TextEditor&&)      = delete;

  /** Destroy the hidden logical buffer. */
  ~TextEditor();

  /** Replace the current viewport and keep its caret visible. */
  void set_viewport(std::size_t width, std::size_t height);

  /** Change the wrap width used before a nonzero viewport is available. */
  void set_fallback_width(std::size_t width) noexcept;

  /** Return a consistent copy of logical editor state. */
  TextEditorSnapshot snapshot() const;

  /** Return the complete UTF-8 buffer. */
  std::string text() const;

  /** Return zero for pristine input, otherwise the logical line count. */
  std::size_t logical_line_count() const noexcept;

  /** Return the wrapped-row count for an explicit positive width. */
  std::size_t visual_row_count(std::size_t width) const;

  /** Return cells and caret coordinates for the current visible viewport. */
  TextEditorRenderState render_state() const;

  /** Clear text, caret, selection, scrolling, and any paste transaction. */
  void clear();

  /** Insert committed UTF-8 at the caret, replacing any selection. */
  void insert_utf8(std::string_view text);

  /** Apply a decoded key press; releases and modifier-only keys are ignored. */
  void handle_key(const terminal::KeyEvent& event);

  /** Insert a committed text event unless a paste transaction owns input. */
  void handle_text(const terminal::TextEvent& event);

  /** Apply one transactional bracketed-paste stage. */
  Status handle_paste(const terminal::PasteEvent& event);

  /** Apply a reusable navigation or select-all command. */
  void apply_command(terminal::Command command);

  /** Scroll vertically by signed wrapped rows; horizontal input has no API. */
  void scroll_vertical(std::int64_t delta);

  /** Apply a logical selection event in editor-local cell coordinates. */
  Status update_selection(const SelectionEvent& event);

  /** Extract selected UTF-8 without performing clipboard I/O. */
  Status selected_text(std::string& output) const;

  /** Clear the active selection without changing text or the caret. */
  void reset_selection() noexcept;

  /** Place the caret at an editor-local cell when that cell is visible. */
  Status place_cursor(SelectionPosition position);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden reusable editor representation. */
};

}  // namespace puc::tui
