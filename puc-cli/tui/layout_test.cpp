/**
 * @file layout_test.cpp
 * @brief Unit tests for layout validation, solving, minimums, and composition.
 */

#include "puc-cli/tui/layout.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Square cells isolate raw constraint geometry in most solver tests. */
constexpr CellDimensions kSquareCells{.width = 1, .height = 1};
/** Typical terminal cells verify visual aspect-ratio compensation. */
constexpr CellDimensions kTypicalCells{.width = 1, .height = 2};

/** One recorded Frame::draw invocation. */
struct DrawCall {
  std::string frame_id; /**< Id of the frame that was drawn. */
  Canvas::Rect rect;    /**< Rectangle supplied by Layout. */
};

/** Configurable frame double that records calls and can inject a draw error. */
class TestFrame final : public Frame {
 public:
  /** Construct a frame double with configurable update and result behavior. */
  TestFrame(std::string name, std::vector<DrawCall>* calls = nullptr,
            bool needs_update = true, Status draw_status = Status::OK)
      : Frame(std::move(name)),
        calls_(calls),
        needs_update_(needs_update),
        draw_status_(draw_status) {}

  Status draw(const State&, const Theme&, Canvas&,
              const Canvas::Rect& rect) override {
    if (calls_ != nullptr) {
      calls_->push_back(DrawCall{.frame_id = name_, .rect = rect});
    }
    return draw_status_;
  }

  bool needs_update() const override { return needs_update_; }

 private:
  /** Optional destination for observed draw calls. */
  std::vector<DrawCall>* calls_;
  /** Value returned by needs_update(). */
  bool needs_update_;
  /** Value returned by draw(). */
  Status draw_status_;
};

/** Allocate a TestFrame through Layout's public Frame ownership type. */
std::shared_ptr<Frame> frame(std::string name,
                             std::vector<DrawCall>* calls = nullptr,
                             bool needs_update            = true,
                             Status draw_status           = Status::OK) {
  return std::make_shared<TestFrame>(std::move(name), calls, needs_update,
                                     draw_status);
}

/** Add a constraint and fail the current test immediately if rejected. */
void add_constraint(Layout& layout,
                    const std::shared_ptr<Layout::LayoutDescription>& desc,
                    const std::string& frame_id,
                    const Layout::Constraint& constraint) {
  ASSERT_EQ(layout.add_constraint_to_frame(desc, frame_id, constraint),
            Status::OK);
}

/** Add a CHARACTERS constraint with concise test syntax. */
void add_character_constraint(
    Layout& layout, const std::shared_ptr<Layout::LayoutDescription>& desc,
    const std::string& frame_id, Layout::ConstraintType type, size_t value) {
  add_constraint(layout, desc, frame_id,
                 Layout::make_character_constraint(type, value));
}

/**
 * Add a frame whose minimum and maximum dimensions are the same fixed values.
 */
void add_fixed_frame(Layout& layout,
                     const std::shared_ptr<Layout::LayoutDescription>& desc,
                     const std::string& frame_id, size_t width, size_t height,
                     std::shared_ptr<Frame> implementation = nullptr) {
  if (!implementation) {
    implementation = frame(frame_id);
  }
  ASSERT_EQ(layout.add_frame_to_layout_description(desc, frame_id,
                                                   std::move(implementation)),
            Status::OK);
  add_character_constraint(layout, desc, frame_id,
                           Layout::ConstraintType::MIN_WIDTH, width);
  add_character_constraint(layout, desc, frame_id,
                           Layout::ConstraintType::MAX_WIDTH, width);
  add_character_constraint(layout, desc, frame_id,
                           Layout::ConstraintType::MIN_HEIGHT, height);
  add_character_constraint(layout, desc, frame_id,
                           Layout::ConstraintType::MAX_HEIGHT, height);
}

/** Assert every field of one solved frame rectangle. */
void expect_rect(const Layout::AbsoluteLayout& absolute,
                 const std::string& frame_id, size_t x, size_t y, size_t width,
                 size_t height) {
  const auto found = absolute.frame_layouts.find(frame_id);
  ASSERT_NE(found, absolute.frame_layouts.end());
  EXPECT_EQ(found->second.x, x);
  EXPECT_EQ(found->second.y, y);
  EXPECT_EQ(found->second.width, width);
  EXPECT_EQ(found->second.height, height);
}

