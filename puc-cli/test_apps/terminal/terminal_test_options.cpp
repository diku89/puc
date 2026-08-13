/**
 * @file terminal_test_options.cpp
 * @brief Command-line validation for the terminal conformance executable.
 */

#include "puc-cli/test_apps/terminal/terminal_test_options.hpp"

#include <cstddef>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>

namespace puc::terminal {
namespace {

/** Store failure context while keeping each parser return site concise. */
TerminalTestOptionsStatus fail(TerminalTestOptions& output,
                               TerminalTestOptions parsed,
                               TerminalTestOptionsStatus status,
                               std::string_view argument = {}) noexcept {
  parsed.argument = argument;
  output          = parsed;
  return status;
}

}  // namespace

TerminalTestOptionsStatus parse_terminal_test_options(
    std::span<const std::string_view> arguments,
    TerminalTestOptions& options) noexcept {
  TerminalTestOptions parsed;
  for (std::size_t index = 0U; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--list") {
      if (parsed.command != TerminalTestCommand::RUN ||
          parsed.selected_test.has_value()) {
        return fail(options, parsed,
                    TerminalTestOptionsStatus::CONFLICTING_OPTIONS, argument);
      }
      parsed.command = TerminalTestCommand::LIST;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      if (parsed.command != TerminalTestCommand::RUN ||
          parsed.selected_test.has_value()) {
        return fail(options, parsed,
                    TerminalTestOptionsStatus::CONFLICTING_OPTIONS, argument);
      }
      parsed.command = TerminalTestCommand::HELP;
      continue;
    }
    if (argument == "--test") {
      if (parsed.command != TerminalTestCommand::RUN ||
          parsed.selected_test.has_value()) {
        return fail(options, parsed,
                    TerminalTestOptionsStatus::CONFLICTING_OPTIONS, argument);
      }
      if (index + 1U >= arguments.size() ||
          arguments[index + 1U].starts_with("--")) {
        return fail(options, parsed,
                    TerminalTestOptionsStatus::MISSING_TEST_NAME, argument);
      }
      const std::string_view test_name = arguments[++index];
      const std::optional<InputConformanceTest> test =
          find_input_conformance_test(test_name);
      if (!test.has_value()) {
        return fail(options, parsed,
                    TerminalTestOptionsStatus::UNKNOWN_TEST_NAME, test_name);
      }
      parsed.selected_test = *test;
      continue;
    }
    return fail(options, parsed, TerminalTestOptionsStatus::UNKNOWN_ARGUMENT,
                argument);
  }
  options = parsed;
  return TerminalTestOptionsStatus::OK;
}

void print_terminal_test_usage(std::ostream& output,
                               std::string_view executable) {
  const std::string_view program =
      executable.empty() ? std::string_view{"terminal-test"} : executable;
  output << "Usage:\n"
         << "  " << program << "\n"
         << "  " << program << " --list\n"
         << "  " << program << " --test <test-name>\n"
         << "  " << program << " --help\n";
}

void print_terminal_test_list(std::ostream& output) {
  output << "Available terminal input tests:\n";
  for (const InputConformanceTestDescriptor& descriptor :
       input_conformance_tests()) {
    output << "  " << descriptor.cli_name << "\n"
           << "      " << descriptor.name << ": " << descriptor.instruction
           << '\n';
  }
}

}  // namespace puc::terminal
