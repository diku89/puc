#pragma once

/**
 * @file terminal_test_options.hpp
 * @brief Non-throwing command-line parser for the terminal conformance app.
 */

#include <optional>
#include <span>
#include <string_view>

#include "puc-cli/terminal/terminal_test_runner.hpp"

namespace puc::terminal {

/** Top-level operation selected before any terminal resources are acquired. */
enum class TerminalTestCommand {
  RUN,  /**< Execute the full plan or one selected test. */
  LIST, /**< Print available test names and exit. */
  HELP, /**< Print command usage and exit. */
};

/** Result of parsing terminal-test command-line arguments. */
enum class TerminalTestOptionsStatus {
  OK,                  /**< Every token formed one supported command. */
  UNKNOWN_ARGUMENT,    /**< A token is not a recognized option. */
  MISSING_TEST_NAME,   /**< `--test` has no following name. */
  UNKNOWN_TEST_NAME,   /**< A name is absent from the test registry. */
  CONFLICTING_OPTIONS, /**< More than one operation was requested. */
};

/** Validated command-line selection plus optional error context. */
struct TerminalTestOptions {
  TerminalTestCommand command = TerminalTestCommand::RUN; /**< Operation. */
  std::optional<InputConformanceTest>
      selected_test; /**< The sole test for RUN, or all tests when empty. */
  std::string_view argument; /**< Argument associated with a parse failure. */

  /** Compare the complete parsed value. */
  constexpr bool operator==(const TerminalTestOptions&) const noexcept =
      default;
};

/**
 * Parse arguments following argv[0] without allocating or throwing.
 *
 * Supported forms are no arguments, `--list`, `--help`, and
 * `--test <test-name>`. Test names are the stable values returned by
 * input_conformance_tests(). Options that request multiple operations are
 * rejected rather than resolved by argument order.
 *
 * @param[in] arguments Command-line tokens excluding argv[0].
 * @param[out] options Parsed selection or failure context.
 * @return TerminalTestOptionsStatus::OK or a human-readable parse error.
 */
TerminalTestOptionsStatus parse_terminal_test_options(
    std::span<const std::string_view> arguments,
    TerminalTestOptions& options) noexcept;

/** Return human-readable text for every parser result. */
constexpr std::string_view terminal_test_options_status_message(
    TerminalTestOptionsStatus status) noexcept {
  switch (status) {
    case TerminalTestOptionsStatus::OK:
      return "ok";
    case TerminalTestOptionsStatus::UNKNOWN_ARGUMENT:
      return "unknown command-line argument";
    case TerminalTestOptionsStatus::MISSING_TEST_NAME:
      return "--test requires a test name";
    case TerminalTestOptionsStatus::UNKNOWN_TEST_NAME:
      return "unknown terminal test name";
    case TerminalTestOptionsStatus::CONFLICTING_OPTIONS:
      return "only one of --list, --help, or --test may be specified";
  }
  return "unknown terminal-test option status";
}

}  // namespace puc::terminal
