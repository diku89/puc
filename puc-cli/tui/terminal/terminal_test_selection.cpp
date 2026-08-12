/**
 * @file terminal_test_selection.cpp
 * @brief Logical selection implementation for the terminal conformance app.
 */

#include "puc-cli/tui/terminal/terminal_test_selection.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace puc::terminal {

void TerminalTestSelection::set_lines(std::vector<SelectableLine> lines) {
  std::erase_if(lines,
                [](const SelectableLine& line) { return line.text.empty(); });
  const std::unique_lock lock(mutex_);
  if (lines_ != lines) {
    lines_ = std::move(lines);
    selection_.reset();
  }
}

tui::Status TerminalTestSelection::update(const tui::SelectionEvent& event) {
  const std::unique_lock lock(mutex_);
  if (event.type == tui::SelectionEventType::RESET) {
    selection_.reset();
    return tui::Status::OK;
  }
  if (event.type == tui::SelectionEventType::SELECT_ALL) {
    if (lines_.empty()) {
      return tui::Status::NO_SELECTION;
    }
    selection_ = LogicalSelection{
        .first = {},
        .last  = {.line   = lines_.size() - 1U,
                  .column = lines_.back().text.size() - 1U},
    };
    return tui::Status::OK;
  }

  const std::optional<LogicalPosition> extent = map_position(event.extent);
  if (!extent.has_value()) {
    return tui::Status::NO_SELECTION;
  }
  if (event.type == tui::SelectionEventType::SELECT_WORD) {
    const std::string& text = lines_[extent->line].text;
    std::size_t first       = extent->column;
    std::size_t last        = extent->column;
    const auto is_word      = [](unsigned char byte) {
      return std::isalnum(byte) != 0 || byte == '_' || byte == '-';
    };
    if (is_word(static_cast<unsigned char>(text[extent->column]))) {
      while (first > 0U &&
             is_word(static_cast<unsigned char>(text[first - 1U]))) {
        --first;
      }
      while (last + 1U < text.size() &&
             is_word(static_cast<unsigned char>(text[last + 1U]))) {
        ++last;
      }
    }
    selection_ = LogicalSelection{
        .first = {.line = extent->line, .column = first},
        .last  = {.line = extent->line, .column = last},
    };
    return tui::Status::OK;
  }
  if (event.type == tui::SelectionEventType::SELECT_LINE) {
    selection_ = LogicalSelection{
        .first = {.line = extent->line, .column = 0U},
        .last  = {.line   = extent->line,
                  .column = lines_[extent->line].text.size() - 1U},
    };
    return tui::Status::OK;
  }

  const std::optional<LogicalPosition> anchor = map_position(event.anchor);
  if (!anchor.has_value()) {
    return tui::Status::NO_SELECTION;
  }
  selection_ = *extent < *anchor
                   ? LogicalSelection{.first = *extent, .last = *anchor}
                   : LogicalSelection{.first = *anchor, .last = *extent};
  return tui::Status::OK;
}

std::vector<HighlightSpan> TerminalTestSelection::highlight_spans() const {
  const std::shared_lock lock(mutex_);
  std::vector<HighlightSpan> spans;
  if (!selection_.has_value()) {
    return spans;
  }
  spans.reserve(selection_->last.line - selection_->first.line + 1U);
  for (std::size_t line = selection_->first.line; line <= selection_->last.line;
       ++line) {
    spans.push_back(HighlightSpan{
        .line  = line,
        .first = line == selection_->first.line ? selection_->first.column : 0U,
        .last  = line == selection_->last.line ? selection_->last.column
                                               : lines_[line].text.size() - 1U,
    });
  }
  return spans;
}

tui::Status TerminalTestSelection::selected_text(std::string& output) const {
  const std::shared_lock lock(mutex_);
  output.clear();
  if (!selection_.has_value()) {
    return tui::Status::NO_SELECTION;
  }
  for (std::size_t line = selection_->first.line; line <= selection_->last.line;
       ++line) {
    const std::size_t first =
        line == selection_->first.line ? selection_->first.column : 0U;
    const std::size_t last = line == selection_->last.line
                                 ? selection_->last.column
                                 : lines_[line].text.size() - 1U;
    output.append(lines_[line].text.substr(first, last - first + 1U));
    if (line != selection_->last.line) {
      output.push_back('\n');
    }
  }
  return tui::Status::OK;
}

std::optional<TerminalTestSelection::LogicalPosition>
TerminalTestSelection::map_position(tui::SelectionPosition position) const {
  if (lines_.empty()) {
    return std::nullopt;
  }
  std::size_t line = 0U;
  if (position.y <= lines_.front().y) {
    line = 0U;
  } else if (position.y >= lines_.back().y) {
    line = lines_.size() - 1U;
  } else {
    for (std::size_t index = 0U; index < lines_.size(); ++index) {
      if (position.y == lines_[index].y) {
        line = index;
        break;
      }
      if (position.y < lines_[index].y) {
        const std::int64_t previous_distance =
            position.y - lines_[index - 1U].y;
        const std::int64_t next_distance = lines_[index].y - position.y;
        line = previous_distance <= next_distance ? index - 1U : index;
        break;
      }
    }
  }

  const SelectableLine& selected_line = lines_[line];
  if (selected_line.text.empty()) {
    return std::nullopt;
  }
  std::size_t column = 0U;
  if (position.x > selected_line.x) {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(position.x - selected_line.x);
    column = static_cast<std::size_t>(
        std::min<std::uint64_t>(offset, selected_line.text.size() - 1U));
  }
  return LogicalPosition{.line = line, .column = column};
}

}  // namespace puc::terminal