TEST(LayoutTest, ConstraintFactoriesPreserveTypesUnitsAndValues) {
  const Layout::Constraint percentage = Layout::make_percentage_constraint(
      Layout::ConstraintType::MAX_WIDTH, 0.5F);
  EXPECT_EQ(percentage.type, Layout::ConstraintType::MAX_WIDTH);
  EXPECT_EQ(percentage.unit, Layout::Unit::PERCENT);
  ASSERT_NE(std::get_if<float>(&percentage.value), nullptr);
  EXPECT_FLOAT_EQ(*std::get_if<float>(&percentage.value), 0.5F);

  const Layout::Constraint characters =
      Layout::make_character_constraint(Layout::ConstraintType::MIN_HEIGHT, 7);
  EXPECT_EQ(characters.unit, Layout::Unit::CHARACTERS);
  ASSERT_NE(std::get_if<size_t>(&characters.value), nullptr);
  EXPECT_EQ(*std::get_if<size_t>(&characters.value), 7U);

  const Layout::Constraint ratio =
      Layout::make_ratio_constraint(Layout::ConstraintType::ASPECT_RATIO, 4, 3);
  EXPECT_EQ(ratio.unit, Layout::Unit::RATIO);
  const auto* ratio_value = std::get_if<Layout::AspectRatio>(&ratio.value);
  ASSERT_NE(ratio_value, nullptr);
  EXPECT_EQ(ratio_value->width, 4);
  EXPECT_EQ(ratio_value->height, 3);

  const Layout::Constraint name = Layout::make_name_constraint(
      Layout::ConstraintType::LEFT_ANCHOR, "reference");
  EXPECT_EQ(name.unit, Layout::Unit::NAME);
  ASSERT_NE(std::get_if<std::string>(&name.value), nullptr);
  EXPECT_EQ(*std::get_if<std::string>(&name.value), "reference");
}

TEST(LayoutTest, AddsFramesAndRejectsInvalidFrameArguments) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");

  EXPECT_EQ(
      layout.add_frame_to_layout_description(nullptr, "frame", frame("frame")),
      Status::INVALID_ARGUMENT);
  EXPECT_EQ(layout.add_frame_to_layout_description(desc, "", frame("frame")),
            Status::INVALID_ARGUMENT);
  EXPECT_EQ(layout.add_frame_to_layout_description(desc, "frame", nullptr),
            Status::INVALID_ARGUMENT);
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);
  EXPECT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("duplicate")),
      Status::DUPLICATE_FRAME_ID);
  ASSERT_EQ(desc->z_buffer.frames().size(), 1U);
  EXPECT_TRUE(desc->constraints.contains("frame"));
}

TEST(LayoutTest, ValidatesConstraintsWhenTheyAreAdded) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);

  EXPECT_EQ(
      layout.add_constraint_to_frame(nullptr, "frame",
                                     Layout::make_character_constraint(
                                         Layout::ConstraintType::MIN_WIDTH, 1)),
      Status::INVALID_ARGUMENT);
  EXPECT_EQ(
      layout.add_constraint_to_frame(desc, "missing",
                                     Layout::make_character_constraint(
                                         Layout::ConstraintType::MIN_WIDTH, 1)),
      Status::FRAME_NOT_FOUND);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_percentage_constraint(
                    Layout::ConstraintType::MAX_WIDTH, -0.1F)),
            Status::INVALID_PERCENTAGE);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_percentage_constraint(
                    Layout::ConstraintType::MAX_WIDTH, std::nanf(""))),
            Status::INVALID_PERCENTAGE);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_ratio_constraint(
                    Layout::ConstraintType::ASPECT_RATIO, 4, 0)),
            Status::INVALID_RATIO);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_ratio_constraint(Layout::ConstraintType::MIN_WIDTH,
                                              4, 3)),
            Status::INVALID_CONSTRAINT);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_name_constraint(
                    Layout::ConstraintType::HORIZONTAL_CENTER, "other")),
            Status::INVALID_CONSTRAINT);

  const Layout::Constraint malformed{
      .type  = Layout::ConstraintType::MIN_WIDTH,
      .unit  = Layout::Unit::CHARACTERS,
      .value = 0.5F,
  };
  EXPECT_EQ(layout.add_constraint_to_frame(desc, "frame", malformed),
            Status::INVALID_CONSTRAINT);
}

TEST(LayoutTest, RejectsDuplicateAndAmbiguousPlacementConstraints) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);

  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::MIN_WIDTH, 2);
  EXPECT_EQ(
      layout.add_constraint_to_frame(desc, "frame",
                                     Layout::make_character_constraint(
                                         Layout::ConstraintType::MIN_WIDTH, 3)),
      Status::INVALID_CONSTRAINT);

  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::LEFT_ANCHOR, 0);
  EXPECT_EQ(layout.add_constraint_to_frame(
                desc, "frame",
                Layout::make_character_constraint(
                    Layout::ConstraintType::RIGHT_ANCHOR, 0)),
            Status::INVALID_CONSTRAINT);
}

