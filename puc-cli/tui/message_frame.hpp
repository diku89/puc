#pragma once

/**
 * @file message_frame.hpp
 * @brief Centered single-line semantic message frame.
 */

#include <string>

#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/theme.hpp"

namespace puc::tui {

/**
 * Draw one clipped ASCII/UTF-8-byte message centered in its assigned rectangle.
 *
 * This lightweight fallback component is intended for minimum-size notices and
 * other one-line application states. The current implementation renders each
 * message byte as one terminal cell; callers should use ASCII text until a
 * display-width utility is introduced.
 */
class MessageFrame final : public Frame {
 public:
  /** Construct one immutable centered message and its semantic colors. */
  MessageFrame(std::string name, std::string message,
               Theme::ColorTypes foreground = Theme::ColorTypes::TEXT_WARNING,
               Theme::ColorTypes background = Theme::ColorTypes::BACKGROUND);

  /** Draw the clipped message on the vertical and horizontal center. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

 private:
  std::string message_; /**< Immutable byte text rendered one cell per byte. */
  Theme::ColorTypes foreground_; /**< Semantic foreground role. */
  Theme::ColorTypes background_; /**< Semantic background role. */
};

}  // namespace puc::tui
