#pragma once

/**
 * @file terminal_test_selection.hpp
 * @brief Logical text-selection state used by the terminal conformance frame.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "puc-cli/tui/rendering/selection.hpp"
#include "puc-cli/tui/rendering/status.hpp"

namespace puc::terminal {

/** One selectable logical line and its origin in frame-local cells. */
struct SelectableLine {
  std::int64_t x = 0; /**< Column containing the first logical character. */
  std::int64_t y = 0; /**< Row containing this logical line. */
  std::string text; /**< Text represented one byte per test-app Canvas cell. */

  /** Compare origin and logical contents. */
  bool operator==(const SelectableLine&) const = default;
};

/** One selected portion of a rendered line, with an inclusive final column. */
struct HighlightSpan {
  std::size_t line  = 0U; /**< Selectable-line index. */
  std::size_t first = 0U; /**< First selected character. */
  std::size_t last  = 0U; /**< Last selected character. */

  /** Compare line and inclusive character bounds. */
  constexpr bool operator==(const HighlightSpan&) const noexcept = default;
};

/**
 * Own the conformance frame's logical range independently of visual padding.
 *
 * The interactive test renders deliberately printable ASCII, so each byte is
 * one Canvas cell. Production Unicode frames remain free to implement their
 * own grapheme/cell mapping behind the same Frame selection interface.
 */
class TerminalTestSelection {
 public:
  /**
   * Replace logical lines and clear a selection invalidated by any change.
   *
   * Empty entries represent visual padding rather than selectable text and
   * are discarded before the new snapshot is compared with the old one.
   */
  void set_lines(std::vector<SelectableLine> lines);

  /** Apply one semantic frame-local selection event. */
  tui::Status update(const tui::SelectionEvent& event);

  /** Return inclusive spans used to render semantic highlight colors. */
  std::vector<HighlightSpan> highlight_spans() const;

  /** Extract selected logical bytes without visual padding or decorations. */
  tui::Status selected_text(std::string& output) const;

 private:
  /** Zero-based address of one logical character. */
  struct LogicalPosition {
    std::size_t line   = 0U; /**< Selectable-line index. */
    std::size_t column = 0U; /**< Character index in that line. */

    /** Compare positions in reading order. */
    constexpr bool operator<(const LogicalPosition& other) const noexcept {
      return line < other.line || (line == other.line && column < other.column);
    }
  };

  /** Inclusive range in reading order. */
  struct LogicalSelection {
    LogicalPosition first; /**< First selected character. */
    LogicalPosition last;  /**< Last selected character. */
  };

  /** Map a signed frame-local cell to the nearest real logical character. */
  std::optional<LogicalPosition> map_position(
      tui::SelectionPosition position) const;

  mutable std::shared_mutex mutex_;   /**< Synchronizes input and rendering. */
  std::vector<SelectableLine> lines_; /**< Current rendered logical lines. */
  std::optional<LogicalSelection> selection_; /**< Current inclusive range. */
};

}  // namespace puc::terminal