TEST(LayoutTest, UnconstrainedFramesFillTheAvailableScreen) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 10, 5, kSquareCells, absolute),
            Status::OK);
  expect_rect(absolute, "frame", 0, 0, 10, 5);
}

TEST(LayoutTest, RejectsZeroSizedCharacterCells) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);

  Layout::AbsoluteLayout absolute;
  const CellDimensions invalid_cells{.width = 0, .height = 2};
  EXPECT_EQ(
      layout.compute_absolute_layout(desc, 10, 5, invalid_cells, absolute),
      Status::INVALID_DIMENSIONS);
  EXPECT_TRUE(absolute.frame_layouts.empty());

  size_t minimum_width  = 1;
  size_t minimum_height = 1;
  EXPECT_EQ(layout.compute_minimum_dimensions(desc, invalid_cells,
                                              minimum_width, minimum_height),
            Status::INVALID_DIMENSIONS);
  EXPECT_EQ(minimum_width, 0U);
  EXPECT_EQ(minimum_height, 0U);
}

TEST(LayoutTest, PlacesFixedFramesAtCornersAndExactCenter) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");

  add_fixed_frame(layout, desc, "top-left", 2, 1);
  add_character_constraint(layout, desc, "top-left",
                           Layout::ConstraintType::LEFT_ANCHOR, 0);
  add_character_constraint(layout, desc, "top-left",
                           Layout::ConstraintType::TOP_ANCHOR, 0);

  add_fixed_frame(layout, desc, "top-right", 2, 1);
  add_character_constraint(layout, desc, "top-right",
                           Layout::ConstraintType::RIGHT_ANCHOR, 0);
  add_character_constraint(layout, desc, "top-right",
                           Layout::ConstraintType::TOP_ANCHOR, 0);

  add_fixed_frame(layout, desc, "bottom-left", 2, 1);
  add_character_constraint(layout, desc, "bottom-left",
                           Layout::ConstraintType::LEFT_ANCHOR, 0);
  add_character_constraint(layout, desc, "bottom-left",
                           Layout::ConstraintType::BOTTOM_ANCHOR, 0);

  add_fixed_frame(layout, desc, "bottom-right", 2, 1);
  add_character_constraint(layout, desc, "bottom-right",
                           Layout::ConstraintType::RIGHT_ANCHOR, 0);
  add_character_constraint(layout, desc, "bottom-right",
                           Layout::ConstraintType::BOTTOM_ANCHOR, 0);

  add_fixed_frame(layout, desc, "center", 1, 1);
  add_character_constraint(layout, desc, "center",
                           Layout::ConstraintType::HORIZONTAL_CENTER, 0);
  add_character_constraint(layout, desc, "center",
                           Layout::ConstraintType::VERTICAL_CENTER, 0);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 10, 5, kSquareCells, absolute),
            Status::OK);
  expect_rect(absolute, "top-left", 0, 0, 2, 1);
  expect_rect(absolute, "top-right", 8, 0, 2, 1);
  expect_rect(absolute, "bottom-left", 0, 4, 2, 1);
  expect_rect(absolute, "bottom-right", 8, 4, 2, 1);
  expect_rect(absolute, "center", 4, 2, 1, 1);
}

TEST(LayoutTest, AppliesPercentageMaximumAndAspectRatio) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "metrics", frame("metrics")),
      Status::OK);
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::MIN_WIDTH, 24);
  add_constraint(layout, desc, "metrics",
                 Layout::make_percentage_constraint(
                     Layout::ConstraintType::MAX_WIDTH, 0.4F));
  add_constraint(layout, desc, "metrics",
                 Layout::make_ratio_constraint(
                     Layout::ConstraintType::ASPECT_RATIO, 4, 3));
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::RIGHT_ANCHOR, 0);
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::TOP_ANCHOR, 0);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(
      layout.compute_absolute_layout(desc, 80, 30, kSquareCells, absolute),
      Status::OK);
  expect_rect(absolute, "metrics", 48, 0, 32, 24);
}

