/**
 * @file status_test.cpp
 * @brief Tests for complete, stable terminal status diagnostics.
 */

#include "puc-cli/terminal/status.hpp"

#include <array>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

TEST(TerminalStatusTest, OnlyOkIsSuccessful) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::INVALID_ARGUMENT));
  EXPECT_FALSE(is_ok(Status::TERMINAL_READ_FAILED));
  EXPECT_FALSE(is_ok(Status::UNSUPPORTED));
}

TEST(TerminalStatusTest, EveryDeclaredStatusHasHumanReadableText) {
  constexpr std::array statuses{
      Status::OK,
      Status::INVALID_ARGUMENT,
      Status::ALREADY_ACTIVE,
      Status::NOT_ACTIVE,
      Status::TERMINAL_NOT_AVAILABLE,
      Status::TERMINAL_CONFIG_FAILED,
      Status::TERMINAL_QUERY_FAILED,
      Status::TERMINAL_READ_FAILED,
      Status::TERMINAL_WRITE_FAILED,
      Status::CHANNEL_SETUP_FAILED,
      Status::INPUT_LIMIT_EXCEEDED,
      Status::OUTPUT_LIMIT_EXCEEDED,
      Status::CONFIGURATION_LOAD_FAILED,
      Status::CONFIGURATION_PARSE_FAILED,
      Status::TERMINFO_LOAD_FAILED,
      Status::UNSUPPORTED,
  };
  for (const Status status : statuses) {
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status), "unknown terminal status");
  }
}

TEST(TerminalStatusTest, UnknownEnumValuesHaveASafeFallback) {
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            std::string_view{"unknown terminal status"});
}

}  // namespace
}  // namespace puc::terminal
