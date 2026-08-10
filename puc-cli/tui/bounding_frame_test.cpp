/**
 * @file bounding_frame_test.cpp
 * @brief Focused tests for colored bounds, margins, and delegation.
 */

#include "puc-cli/tui/bounding_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Child Frame that records geometry and translated interaction coordinates. */
class RecordingFrame final : public Frame {
 public:
  RecordingFrame() : Frame("recording") {}

  Status draw(const Theme&, Canvas& canvas, const Canvas::Rect& rect) override {
    last_rect = rect;
    std::vector<std::vector<Canvas::Cell>> cells(
        rect.height, std::vector<Canvas::Cell>(
                         rect.width, Canvas::Cell{.character        = U'X',
                                                  .foreground_color = 41U,
                                                  .background_color = 42U}));
    std::vector<std::span<Canvas::Cell>> rows;
    for (auto& row : cells) {
      rows.emplace_back(row);
    }
    return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
  }

  bool is_selectable() const noexcept override { return true; }

  Status update_selection(const SelectionEvent& event) override {
    last_selection = event;
    return Status::OK;
  }

  Status selected_text(std::string& output) const override {
    output = "child selection";
    return Status::OK;
  }

  bool accepts_cursor_placement() const noexcept override { return true; }

  Status place_cursor(SelectionPosition position) override {
    last_cursor = position;
    return Status::OK;
  }

  Canvas::Rect last_rect{};
  SelectionEvent last_selection{};
  SelectionPosition last_cursor{};
};

/** Return a palette with recognizable bounding-frame roles. */
Theme bounding_theme() {
  Theme theme;
  Theme::Colors colors{};
  colors.text       = 1U;
  colors.secondary  = 2U;
  colors.tertiary   = 3U;
  colors.background = 4U;
  theme.load_colors(colors);
  return theme;
}

/** Return one published cell by absolute coordinate. */
const Canvas::Cell& at(const Canvas& canvas, std::size_t x, std::size_t y) {
  const auto [width, height] = canvas.get_dimensions();
  static_cast<void>(height);
  return canvas.get_drawable_buffer()[y * width + x];
}

TEST(BoundingFrameGeometryTest, ComputesOuterBoxBorderAndInnerInsets) {
  auto child = std::make_shared<RecordingFrame>();
  BoundingFrame frame(
      "bounds", child,
      BoundingFrameConfiguration{
          .outer_margins = {.top = 1U, .bottom = 2U, .left = 3U, .right = 4U},
          .inner_margins = {.top = 1U, .bottom = 2U, .left = 2U, .right = 1U},
      });
  const Canvas::Rect assigned{.x = 10U, .y = 20U, .width = 30U, .height = 20U};
  EXPECT_EQ(frame.box_rect(assigned),
            (Canvas::Rect{.x = 13U, .y = 21U, .width = 23U, .height = 17U}));
  EXPECT_EQ(frame.content_rect(assigned),
            (Canvas::Rect{.x = 16U, .y = 23U, .width = 18U, .height = 12U}));
}

TEST(BoundingFrameGeometryTest, RejectsInsetOverflowAndMissingChildren) {
  BoundingFrame overflow(
      "overflow", std::make_shared<RecordingFrame>(),
      BoundingFrameConfiguration{.outer_margins = {.left = 1U}});
  EXPECT_FALSE(overflow.content_rect(Canvas::Rect{
      .x      = std::numeric_limits<std::size_t>::max(),
      .y      = 0U,
      .width  = 4U,
      .height = 4U,
  }));

  const Theme theme = bounding_theme();
  Canvas canvas(8U, 6U);
  BoundingFrame missing("missing", std::shared_ptr<Frame>{});
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(
      missing.draw(theme, canvas,
                   Canvas::Rect{.x = 0U, .y = 0U, .width = 8U, .height = 6U}),
      Status::INVALID_ARGUMENT);
  EXPECT_EQ(canvas.cancel_frame(), Status::OK);
}

