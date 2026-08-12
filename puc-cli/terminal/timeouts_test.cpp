/**
 * @file timeouts_test.cpp
 * @brief Unit tests for layered terminal timeout configuration.
 */

#include "puc-cli/terminal/timeouts.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "gtest/gtest.h"
#include "properties/properties.hpp"

namespace puc::terminal {
namespace {

using namespace std::chrono_literals;

bool write_file(const std::filesystem::path& path, std::string_view contents) {
  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fwrite(contents.data(), 1U, contents.size(),
                                   file) == contents.size();
  return std::fclose(file) == 0 && written;
}

std::filesystem::path runfiles_root() {
  std::error_code error;
  const std::filesystem::path root = std::filesystem::current_path(error);
  EXPECT_FALSE(error);
  return root;
}

std::filesystem::path make_override_root(std::string_view name) {
  const char* temporary = std::getenv("TEST_TMPDIR");
  EXPECT_NE(temporary, nullptr);
  if (temporary == nullptr) {
    return {};
  }
  const std::filesystem::path root =
      std::filesystem::path{temporary} / std::string{name};
  std::error_code error;
  static_cast<void>(std::filesystem::remove_all(root, error));
  error.clear();
  EXPECT_TRUE(std::filesystem::create_directories(root, error));
  EXPECT_FALSE(error);
  return root;
}

TEST(TerminalTimeoutSettingsTest, LoadsRepositoryDefaults) {
  const std::filesystem::path primary = runfiles_root();
  properties::Properties properties{primary, primary / "missing_overrides"};
  TimeoutSettings settings{.input_sequence = 1ms, .multiple_click = 1ms};

  ASSERT_EQ(load_timeout_settings(properties, settings), Status::OK);
  EXPECT_EQ(settings.input_sequence, 50ms);
  EXPECT_EQ(settings.multiple_click, 500ms);
}

TEST(TerminalTimeoutSettingsTest, UserFileOverridesIndividualDefaults) {
  const std::filesystem::path primary = runfiles_root();
  const std::filesystem::path overrides =
      make_override_root("terminal_timeout_overrides");
  ASSERT_FALSE(overrides.empty());
  ASSERT_TRUE(write_file(overrides / kTimeoutConfigurationPath, R"toml(
[timeouts]
multiple_click_ms = 750
)toml"));
  properties::Properties properties{primary, overrides};
  TimeoutSettings settings;

  ASSERT_EQ(load_timeout_settings(properties, settings), Status::OK);
  EXPECT_EQ(settings.input_sequence, 50ms);
  EXPECT_EQ(settings.multiple_click, 750ms);
}

TEST(TerminalTimeoutSettingsTest, InvalidOverrideDoesNotPartiallyMutateOutput) {
  const std::filesystem::path primary = runfiles_root();
  const std::filesystem::path overrides =
      make_override_root("invalid_terminal_timeout_overrides");
  ASSERT_FALSE(overrides.empty());
  ASSERT_TRUE(write_file(overrides / kTimeoutConfigurationPath, R"toml(
[timeouts]
input_sequence_ms = 75
multiple_click_ms = 0
)toml"));
  properties::Properties properties{primary, overrides};
  const TimeoutSettings original{.input_sequence = 12ms,
                                 .multiple_click = 34ms};
  TimeoutSettings settings = original;

  EXPECT_EQ(load_timeout_settings(properties, settings),
            Status::CONFIGURATION_PARSE_FAILED);
  EXPECT_EQ(settings, original);
}

}  // namespace
}  // namespace puc::terminal
