/**
 * @file message_frame.cpp
 * @brief Centered semantic message frame implementation.
 */

#include "puc-cli/tui/frames/message_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace puc::tui {

MessageFrame::MessageFrame(std::string name, std::string message,
                           Theme::ColorTypes foreground,
                           Theme::ColorTypes background)
    : Frame(std::move(name)),
      message_(std::move(message)),
      foreground_(foreground),
      background_(background) {}

Status MessageFrame::draw(const Theme& theme, Canvas& canvas,
                          const Canvas::Rect& rect) {
  if (rect.width == 0U || rect.height == 0U) {
    return Status::OK;
  }
  const std::size_t count        = std::min(rect.width, message_.size());
  const std::uint32_t foreground = theme.get_color(foreground_);
  const std::uint32_t background = theme.get_color(background_);
  std::vector<std::vector<Canvas::Cell>> cells(
      1U, std::vector<Canvas::Cell>(
              count, Canvas::Cell{.character        = U' ',
                                  .foreground_color = foreground,
                                  .background_color = background}));
  for (std::size_t index = 0U; index < count; ++index) {
    cells.front()[index].character =
        static_cast<unsigned char>(message_[index]);
  }
  return canvas.write_cells(
      Canvas::Rect{.x      = rect.x + (rect.width - count) / 2U,
                   .y      = rect.y + rect.height / 2U,
                   .width  = count,
                   .height = 1U},
      cells);
}

}  // namespace puc::tui