TEST(LayoutTest, AspectRatioUsesVisualCellDimensions) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "metrics", frame("metrics")),
      Status::OK);
  add_constraint(layout, desc, "metrics",
                 Layout::make_percentage_constraint(
                     Layout::ConstraintType::MAX_WIDTH, 0.4F));
  add_constraint(layout, desc, "metrics",
                 Layout::make_ratio_constraint(
                     Layout::ConstraintType::ASPECT_RATIO, 4, 3));
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::RIGHT_ANCHOR, 0);
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::TOP_ANCHOR, 0);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(
      layout.compute_absolute_layout(desc, 80, 30, kTypicalCells, absolute),
      Status::OK);
  expect_rect(absolute, "metrics", 48, 0, 32, 12);

  const Canvas::Rect& metrics = absolute.frame_layouts.at("metrics");
  EXPECT_EQ(metrics.width * kTypicalCells.width * 3,
            metrics.height * kTypicalCells.height * 4);
}

TEST(LayoutTest, LetsScreenAndMaximumWinWhenScreenIsTooSmall) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::MIN_WIDTH, 10);
  add_constraint(layout, desc, "frame",
                 Layout::make_percentage_constraint(
                     Layout::ConstraintType::MAX_WIDTH, 0.5F));
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::MIN_HEIGHT, 5);
  add_constraint(layout, desc, "frame",
                 Layout::make_percentage_constraint(
                     Layout::ConstraintType::MAX_HEIGHT, 0.5F));

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 8, 4, kSquareCells, absolute),
            Status::OK);
  expect_rect(absolute, "frame", 0, 0, 4, 2);
}

TEST(LayoutTest, CombinesMarginsAndInwardAnchorOffsets) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  add_fixed_frame(layout, desc, "frame", 4, 2);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::LEFT_MARGIN, 2);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::RIGHT_MARGIN, 1);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::TOP_MARGIN, 1);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::BOTTOM_MARGIN, 1);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::RIGHT_ANCHOR, 1);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::BOTTOM_ANCHOR, 1);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 10, 6, kSquareCells, absolute),
            Status::OK);
  expect_rect(absolute, "frame", 4, 2, 4, 2);
}

TEST(LayoutTest, NamedAnchorsAlignTheCorrespondingReferenceEdges) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  add_fixed_frame(layout, desc, "follower", 4, 1);
  add_constraint(layout, desc, "follower",
                 Layout::make_name_constraint(
                     Layout::ConstraintType::RIGHT_ANCHOR, "reference"));
  add_constraint(layout, desc, "follower",
                 Layout::make_name_constraint(
                     Layout::ConstraintType::BOTTOM_ANCHOR, "reference"));

  add_fixed_frame(layout, desc, "reference", 2, 2);
  add_character_constraint(layout, desc, "reference",
                           Layout::ConstraintType::RIGHT_ANCHOR, 1);
  add_character_constraint(layout, desc, "reference",
                           Layout::ConstraintType::BOTTOM_ANCHOR, 1);

  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 10, 6, kSquareCells, absolute),
            Status::OK);
  expect_rect(absolute, "reference", 7, 3, 2, 2);
  expect_rect(absolute, "follower", 5, 4, 4, 1);
}

TEST(LayoutTest, DetectsMissingNamedFramesAndConstraintCycles) {
  Layout layout;
  const auto missing = layout.make_layout_description("missing");
  add_fixed_frame(layout, missing, "frame", 1, 1);
  add_constraint(layout, missing, "frame",
                 Layout::make_name_constraint(
                     Layout::ConstraintType::LEFT_ANCHOR, "unknown"));

  Layout::AbsoluteLayout absolute;
  EXPECT_EQ(
      layout.compute_absolute_layout(missing, 10, 5, kSquareCells, absolute),
      Status::FRAME_NOT_FOUND);
  EXPECT_TRUE(absolute.frame_layouts.empty());

  const auto cycle = layout.make_layout_description("cycle");
  add_fixed_frame(layout, cycle, "a", 1, 1);
  add_fixed_frame(layout, cycle, "b", 1, 1);
  add_constraint(
      layout, cycle, "a",
      Layout::make_name_constraint(Layout::ConstraintType::LEFT_ANCHOR, "b"));
  add_constraint(
      layout, cycle, "b",
      Layout::make_name_constraint(Layout::ConstraintType::RIGHT_ANCHOR, "a"));
  EXPECT_EQ(
      layout.compute_absolute_layout(cycle, 10, 5, kSquareCells, absolute),
      Status::CONSTRAINT_CYCLE);
  EXPECT_TRUE(absolute.frame_layouts.empty());
}

