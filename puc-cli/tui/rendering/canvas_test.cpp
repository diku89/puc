/**
 * @file canvas_test.cpp
 * @brief Unit tests for Canvas construction, transactions, and validated
 * writes.
 */

#include "puc-cli/tui/rendering/canvas.hpp"

#include <limits>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Construct a distinctive cell with convenient default test colors. */
Canvas::Cell cell(char32_t character, uint32_t foreground = 1,
                  uint32_t background = 2) {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Adapt owned test rows to Canvas's nested-span write interface. */
Status write(Canvas& canvas, const Canvas::Rect& rect,
             std::vector<std::vector<Canvas::Cell>>& cells) {
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  const std::span<std::span<Canvas::Cell>> grid{rows};
  return canvas.write_cells(rect, grid);
}

/** Compare all display attributes of two Canvas cells. */
void expect_cell(const Canvas::Cell& actual, const Canvas::Cell& expected) {
  EXPECT_EQ(actual.character, expected.character);
  EXPECT_EQ(actual.foreground_color, expected.foreground_color);
  EXPECT_EQ(actual.background_color, expected.background_color);
}

TEST(CanvasTest, ConstructsBlankBuffersWithRequestedDimensions) {
  Canvas canvas(3, 2);
  const std::pair<size_t, size_t> expected_dimensions{3, 2};

  EXPECT_EQ(canvas.get_status(), Status::OK);
  EXPECT_EQ(canvas.get_dimensions(), expected_dimensions);
  ASSERT_EQ(canvas.get_drawable_buffer().size(), 6U);
  for (const Canvas::Cell& current : canvas.get_drawable_buffer()) {
    expect_cell(current, Canvas::Cell{});
  }
}

TEST(CanvasTest, RejectsDimensionsThatOverflowStorage) {
  Canvas canvas(std::numeric_limits<size_t>::max(), 2);
  const std::pair<size_t, size_t> expected_dimensions{0, 0};

  EXPECT_EQ(canvas.get_status(), Status::DIMENSION_OVERFLOW);
  EXPECT_EQ(canvas.get_dimensions(), expected_dimensions);
  EXPECT_TRUE(canvas.get_drawable_buffer().empty());
  EXPECT_EQ(canvas.begin_frame(), Status::DIMENSION_OVERFLOW);
}

TEST(CanvasTest, PublishesWritesOnlyWhenFrameEnds) {
  Canvas canvas(3, 2);
  std::vector<std::vector<Canvas::Cell>> rows{{cell(U'A'), cell(U'B')}};

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(write(canvas, Canvas::Rect{.x = 1, .y = 1, .width = 2, .height = 1},
                  rows),
            Status::OK);
  EXPECT_EQ(canvas.get_drawable_buffer()[4].character, U' ');
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  EXPECT_EQ(canvas.get_drawable_buffer()[4].character, U'A');
  EXPECT_EQ(canvas.get_drawable_buffer()[5].character, U'B');
}

TEST(CanvasTest, PreservesUnchangedCellsAcrossPartialFrames) {
  Canvas canvas(3, 1);
  std::vector<std::vector<Canvas::Cell>> first{
      {cell(U'A'), cell(U'B'), cell(U'C')}};
  std::vector<std::vector<Canvas::Cell>> second{{cell(U'X')}};

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 3, .height = 1},
                  first),
            Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(write(canvas, Canvas::Rect{.x = 1, .y = 0, .width = 1, .height = 1},
                  second),
            Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  const auto displayed = canvas.get_drawable_buffer();
  EXPECT_EQ(displayed[0].character, U'A');
  EXPECT_EQ(displayed[1].character, U'X');
  EXPECT_EQ(displayed[2].character, U'C');
}

TEST(CanvasTest, CancellationPreservesThePublishedImage) {
  Canvas canvas(2, 1);
  std::vector<std::vector<Canvas::Cell>> published{{cell(U'A'), cell(U'B')}};
  std::vector<std::vector<Canvas::Cell>> abandoned{{cell(U'X'), cell(U'Y')}};

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 2, .height = 1},
                  published),
            Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 2, .height = 1},
                  abandoned),
            Status::OK);
  ASSERT_EQ(canvas.cancel_frame(), Status::OK);

  EXPECT_EQ(canvas.get_drawable_buffer()[0].character, U'A');
  EXPECT_EQ(canvas.get_drawable_buffer()[1].character, U'B');
  EXPECT_EQ(canvas.cancel_frame(), Status::NO_FRAME_IN_PROGRESS);
}

