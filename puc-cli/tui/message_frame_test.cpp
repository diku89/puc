/**
 * @file message_frame_test.cpp
 * @brief Tests for centered semantic fallback messages.
 */

#include "puc-cli/tui/message_frame.hpp"

#include "gtest/gtest.h"
#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/theme.hpp"

namespace puc::tui {
namespace {

TEST(MessageFrameTest, CentersAndClipsMessageThroughCanvasApi) {
  Canvas canvas(5U, 3U);
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  Theme theme;
  Theme::Colors colors{};
  colors.text_warning = 0x123456U;
  colors.background   = 0x010203U;
  theme.load_colors(colors);
  MessageFrame frame("notice", "long message");
  ASSERT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 0U, .width = 5U, .height = 3U}),
      Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);
  const std::span<const Canvas::Cell> cells = canvas.get_drawable_buffer();
  EXPECT_EQ(cells[5U].character, U'l');
  EXPECT_EQ(cells[9U].character, U' ');
  EXPECT_EQ(cells[5U].foreground_color, 0x123456U);
}

}  // namespace
}  // namespace puc::tui
