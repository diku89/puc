/**
 * @file status_test.cpp
 * @brief Unit tests for TUI status classification and diagnostic messages.
 */

#include "puc-cli/tui/status.hpp"

#include <array>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

TEST(StatusTest, OnlyOkIsSuccessful) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::INVALID_ARGUMENT));
  EXPECT_FALSE(is_ok(Status::TERMINAL_WRITE_FAILED));
}

TEST(StatusTest, EveryStatusHasAHumanReadableMessage) {
  constexpr std::array statuses{
      Status::OK,
      Status::INVALID_ARGUMENT,
      Status::INVALID_DIMENSIONS,
      Status::DIMENSION_OVERFLOW,
      Status::FRAME_ALREADY_IN_PROGRESS,
      Status::NO_FRAME_IN_PROGRESS,
      Status::RECT_OUT_OF_BOUNDS,
      Status::CELL_SHAPE_MISMATCH,
      Status::DUPLICATE_FRAME_ID,
      Status::FRAME_NOT_FOUND,
      Status::INVALID_PERCENTAGE,
      Status::INVALID_RATIO,
      Status::INVALID_CONSTRAINT,
      Status::CONSTRAINT_CYCLE,
      Status::CANVAS_NOT_SET,
      Status::TERMINAL_NOT_AVAILABLE,
      Status::TERMINAL_QUERY_FAILED,
      Status::TERMINAL_CONFIG_FAILED,
      Status::TERMINAL_WRITE_FAILED,
      Status::EVENT_BUFFER_FULL,
  };

  for (const Status status : statuses) {
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status),
              std::string_view{"unknown terminal UI status"});
  }
}

TEST(StatusTest, UnknownValuesStillHaveAMessage) {
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown terminal UI status");
}

}  // namespace
}  // namespace puc::tui
