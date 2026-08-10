/**
 * @file integrated_term_frame_test.cpp
 * @brief Focused tests for libtmt rendering and lifecycle state.
 */

#include "puc-cli/tui/integrated_term_frame.hpp"

#include <cstddef>
#include <string>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Return a palette whose values expose terminal ANSI mappings. */
Theme terminal_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.primary        = 10U;
  colors.secondary      = 3U;
  colors.tertiary       = 9U;
  colors.text           = 1U;
  colors.text_secondary = 4U;
  colors.text_muted     = 5U;
  colors.text_error     = 11U;
  colors.text_warning   = 12U;
  colors.text_success   = 6U;
  colors.text_info      = 13U;
  colors.text_emphasis  = 14U;
  colors.background     = 2U;
  theme.load_colors(colors);
  return theme;
}

/** Draw and publish one exact terminal surface. */
void draw(IntegratedTermFrame& frame, Canvas& canvas, const Theme& theme) {
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

TEST(IntegratedTermFrameRenderingTest,
     ReplaysQueuedOutputAndMapsAnsiColorsAndCursor) {
  const Theme theme = terminal_theme();
  Canvas canvas(10U, 3U);
  IntegratedTermFrame frame;
  ASSERT_EQ(frame.write("\x1b[31mR\x1b[0m"), Status::OK);
  EXPECT_FALSE(frame.snapshot().session_active);
  frame.activate_session();
  draw(frame, canvas, theme);

  EXPECT_EQ(at(canvas, 0U, 0U).character, U'R');
  EXPECT_EQ(at(canvas, 0U, 0U).foreground_color, 11U);
  EXPECT_EQ(at(canvas, 1U, 0U).background_color, 9U);
  const IntegratedTermFrameSnapshot state = frame.snapshot();
  EXPECT_EQ(state.rows, 3U);
  EXPECT_EQ(state.columns, 10U);
  EXPECT_TRUE(state.session_active);
  EXPECT_EQ(state.generation, 1U);
}

TEST(IntegratedTermFrameRenderingTest, ResizesWithoutDiscardingScreenContents) {
  const Theme theme = terminal_theme();
  IntegratedTermFrame frame;
  frame.activate_session();
  ASSERT_EQ(frame.write("kept"), Status::OK);
  Canvas first(8U, 3U);
  draw(frame, first, theme);
  EXPECT_EQ(frame.snapshot().columns, 8U);

  Canvas second(12U, 4U);
  draw(frame, second, theme);
  EXPECT_EQ(frame.snapshot().rows, 4U);
  EXPECT_EQ(frame.snapshot().columns, 12U);
  EXPECT_EQ(at(second, 0U, 0U).character, U'k');
  EXPECT_EQ(at(second, 3U, 0U).character, U't');
}

TEST(IntegratedTermFrameProtocolTest, ExposesAndConsumesTerminalReplies) {
  const Theme theme = terminal_theme();
  Canvas canvas(10U, 3U);
  IntegratedTermFrame frame;
  frame.activate_session();
  ASSERT_EQ(frame.write("\x1b[6n"), Status::OK);
  draw(frame, canvas, theme);
  EXPECT_EQ(frame.take_responses(), "\x1b[1;1R");
  EXPECT_TRUE(frame.take_responses().empty());
}

TEST(IntegratedTermFrameLifecycleTest,
     ResetRetainsSessionWhileStartNewAdvancesGeneration) {
  const Theme theme = terminal_theme();
  Canvas canvas(10U, 3U);
  IntegratedTermFrame frame;
  frame.activate_session();
  const std::size_t first = frame.snapshot().generation;
  ASSERT_EQ(frame.write("old"), Status::OK);
  draw(frame, canvas, theme);

  frame.reset();
  EXPECT_TRUE(frame.snapshot().session_active);
  EXPECT_EQ(frame.snapshot().generation, first);
  draw(frame, canvas, theme);
  EXPECT_EQ(at(canvas, 0U, 0U).character, U' ');

  frame.start_new_session();
  EXPECT_TRUE(frame.snapshot().session_active);
  EXPECT_GT(frame.snapshot().generation, first);
  EXPECT_EQ(frame.snapshot().rows, 0U);
  EXPECT_EQ(frame.snapshot().columns, 0U);
}

TEST(IntegratedTermFrameLifecycleTest,
     CloseDestroysDisplayAndDeactivatesOwner) {
  const Theme theme = terminal_theme();
  Canvas canvas(10U, 3U);
  IntegratedTermFrame frame;
  frame.activate_session();
  draw(frame, canvas, theme);
  ASSERT_GT(frame.snapshot().rows, 0U);
  frame.close_session();

  const IntegratedTermFrameSnapshot state = frame.snapshot();
  EXPECT_FALSE(state.session_active);
  EXPECT_EQ(state.rows, 0U);
  EXPECT_EQ(state.columns, 0U);
  EXPECT_TRUE(state.cursor_visible);
}

TEST(IntegratedTermFrameContractTest, RejectsTinySurfacesAndSelection) {
  const Theme theme = terminal_theme();
  Canvas canvas(2U, 2U);
  IntegratedTermFrame frame;
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 0U, .width = 1U, .height = 2U}),
      Status::INVALID_DIMENSIONS);
  EXPECT_EQ(canvas.cancel_frame(), Status::OK);

  EXPECT_FALSE(frame.is_selectable());
  EXPECT_FALSE(frame.accepts_cursor_placement());
  std::string selected = "stale";
  EXPECT_EQ(frame.selected_text(selected), Status::FRAME_NOT_SELECTABLE);
  EXPECT_TRUE(selected.empty());
  EXPECT_EQ(frame.update_selection(
                SelectionEvent{.type = SelectionEventType::SELECT_ALL}),
            Status::FRAME_NOT_SELECTABLE);
}

}  // namespace
}  // namespace puc::tui
