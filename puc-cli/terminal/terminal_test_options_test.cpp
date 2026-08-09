/**
 * @file terminal_test_options_test.cpp
 * @brief Tests for terminal-test command-line validation.
 */

#include "puc-cli/terminal/terminal_test_options.hpp"

#include <array>
#include <span>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

TEST(TerminalTestOptionsTest, EmptyArgumentsSelectTheFullRun) {
  TerminalTestOptions options;
  EXPECT_EQ(parse_terminal_test_options({}, options),
            TerminalTestOptionsStatus::OK);
  EXPECT_EQ(options.command, TerminalTestCommand::RUN);
  EXPECT_EQ(options.selected_test, std::nullopt);
}

TEST(TerminalTestOptionsTest, ListAndHelpSelectNonInteractiveCommands) {
  TerminalTestOptions options;
  constexpr std::array<std::string_view, 1U> list{"--list"};
  EXPECT_EQ(parse_terminal_test_options(list, options),
            TerminalTestOptionsStatus::OK);
  EXPECT_EQ(options.command, TerminalTestCommand::LIST);
  EXPECT_EQ(options.selected_test, std::nullopt);

  constexpr std::array<std::string_view, 1U> help{"-h"};
  EXPECT_EQ(parse_terminal_test_options(help, options),
            TerminalTestOptionsStatus::OK);
  EXPECT_EQ(options.command, TerminalTestCommand::HELP);
}

TEST(TerminalTestOptionsTest, TestSelectsOneKnownStableName) {
  TerminalTestOptions options;
  constexpr std::array<std::string_view, 2U> arguments{"--test",
                                                       "clipboard-paste"};
  EXPECT_EQ(parse_terminal_test_options(arguments, options),
            TerminalTestOptionsStatus::OK);
  EXPECT_EQ(options.command, TerminalTestCommand::RUN);
  EXPECT_EQ(options.selected_test, InputConformanceTest::CLIPBOARD_PASTE);
}

TEST(TerminalTestOptionsTest, RejectsMissingAndUnknownTestNames) {
  TerminalTestOptions options;
  constexpr std::array<std::string_view, 1U> missing{"--test"};
  EXPECT_EQ(parse_terminal_test_options(missing, options),
            TerminalTestOptionsStatus::MISSING_TEST_NAME);
  EXPECT_EQ(options.argument, "--test");

  constexpr std::array<std::string_view, 2U> unknown{"--test", "telepathy"};
  EXPECT_EQ(parse_terminal_test_options(unknown, options),
            TerminalTestOptionsStatus::UNKNOWN_TEST_NAME);
  EXPECT_EQ(options.argument, "telepathy");
}

TEST(TerminalTestOptionsTest, RejectsUnknownAndConflictingOptions) {
  TerminalTestOptions options;
  constexpr std::array<std::string_view, 1U> unknown{"--wat"};
  EXPECT_EQ(parse_terminal_test_options(unknown, options),
            TerminalTestOptionsStatus::UNKNOWN_ARGUMENT);
  EXPECT_EQ(options.argument, "--wat");

  constexpr std::array<std::string_view, 3U> conflict{"--list", "--test",
                                                      "text"};
  EXPECT_EQ(parse_terminal_test_options(conflict, options),
            TerminalTestOptionsStatus::CONFLICTING_OPTIONS);
  EXPECT_EQ(options.argument, "--test");

  constexpr std::array<std::string_view, 4U> duplicate{"--test", "text",
                                                       "--test", "focus"};
  EXPECT_EQ(parse_terminal_test_options(duplicate, options),
            TerminalTestOptionsStatus::CONFLICTING_OPTIONS);
}

TEST(TerminalTestOptionsTest, EveryStatusHasReadableText) {
  EXPECT_EQ(terminal_test_options_status_message(TerminalTestOptionsStatus::OK),
            "ok");
  EXPECT_NE(terminal_test_options_status_message(
                static_cast<TerminalTestOptionsStatus>(-1)),
            "");
}

}  // namespace
}  // namespace puc::terminal
