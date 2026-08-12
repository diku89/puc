/**
 * @file frame.cpp
 * @brief Default behavior for nonselectable terminal UI Frames.
 */

#include "puc-cli/tui/rendering/frame.hpp"

#include <string>

namespace puc::tui {

bool Frame::is_selectable() const noexcept { return false; }

Status Frame::update_selection(const SelectionEvent&) {
  return Status::FRAME_NOT_SELECTABLE;
}

Status Frame::selected_text(std::string& output) const {
  output.clear();
  return Status::FRAME_NOT_SELECTABLE;
}

bool Frame::accepts_cursor_placement() const noexcept { return false; }

Status Frame::place_cursor(SelectionPosition) {
  return Status::FRAME_NOT_SELECTABLE;
}

}  // namespace puc::tui
