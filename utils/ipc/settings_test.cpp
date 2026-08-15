/**
 * @file settings_test.cpp
 * @brief IPC configuration loading and override tests.
 */

#include "utils/ipc/settings.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "properties/properties.hpp"

namespace puc::ipc {
namespace {

class IpcSettingsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* temporary_directory = std::getenv("TEST_TMPDIR");
    ASSERT_NE(temporary_directory, nullptr);
    root_      = std::filesystem::path{temporary_directory} / "ipc-settings";
    defaults_  = root_ / "defaults";
    overrides_ = root_ / "overrides";
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(defaults_);
    std::filesystem::create_directories(overrides_);
  }

  void write(const std::filesystem::path& root, std::string_view value) {
    std::ofstream output{root / "ipc.toml"};
    ASSERT_TRUE(output.is_open());
    output << "[channel]\nmaximum_message_bytes = " << value << "\n";
  }

  std::filesystem::path root_;
  std::filesystem::path defaults_;
  std::filesystem::path overrides_;
};

TEST_F(IpcSettingsTest, LoadsOneGibibyteDefault) {
  write(defaults_, "1073741824");
  properties::Properties properties{defaults_, overrides_};
  Settings settings;
  ASSERT_TRUE(load_settings(properties, settings));
  EXPECT_EQ(settings.maximum_message_bytes, 1073741824U);
}

TEST_F(IpcSettingsTest, AppliesUserOverride) {
  write(defaults_, "1073741824");
  write(overrides_, "4096");
  properties::Properties properties{defaults_, overrides_};
  Settings settings;
  ASSERT_TRUE(load_settings(properties, settings));
  EXPECT_EQ(settings.maximum_message_bytes, 4096U);
}

TEST_F(IpcSettingsTest, RejectsNonpositiveAndUnframableLimits) {
  for (const std::string_view value : {"0", "4294967296"}) {
    write(defaults_, value);
    properties::Properties properties{defaults_, overrides_};
    Settings settings;
    EXPECT_FALSE(load_settings(properties, settings));
    EXPECT_EQ(settings.maximum_message_bytes, 0U);
  }
}

}  // namespace
}  // namespace puc::ipc
