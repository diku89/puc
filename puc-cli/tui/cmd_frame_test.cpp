/**
 * @file cmd_frame_test.cpp
 * @brief Focused tests for the composed command-mode view.
 */

#include "puc-cli/tui/cmd_frame.hpp"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"
#include "puc-cli/terminal/event.hpp"

namespace puc::tui {
namespace {

/** Return a palette with recognizable command colors. */
Theme command_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.text                 = 1U;
  colors.text_muted           = 2U;
  colors.text_success         = 3U;
  colors.secondary            = 4U;
  colors.highlight_text       = 5U;
  colors.highlight_background = 6U;
  theme.load_colors(colors);
  return theme;
}

/** Feed committed command text. */
void type(CmdFrame& frame, std::string text) {
  ASSERT_EQ(frame.handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = std::move(text)}}),
            Status::OK);
}

/** Draw and publish one command frame. */
void draw(CmdFrame& frame, Canvas& canvas, const Theme& theme) {
  const auto [width, height] = canvas.get_dimensions();
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(frame.draw(theme, canvas,
                       Canvas::Rect{
                           .x = 0U, .y = 0U, .width = width, .height = height}),
            Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);
}

/** Return one published cell by absolute coordinate. */
const Canvas::Cell& at(const Canvas& canvas, std::size_t x, std::size_t y) {
  const auto [width, height] = canvas.get_dimensions();
  static_cast<void>(height);
  return canvas.get_drawable_buffer()[y * width + x];
}

TEST(CmdFrameStateTest, OwnsAndClearsItsDisposableEditorBuffer) {
  CmdFrame frame;
  type(frame, "status\nverbose");
  EXPECT_EQ(frame.snapshot().text, "status\nverbose");
  EXPECT_EQ(frame.snapshot().cursor, (TextCursor{.line = 1U, .column = 7U}));
  frame.clear();
  EXPECT_TRUE(frame.snapshot().text.empty());
  EXPECT_EQ(frame.snapshot().cursor, TextCursor{});
}

TEST(CmdFrameRenderingTest, ComposesMutedNumbersWithGreenTextAndCaret) {
  const Theme theme = command_theme();
  Canvas canvas(12U, 2U);
  CmdFrame frame;
  type(frame, "x");
  draw(frame, canvas, theme);

  EXPECT_EQ(frame.gutter_width(), 3U);
  EXPECT_EQ(at(canvas, 1U, 0U).character, U'1');
  EXPECT_EQ(at(canvas, 1U, 0U).foreground_color, 2U);
  EXPECT_EQ(at(canvas, 3U, 0U).character, U'x');
  EXPECT_EQ(at(canvas, 3U, 0U).foreground_color, 3U);
  EXPECT_EQ(at(canvas, 4U, 0U).background_color, 3U);
}

TEST(CmdFrameGeometryTest, ReportsRowsAfterSubtractingItsAnnotationGutter) {
  CmdFrame frame;
  type(frame, "abcdef");
  EXPECT_EQ(frame.preferred_rows(7U), 2U);

  std::string lines;
  for (std::size_t line = 0U; line < 100U; ++line) {
    if (line != 0U) {
      lines.push_back('\n');
    }
    lines.push_back('x');
  }
  frame.clear();
  type(frame, std::move(lines));
  EXPECT_EQ(frame.gutter_width(), 4U);
}

TEST(CmdFrameSelectionTest, DelegatesSelectionAndCaretThroughLineNumbers) {
  const Theme theme = command_theme();
  Canvas canvas(12U, 2U);
  CmdFrame frame;
  type(frame, "hello");
  draw(frame, canvas, theme);

  ASSERT_EQ(frame.place_cursor({.x = 5, .y = 0}), Status::OK);
  EXPECT_EQ(frame.snapshot().cursor.column, 2U);
  ASSERT_EQ(frame.update_selection(
                SelectionEvent{.type = SelectionEventType::SELECT_ALL}),
            Status::OK);
  std::string selected;
  ASSERT_EQ(frame.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "hello");
  EXPECT_TRUE(frame.is_selectable());
  EXPECT_TRUE(frame.accepts_cursor_placement());
}

}  // namespace
}  // namespace puc::tui
