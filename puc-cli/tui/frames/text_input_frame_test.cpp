/**
 * @file text_input_frame_test.cpp
 * @brief Focused tests for the renderable text-entry Frame.
 */

#include "puc-cli/tui/frames/text_input_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/tui/terminal/event.hpp"

namespace puc::tui {
namespace {

/** Return a palette with distinct text-input roles. */
Theme text_input_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.text                 = 1U;
  colors.text_secondary       = 2U;
  colors.text_success         = 3U;
  colors.secondary            = 4U;
  colors.highlight_text       = 5U;
  colors.highlight_background = 6U;
  theme.load_colors(colors);
  return theme;
}

/** Feed committed text into a text-input frame. */
void type(TextInputFrame& frame, std::string text) {
  ASSERT_EQ(frame.handle_event(
                terminal::Event{terminal::TextEvent{.utf8 = std::move(text)}}),
            Status::OK);
}

/** Feed one named key press into a text-input frame. */
void key(TextInputFrame& frame, terminal::NamedKey named,
         terminal::Modifiers modifiers = {}) {
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                .key       = named,
                .modifiers = modifiers,
            }}),
            Status::OK);
}

/** Draw a frame into an arbitrary Canvas rectangle and publish it. */
void draw(TextInputFrame& frame, Canvas& canvas, const Theme& theme,
          const Canvas::Rect& rect) {
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(frame.draw(theme, canvas, rect), Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);
}

/** Return one published cell by absolute coordinate. */
const Canvas::Cell& at(const Canvas& canvas, std::size_t x, std::size_t y) {
  const auto [width, height] = canvas.get_dimensions();
  static_cast<void>(height);
  return canvas.get_drawable_buffer()[y * width + x];
}

TEST(TextInputFrameEventTest, RoutesTypingEditingAndNavigationToTextEditor) {
  TextInputFrame frame;
  type(frame, "hello");
  key(frame, terminal::NamedKey::LEFT);
  key(frame, terminal::NamedKey::BACKSPACE);
  EXPECT_EQ(frame.snapshot().text, "helo");
  EXPECT_EQ(frame.snapshot().cursor.column, 3U);

  key(frame, terminal::NamedKey::ENTER);
  EXPECT_EQ(frame.snapshot().text, "helo");
  key(frame, terminal::NamedKey::ENTER,
      terminal::Modifiers{terminal::Modifier::SHIFT});
  EXPECT_EQ(frame.snapshot().text, "hel\no");

  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::CommandEvent{
                .command = terminal::Command::MOVE_BUFFER_START,
            }}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().cursor, TextCursor{});
}

TEST(TextInputFrameEventTest, UsesEnhancedAssociatedTextAndPasteTransactions) {
  TextInputFrame frame;
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::KeyEvent{
                .key         = U'a',
                .modifiers   = terminal::Modifier::SHIFT,
                .shifted_key = U'A',
                .text        = "\xc3\xa5",
            }}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().text, "\xc3\xa5");

  ASSERT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::BEGIN}}),
            Status::OK);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::PasteEvent{
                .phase = terminal::PastePhase::DATA, .data = " pasted"}}),
            Status::OK);
  EXPECT_TRUE(frame.snapshot().paste_in_progress);
  ASSERT_EQ(frame.handle_event(terminal::Event{
                terminal::PasteEvent{.phase = terminal::PastePhase::END}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().text, "\xc3\xa5 pasted");
}

TEST(TextInputFrameRenderingTest, DrawsConfiguredTextSelectionAndCaretColors) {
  const Theme theme = text_input_theme();
  Canvas canvas(8U, 2U);
  TextInputFrame frame;
  type(frame, "ab");
  draw(frame, canvas, theme,
       Canvas::Rect{.x = 0U, .y = 0U, .width = 8U, .height = 2U});
  EXPECT_EQ(at(canvas, 0U, 0U).character, U'a');
  EXPECT_EQ(at(canvas, 0U, 0U).foreground_color, 2U);
  EXPECT_EQ(at(canvas, 0U, 0U).background_color, 4U);
  EXPECT_EQ(at(canvas, 2U, 0U).background_color, 1U);

  ASSERT_EQ(frame.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_WORD,
                .extent = {.x = 0, .y = 0},
            }),
            Status::OK);
  draw(frame, canvas, theme,
       Canvas::Rect{.x = 0U, .y = 0U, .width = 8U, .height = 2U});
  EXPECT_EQ(at(canvas, 0U, 0U).foreground_color, 5U);
  EXPECT_EQ(at(canvas, 0U, 0U).background_color, 6U);
}

TEST(TextInputFrameRenderingTest, SupportsIndependentGreenEditorStyling) {
  const Theme theme = text_input_theme();
  Canvas canvas(8U, 2U);
  TextInputFrame frame("green",
                       TextInputFrameStyle{
                           .text_color   = Theme::ColorTypes::TEXT_SUCCESS,
                           .cursor_color = Theme::ColorTypes::TEXT_SUCCESS,
                       });
  type(frame, "x");
  draw(frame, canvas, theme,
       Canvas::Rect{.x = 0U, .y = 0U, .width = 8U, .height = 2U});
  EXPECT_EQ(at(canvas, 0U, 0U).foreground_color, 3U);
  EXPECT_EQ(at(canvas, 1U, 0U).background_color, 3U);
}

TEST(TextInputFrameScrollTest,
     HitTestsVerticalScrollAndIgnoresHorizontalInput) {
  const Theme theme = text_input_theme();
  Canvas canvas(12U, 8U);
  TextInputFrame frame;
  type(frame, "0\n1\n2\n3\n4");
  const Canvas::Rect rect{.x = 2U, .y = 3U, .width = 8U, .height = 2U};
  draw(frame, canvas, theme, rect);
  ASSERT_EQ(frame.snapshot().scroll_row, 3U);

  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::ScrollEvent{
                .position = {.x = 0U, .y = 0U}, .delta_y = 1}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().scroll_row, 3U);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::ScrollEvent{
                .position = {.x = 3U, .y = 3U}, .delta_x = 10}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().scroll_row, 3U);
  ASSERT_EQ(frame.handle_event(terminal::Event{terminal::ScrollEvent{
                .position = {.x = 3U, .y = 3U}, .delta_y = 2}}),
            Status::OK);
  EXPECT_EQ(frame.snapshot().scroll_row, 1U);
}

TEST(TextInputFrameAnnotationTest, ExposesLogicalIdentityOfVisibleWrappedRows) {
  const Theme theme = text_input_theme();
  Canvas canvas(4U, 2U);
  TextInputFrame frame;
  type(frame, "abcdef");
  draw(frame, canvas, theme,
       Canvas::Rect{.x = 0U, .y = 0U, .width = 4U, .height = 2U});

  const std::vector<AnnotatedTextRow> rows = frame.visible_text_rows();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].logical_line, 0U);
  EXPECT_TRUE(rows[0].first_visual_row);
  EXPECT_EQ(rows[1].logical_line, 0U);
  EXPECT_FALSE(rows[1].first_visual_row);
}

TEST(TextInputFrameSelectionTest, SelectsAndPlacesCaretInLocalCoordinates) {
  const Theme theme = text_input_theme();
  Canvas canvas(10U, 2U);
  TextInputFrame frame;
  type(frame, "hello");
  draw(frame, canvas, theme,
       Canvas::Rect{.x = 0U, .y = 0U, .width = 10U, .height = 2U});

  ASSERT_EQ(frame.place_cursor({.x = 2, .y = 0}), Status::OK);
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
