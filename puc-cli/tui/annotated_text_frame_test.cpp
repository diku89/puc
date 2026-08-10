/**
 * @file annotated_text_frame_test.cpp
 * @brief Focused tests for line numbers, status markers, and delegation.
 */

#include "puc-cli/tui/annotated_text_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/text_input_frame.hpp"

namespace puc::tui {
namespace {

/** Return a palette whose values expose annotation roles. */
Theme annotation_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.text                 = 1U;
  colors.text_secondary       = 2U;
  colors.text_muted           = 3U;
  colors.text_success         = 4U;
  colors.text_error           = 5U;
  colors.text_warning         = 6U;
  colors.secondary            = 7U;
  colors.highlight_text       = 8U;
  colors.highlight_background = 9U;
  theme.load_colors(colors);
  return theme;
}

/** Insert committed text into a TextInputFrame. */
void type(TextInputFrame& input, std::string text) {
  ASSERT_EQ(input.handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = std::move(text)}}),
            Status::OK);
}

/** Draw one annotation frame and publish its Canvas. */
void draw(AnnotatedTextFrame& frame, Canvas& canvas, const Theme& theme) {
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

TEST(AnnotatedTextFrameNumberTest, LeavesPristineRowsUnnumbered) {
  const Theme theme = annotation_theme();
  Canvas canvas(12U, 3U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);

  EXPECT_EQ(frame.gutter_width(), 3U);
  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 1U, 0U).character, U' ');
  EXPECT_EQ(at(canvas, 2U, 0U).character, U' ');
  EXPECT_EQ(at(canvas, 3U, 0U).background_color, 1U);
}

TEST(AnnotatedTextFrameGeometryTest, RejectsGuttersThatCannotFitOrOverflowX) {
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);
  EXPECT_FALSE(frame.content_rect(
      Canvas::Rect{.x = 0U, .y = 0U, .width = 3U, .height = 1U}));
  EXPECT_FALSE(frame.content_rect(Canvas::Rect{
      .x      = std::numeric_limits<std::size_t>::max() - 1U,
      .y      = 0U,
      .width  = 10U,
      .height = 1U,
  }));
}

TEST(AnnotatedTextFrameNumberTest, NumbersNewlineOnlyRowsButNotUnusedRows) {
  const Theme theme = annotation_theme();
  Canvas canvas(12U, 3U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);
  type(*input, "\n");

  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 1U, 0U).character, U'1');
  EXPECT_EQ(at(canvas, 1U, 1U).character, U'2');
  EXPECT_EQ(at(canvas, 1U, 2U).character, U' ');
  EXPECT_EQ(at(canvas, 1U, 0U).foreground_color, 3U);
}

TEST(AnnotatedTextFrameNumberTest, OmitsNumbersOnWrappedContinuationRows) {
  const Theme theme = annotation_theme();
  Canvas canvas(8U, 2U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);
  type(*input, "abcdefgh");

  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 1U, 0U).character, U'1');
  EXPECT_EQ(at(canvas, 1U, 1U).character, U' ');
  EXPECT_EQ(at(canvas, 3U, 0U).character, U'a');
  EXPECT_EQ(at(canvas, 3U, 1U).character, U'f');
}

TEST(AnnotatedTextFrameNumberTest, ExpandsForTheHundredthLogicalLine) {
  const Theme theme = annotation_theme();
  Canvas canvas(12U, 3U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);
  std::string lines;
  for (std::size_t line = 0U; line < 100U; ++line) {
    if (line != 0U) {
      lines.push_back('\n');
    }
    lines.push_back('x');
  }
  type(*input, std::move(lines));

  EXPECT_EQ(frame.gutter_width(), 4U);
  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 0U, 2U).character, U'1');
  EXPECT_EQ(at(canvas, 1U, 2U).character, U'0');
  EXPECT_EQ(at(canvas, 2U, 2U).character, U'0');
  EXPECT_EQ(at(canvas, 3U, 2U).character, U' ');
  EXPECT_EQ(at(canvas, 4U, 2U).character, U'x');
}

TEST(AnnotatedTextFrameStatusTest, RendersDiffAndAlertMarkersByLogicalLine) {
  const Theme theme = annotation_theme();
  Canvas canvas(14U, 3U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame(
      "annotated", input,
      AnnotatedTextConfiguration{
          .show_line_numbers           = true,
          .minimum_line_number_columns = 2U,
          .status_columns              = 2U,
          .separator_columns           = 1U,
          .background_color            = Theme::ColorTypes::SECONDARY,
          .line_number_color           = Theme::ColorTypes::TEXT_MUTED,
      });
  type(*input, "added\nremoved\nalert");
  frame.set_statuses({
      {.logical_line = 0U,
       .text         = U"++",
       .color        = Theme::ColorTypes::TEXT_SUCCESS},
      {.logical_line = 1U,
       .text         = U"--",
       .color        = Theme::ColorTypes::TEXT_ERROR},
      {.logical_line = 2U,
       .text         = U"⚠️",
       .color        = Theme::ColorTypes::TEXT_WARNING},
  });

  EXPECT_EQ(frame.gutter_width(), 5U);
  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 0U, 0U).character, U'+');
  EXPECT_EQ(at(canvas, 1U, 0U).character, U'+');
  EXPECT_EQ(at(canvas, 0U, 0U).foreground_color, 4U);
  EXPECT_EQ(at(canvas, 0U, 1U).character, U'-');
  EXPECT_EQ(at(canvas, 0U, 1U).foreground_color, 5U);
  EXPECT_EQ(at(canvas, 0U, 2U).character, U'⚠');
  EXPECT_EQ(at(canvas, 0U, 2U).foreground_color, 6U);
  EXPECT_EQ(at(canvas, 3U, 2U).character, U'3');
  EXPECT_EQ(at(canvas, 4U, 2U).character, U' ');
  EXPECT_EQ(at(canvas, 5U, 2U).character, U'a');
}

TEST(AnnotatedTextFrameStatusTest, SupportsStatusOnlyAndMinimumGutters) {
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input,
                           AnnotatedTextConfiguration{
                               .show_line_numbers           = false,
                               .minimum_line_number_columns = 0U,
                               .status_columns              = 2U,
                               .separator_columns           = 1U,
                               .minimum_gutter_width        = 6U,
                           });
  EXPECT_EQ(frame.gutter_width(), 6U);
  frame.clear_statuses();
  EXPECT_EQ(frame.gutter_width(), 6U);
}

TEST(AnnotatedTextFrameDelegationTest,
     TranslatesSelectionsAndRejectsCaretClicksInTheGutter) {
  const Theme theme = annotation_theme();
  Canvas canvas(12U, 2U);
  auto input = std::make_shared<TextInputFrame>();
  AnnotatedTextFrame frame("annotated", input);
  type(*input, "hello");
  draw(frame, canvas, theme);

  ASSERT_EQ(frame.place_cursor({.x = 1, .y = 0}), Status::OK);
  EXPECT_EQ(input->snapshot().cursor.column, 5U);
  ASSERT_EQ(frame.place_cursor({.x = 5, .y = 0}), Status::OK);
  EXPECT_EQ(input->snapshot().cursor.column, 2U);

  ASSERT_EQ(frame.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_AND_EXTEND,
                .anchor = {.x = 3, .y = 0},
                .extent = {.x = 5, .y = 0},
            }),
            Status::OK);
  std::string selected;
  ASSERT_EQ(frame.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "hel");
}

}  // namespace
}  // namespace puc::tui
