/**
 * @file zbuf_test.cpp
 * @brief Unit tests for ZBuffer ownership, ordering, removal, and promotion.
 */

#include "puc-cli/tui/zbuf.hpp"

#include <memory>
#include <string>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Minimal always-dirty frame used to test container behavior. */
class TestFrame final : public Frame {
 public:
  explicit TestFrame(std::string name) : Frame(std::move(name)) {}

  Status draw(const State&, const Theme&, Canvas&,
              const Canvas::Rect&) override {
    return Status::OK;
  }

  bool needs_update() const override { return true; }
};

/** Allocate a TestFrame through the public Frame ownership type. */
std::shared_ptr<Frame> frame(std::string name) {
  return std::make_shared<TestFrame>(std::move(name));
}

TEST(ZBufferTest, AddsFramesInBackToFrontOrder) {
  ZBuffer buffer;

  EXPECT_EQ(buffer.add("back", frame("back")), Status::OK);
  EXPECT_EQ(buffer.add("middle", frame("middle")), Status::OK);
  EXPECT_EQ(buffer.add("front", frame("front")), Status::OK);

  ASSERT_EQ(buffer.frames().size(), 3U);
  EXPECT_EQ(buffer.frames()[0].frame_id, "back");
  EXPECT_EQ(buffer.frames()[1].frame_id, "middle");
  EXPECT_EQ(buffer.frames()[2].frame_id, "front");
}

TEST(ZBufferTest, RejectsEmptyIdsNullFramesAndDuplicates) {
  ZBuffer buffer;

  EXPECT_EQ(buffer.add("", frame("frame")), Status::INVALID_ARGUMENT);
  EXPECT_EQ(buffer.add("frame", nullptr), Status::INVALID_ARGUMENT);
  ASSERT_EQ(buffer.add("frame", frame("first")), Status::OK);
  EXPECT_EQ(buffer.add("frame", frame("replacement")),
            Status::DUPLICATE_FRAME_ID);
  EXPECT_EQ(buffer.frames().size(), 1U);
}

TEST(ZBufferTest, RemovesFramesWithoutChangingRemainingOrder) {
  ZBuffer buffer;
  ASSERT_EQ(buffer.add("one", frame("one")), Status::OK);
  ASSERT_EQ(buffer.add("two", frame("two")), Status::OK);
  ASSERT_EQ(buffer.add("three", frame("three")), Status::OK);

  EXPECT_EQ(buffer.remove("two"), Status::OK);
  ASSERT_EQ(buffer.frames().size(), 2U);
  EXPECT_EQ(buffer.frames()[0].frame_id, "one");
  EXPECT_EQ(buffer.frames()[1].frame_id, "three");
  EXPECT_EQ(buffer.remove("missing"), Status::FRAME_NOT_FOUND);
}

TEST(ZBufferTest, ReleasesItsOwnershipWhenFrameIsRemoved) {
  ZBuffer buffer;
  std::shared_ptr<Frame> owned    = frame("owned");
  const std::weak_ptr<Frame> weak = owned;
  ASSERT_EQ(buffer.add("owned", owned), Status::OK);
  owned.reset();

  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(buffer.remove("owned"), Status::OK);
  EXPECT_TRUE(weak.expired());
}

TEST(ZBufferTest, BringsAnyFrameToTheFront) {
  ZBuffer buffer;
  ASSERT_EQ(buffer.add("one", frame("one")), Status::OK);
  ASSERT_EQ(buffer.add("two", frame("two")), Status::OK);
  ASSERT_EQ(buffer.add("three", frame("three")), Status::OK);

  EXPECT_EQ(buffer.bring_to_front("one"), Status::OK);
  ASSERT_EQ(buffer.frames().size(), 3U);
  EXPECT_EQ(buffer.frames()[0].frame_id, "two");
  EXPECT_EQ(buffer.frames()[1].frame_id, "three");
  EXPECT_EQ(buffer.frames()[2].frame_id, "one");
}

TEST(ZBufferTest, BringingFrontFrameToFrontIsStable) {
  ZBuffer buffer;
  const auto one = frame("one");
  const auto two = frame("two");
  ASSERT_EQ(buffer.add("one", one), Status::OK);
  ASSERT_EQ(buffer.add("two", two), Status::OK);

  EXPECT_EQ(buffer.bring_to_front("two"), Status::OK);
  ASSERT_EQ(buffer.frames().size(), 2U);
  EXPECT_EQ(buffer.frames()[0].frame, one);
  EXPECT_EQ(buffer.frames()[1].frame, two);
  EXPECT_EQ(buffer.bring_to_front("missing"), Status::FRAME_NOT_FOUND);
}

}  // namespace
}  // namespace puc::tui