TEST(BoundingFrameRenderingTest, PaintsOutsideBorderPaddingAndChildSeparately) {
  const Theme theme = bounding_theme();
  Canvas canvas(12U, 8U);
  auto child = std::make_shared<RecordingFrame>();
  BoundingFrame frame(
      "bounds", child,
      BoundingFrameConfiguration{
          .outer_margins = {.top = 1U, .bottom = 1U, .left = 1U, .right = 1U},
          .inner_margins = {.top = 1U, .bottom = 1U, .left = 1U, .right = 1U},
          .outside_box_color = Theme::ColorTypes::BACKGROUND,
          .inside_color      = Theme::ColorTypes::SECONDARY,
          .border_color      = Theme::ColorTypes::TERTIARY,
      });

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 0U, .width = 12U, .height = 8U}),
      Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  EXPECT_EQ(at(canvas, 0U, 0U).background_color, 4U);
  EXPECT_EQ(at(canvas, 1U, 1U).character, U'┌');
  EXPECT_EQ(at(canvas, 1U, 1U).foreground_color, 3U);
  EXPECT_EQ(at(canvas, 2U, 2U).background_color, 2U);
  EXPECT_EQ(at(canvas, 3U, 3U).character, U'X');
  EXPECT_EQ(child->last_rect,
            (Canvas::Rect{.x = 3U, .y = 3U, .width = 6U, .height = 2U}));
}

TEST(BoundingFrameRenderingTest, OptionalOutsideColorPreservesMarginCells) {
  const Theme theme = bounding_theme();
  Canvas canvas(8U, 6U);
  auto child = std::make_shared<RecordingFrame>();
  BoundingFrame frame(
      "bounds", child,
      BoundingFrameConfiguration{
          .outer_margins = {.top = 1U, .bottom = 1U, .left = 1U, .right = 1U},
          .outside_box_color = std::nullopt,
      });

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(
      canvas.clear(Canvas::Cell{
          .character = U'?', .foreground_color = 98U, .background_color = 99U}),
      Status::OK);
  ASSERT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 0U, .width = 8U, .height = 6U}),
      Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);
  EXPECT_EQ(at(canvas, 0U, 0U).character, U'?');
  EXPECT_EQ(at(canvas, 0U, 0U).background_color, 99U);
  EXPECT_EQ(at(canvas, 1U, 1U).character, U'┌');
}

TEST(BoundingFrameConstraintTest, EnforcesMinMaxFullWidthAndCanvasBounds) {
  const Theme theme = bounding_theme();
  Canvas canvas(20U, 10U);
  auto child = std::make_shared<RecordingFrame>();
  BoundingFrame frame("bounds", child,
                      BoundingFrameConfiguration{
                          .size_constraints =
                              FrameSizeConstraints{
                                  .minimum_width             = 10U,
                                  .minimum_height            = 5U,
                                  .maximum_width             = 20U,
                                  .maximum_height            = 8U,
                                  .require_full_canvas_width = true,
                              },
                      });
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 5U, .width = 20U, .height = 4U}),
      Status::INVALID_DIMENSIONS);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 1U, .y = 2U, .width = 19U, .height = 5U}),
      Status::INVALID_DIMENSIONS);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 3U, .width = 20U, .height = 8U}),
      Status::RECT_OUT_OF_BOUNDS);
  EXPECT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 2U, .width = 20U, .height = 8U}),
      Status::OK);
  EXPECT_EQ(canvas.cancel_frame(), Status::OK);
}

TEST(BoundingFrameDelegationTest, TranslatesSelectionAndCaretCoordinates) {
  const Theme theme = bounding_theme();
  Canvas canvas(12U, 8U);
  auto child = std::make_shared<RecordingFrame>();
  BoundingFrame frame("bounds", child,
                      BoundingFrameConfiguration{
                          .outer_margins = {.top = 1U, .left = 2U},
                          .inner_margins = {.top = 1U, .left = 1U},
                      });
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(
      frame.draw(theme, canvas,
                 Canvas::Rect{.x = 0U, .y = 0U, .width = 12U, .height = 8U}),
      Status::OK);
  ASSERT_EQ(frame.update_selection(SelectionEvent{
                .type   = SelectionEventType::SELECT_AND_EXTEND,
                .anchor = {.x = 5, .y = 4},
                .extent = {.x = 7, .y = 5},
            }),
            Status::OK);
  EXPECT_EQ(child->last_selection.anchor, (SelectionPosition{.x = 1, .y = 1}));
  EXPECT_EQ(child->last_selection.extent, (SelectionPosition{.x = 3, .y = 2}));
  ASSERT_EQ(frame.place_cursor({.x = 8, .y = 6}), Status::OK);
  EXPECT_EQ(child->last_cursor, (SelectionPosition{.x = 4, .y = 3}));
  EXPECT_TRUE(frame.is_selectable());
  EXPECT_TRUE(frame.accepts_cursor_placement());
  std::string selected;
  EXPECT_EQ(frame.selected_text(selected), Status::OK);
  EXPECT_EQ(selected, "child selection");
  EXPECT_EQ(canvas.cancel_frame(), Status::OK);
}

}  // namespace
}  // namespace puc::tui