TEST(LayoutTest, ComputesMinimumDimensionsFromFixedAndPercentageConstraints) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "metrics", frame("metrics")),
      Status::OK);
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::MIN_WIDTH, 24);
  add_constraint(layout, desc, "metrics",
                 Layout::make_percentage_constraint(
                     Layout::ConstraintType::MAX_WIDTH, 0.4F));
  add_constraint(layout, desc, "metrics",
                 Layout::make_ratio_constraint(
                     Layout::ConstraintType::ASPECT_RATIO, 4, 3));
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::RIGHT_ANCHOR, 0);
  add_character_constraint(layout, desc, "metrics",
                           Layout::ConstraintType::TOP_ANCHOR, 0);

  size_t minimum_width  = 0;
  size_t minimum_height = 0;
  ASSERT_EQ(layout.compute_minimum_dimensions(desc, kSquareCells, minimum_width,
                                              minimum_height),
            Status::OK);
  EXPECT_EQ(minimum_width, 60U);
  EXPECT_EQ(minimum_height, 18U);

  ASSERT_EQ(layout.compute_minimum_dimensions(desc, kTypicalCells,
                                              minimum_width, minimum_height),
            Status::OK);
  EXPECT_EQ(minimum_width, 60U);
  EXPECT_EQ(minimum_height, 9U);
}

TEST(LayoutTest, RejectsIntrinsicallyContradictoryBounds) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::MIN_WIDTH, 10);
  add_character_constraint(layout, desc, "frame",
                           Layout::ConstraintType::MAX_WIDTH, 5);

  Layout::AbsoluteLayout absolute;
  EXPECT_EQ(
      layout.compute_absolute_layout(desc, 20, 10, kSquareCells, absolute),
      Status::INVALID_CONSTRAINT);
}

TEST(LayoutTest, DrawsDirtyFramesInZBufferOrderAndSkipsCleanFrames) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  std::vector<DrawCall> calls;
  ASSERT_EQ(layout.add_frame_to_layout_description(desc, "back",
                                                   frame("back", &calls)),
            Status::OK);
  ASSERT_EQ(layout.add_frame_to_layout_description(
                desc, "clean", frame("clean", &calls, false)),
            Status::OK);
  ASSERT_EQ(layout.add_frame_to_layout_description(desc, "front",
                                                   frame("front", &calls)),
            Status::OK);

  Canvas canvas(10, 5);
  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 10, 5, kSquareCells, absolute),
            Status::OK);
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  ASSERT_EQ(layout.draw(desc, absolute, State{}, Theme{}, canvas), Status::OK);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  ASSERT_EQ(calls.size(), 2U);
  EXPECT_EQ(calls[0].frame_id, "back");
  EXPECT_EQ(calls[1].frame_id, "front");
  EXPECT_EQ(calls[0].rect.width, 10U);
  EXPECT_EQ(calls[0].rect.height, 5U);
}

TEST(LayoutTest, StopsDrawingAtTheFirstFrameError) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  std::vector<DrawCall> calls;
  ASSERT_EQ(layout.add_frame_to_layout_description(
                desc, "broken",
                frame("broken", &calls, true, Status::TERMINAL_WRITE_FAILED)),
            Status::OK);
  ASSERT_EQ(layout.add_frame_to_layout_description(desc, "later",
                                                   frame("later", &calls)),
            Status::OK);

  Canvas canvas(2, 2);
  Layout::AbsoluteLayout absolute;
  ASSERT_EQ(layout.compute_absolute_layout(desc, 2, 2, kSquareCells, absolute),
            Status::OK);
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(layout.draw(desc, absolute, State{}, Theme{}, canvas),
            Status::TERMINAL_WRITE_FAILED);
  ASSERT_EQ(canvas.end_frame(), Status::OK);

  ASSERT_EQ(calls.size(), 1U);
  EXPECT_EQ(calls.front().frame_id, "broken");
}

TEST(LayoutTest, DrawRequiresASolvedRectangleForEveryFrame) {
  Layout layout;
  const auto desc = layout.make_layout_description("test");
  ASSERT_EQ(
      layout.add_frame_to_layout_description(desc, "frame", frame("frame")),
      Status::OK);

  Canvas canvas(2, 2);
  const Layout::AbsoluteLayout empty_absolute;
  ASSERT_EQ(canvas.begin_frame(), Status::OK);
  EXPECT_EQ(layout.draw(desc, empty_absolute, State{}, Theme{}, canvas),
            Status::FRAME_NOT_FOUND);
  EXPECT_EQ(layout.draw(nullptr, empty_absolute, State{}, Theme{}, canvas),
            Status::INVALID_ARGUMENT);
  ASSERT_EQ(canvas.end_frame(), Status::OK);
}

}  // namespace
}  // namespace puc::tui