TEST(CanvasTest, SupportsConcurrentWritesToDisjointRectangles) {
  Canvas canvas(2, 1);
  std::vector<std::vector<Canvas::Cell>> left{{cell(U'L')}};
  std::vector<std::vector<Canvas::Cell>> right{{cell(U'R')}};
  Status left_status  = Status::INVALID_ARGUMENT;
  Status right_status = Status::INVALID_ARGUMENT;

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  std::thread left_writer([&] {
    left_status = write(
        canvas, Canvas::Rect{.x = 0, .y = 0, .width = 1, .height = 1}, left);
  });
  std::thread right_writer([&] {
    right_status = write(
        canvas, Canvas::Rect{.x = 1, .y = 0, .width = 1, .height = 1}, right);
  });
  left_writer.join();
  right_writer.join();
  EXPECT_EQ(left_status, Status::OK);
  EXPECT_EQ(right_status, Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  EXPECT_EQ(canvas.get_drawable_buffer()[0].character, U'L');
  EXPECT_EQ(canvas.get_drawable_buffer()[1].character, U'R');
}

TEST(CanvasTest, ClearReplacesEveryWritableCell) {
  Canvas canvas(2, 2);
  const Canvas::Cell red = cell(U' ', 0xff0000, 0xff0000);

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(canvas.clear(red), Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  for (const Canvas::Cell& current : canvas.get_drawable_buffer()) {
    expect_cell(current, red);
  }
}

TEST(CanvasTest, EnforcesFrameTransaction) {
  Canvas canvas(1, 1);
  std::vector<std::vector<Canvas::Cell>> rows{{cell(U'X')}};

  EXPECT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 1, .height = 1},
                  rows),
            Status::NO_FRAME_IN_PROGRESS);
  EXPECT_EQ(canvas.clear(cell(U'X')), Status::NO_FRAME_IN_PROGRESS);
  EXPECT_EQ(canvas.end_frame(), Status::NO_FRAME_IN_PROGRESS);

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(canvas.begin_frame(), Status::FRAME_ALREADY_IN_PROGRESS);
  EXPECT_EQ(canvas.end_frame(), Status::OK);
}

TEST(CanvasTest, RejectsOutOfBoundsRectanglesWithoutChangingFrame) {
  Canvas canvas(2, 2);
  std::vector<std::vector<Canvas::Cell>> rows{{cell(U'X'), cell(U'Y')}};

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(write(canvas, Canvas::Rect{.x = 1, .y = 0, .width = 2, .height = 1},
                  rows),
            Status::RECT_OUT_OF_BOUNDS);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  for (const Canvas::Cell& current : canvas.get_drawable_buffer()) {
    EXPECT_EQ(current.character, U' ');
  }
}

TEST(CanvasTest, RejectsMismatchedRowsAndColumns) {
  Canvas canvas(2, 2);
  std::vector<std::vector<Canvas::Cell>> wrong_rows{{cell(U'X'), cell(U'Y')}};
  std::vector<std::vector<Canvas::Cell>> wrong_columns{{cell(U'X')},
                                                       {cell(U'Y')}};

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 2, .height = 2},
                  wrong_rows),
            Status::CELL_SHAPE_MISMATCH);
  EXPECT_EQ(write(canvas, Canvas::Rect{.x = 0, .y = 0, .width = 2, .height = 2},
                  wrong_columns),
            Status::CELL_SHAPE_MISMATCH);
  EXPECT_EQ(canvas.end_frame(), Status::OK);
}

TEST(CanvasTest, SupportsEmptyCanvasAndEmptyWrite) {
  Canvas canvas(0, 0);
  std::vector<std::vector<Canvas::Cell>> rows;

  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(write(canvas, Canvas::Rect{}, rows), Status::OK);
  EXPECT_EQ(canvas.end_frame(), Status::OK);
  EXPECT_TRUE(canvas.get_drawable_buffer().empty());
}

}  // namespace
}  // namespace puc::tui
