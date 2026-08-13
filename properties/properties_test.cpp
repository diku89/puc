/**
 * @file properties_test.cpp
 * @brief Tests for immutable documents and user-mutable property state.
 */

#include "properties/properties.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"

namespace puc::properties {
namespace {

class PropertiesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(test_tmpdir, nullptr);
    const ::testing::TestInfo* test_info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(test_info, nullptr);

    root_ = std::filesystem::path{test_tmpdir} /
            (std::string{"properties_test_"} + test_info->name());
    primary_ = root_ / "primary";
    user_    = root_ / "user";
    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(root_, error));
    error.clear();
    ASSERT_TRUE(std::filesystem::create_directories(primary_, error));
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::create_directories(user_, error));
    ASSERT_FALSE(error);
  }

  void write(const std::filesystem::path& root,
             const std::filesystem::path& relative_path,
             std::string_view contents) {
    const std::filesystem::path path = root / relative_path;
    std::error_code error;
    static_cast<void>(
        std::filesystem::create_directories(path.parent_path(), error));
    ASSERT_FALSE(error);
    FILE* file = std::fopen(path.string().c_str(), "wb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fwrite(contents.data(), 1U, contents.size(), file),
              contents.size());
    EXPECT_EQ(std::fclose(file), 0);
  }

  std::filesystem::path root_;
  std::filesystem::path primary_;
  std::filesystem::path user_;
};

TEST_F(PropertiesTest, RetainsImmutableDocumentsBehindPropertiesViews) {
  write(primary_, "terminal.toml", R"toml(
name = "xterm"
version = 2

[[mapping]]
sequence = "escape"
enabled = true
)toml");
  Properties properties{primary_, user_};

  const LoadResult loaded =
      properties.load_immutable("terminal.input", "terminal.toml");
  ASSERT_EQ(loaded.status, Status::OK);
  EXPECT_EQ(loaded.find("name").as_string(), std::string_view{"xterm"});
  EXPECT_EQ(loaded.find("mapping").at(0).find("sequence").as_string(),
            std::string_view{"escape"});

  Property property;
  ASSERT_EQ(properties.get("terminal.input.version", property), Status::OK);
  EXPECT_EQ(property.value, Scalar{std::int64_t{2}});
  EXPECT_EQ(property.mutability, Mutability::IMMUTABLE);
  EXPECT_EQ(properties.set("terminal.input.version", "3"),
            Status::IMMUTABLE_PROPERTY);
  EXPECT_EQ(properties.list("terminal.input.mapping").size(), 2U);
}

TEST_F(PropertiesTest, MutatesScalarsAndPreservesChangesAcrossReload) {
  write(primary_, "settings.toml", R"toml(
[timeouts]
input_ms = 50
click_ms = 500
enabled = true
label = "default"
)toml");
  write(user_, "settings.toml", R"toml(
[timeouts]
click_ms = 750
)toml");
  Properties properties{primary_, user_};
  ASSERT_EQ(properties.load_mutable_defaults("terminal", "settings.toml"),
            Status::OK);

  Property property;
  ASSERT_EQ(properties.get("terminal.timeouts.click_ms", property), Status::OK);
  EXPECT_EQ(property.value, Scalar{std::int64_t{750}});
  EXPECT_FALSE(property.user_modified);

  ASSERT_EQ(properties.set("terminal.timeouts.click_ms", "900"), Status::OK);
  ASSERT_EQ(properties.set("terminal.timeouts.input_ms", "0x40"), Status::OK);
  ASSERT_EQ(properties.set("terminal.timeouts.label", "interactive"),
            Status::OK);
  EXPECT_EQ(properties.set("terminal.timeouts.enabled", "maybe"),
            Status::INVALID_VALUE);

  write(primary_, "settings.toml", R"toml(
[timeouts]
input_ms = 60
click_ms = 500
enabled = false
label = "new default"
)toml");
  ASSERT_EQ(properties.reload(), Status::OK);

  ASSERT_EQ(properties.get("terminal.timeouts.input_ms", property), Status::OK);
  EXPECT_EQ(property.value, Scalar{std::int64_t{64}});
  EXPECT_TRUE(property.user_modified);
  ASSERT_EQ(properties.get("terminal.timeouts.click_ms", property), Status::OK);
  EXPECT_EQ(property.value, Scalar{std::int64_t{900}});
  EXPECT_TRUE(property.user_modified);
  ASSERT_EQ(properties.get("terminal.timeouts.label", property), Status::OK);
  EXPECT_EQ(property.value, Scalar{std::string{"interactive"}});
}

TEST_F(PropertiesTest, RejectsMutableArraysAndConflictingSources) {
  write(primary_, "array.toml", "values = [1, 2]\n");
  write(primary_, "one.toml", "value = 1\n");
  write(primary_, "two.toml", "value = 2\n");
  Properties properties{primary_, user_};

  EXPECT_EQ(properties.load_mutable_defaults("array", "array.toml"),
            Status::UNSUPPORTED_VALUE);
  EXPECT_EQ(properties.load_immutable("shared", "one.toml").status, Status::OK);
  EXPECT_EQ(properties.load_immutable("shared", "two.toml").status,
            Status::DUPLICATE_SOURCE);
  EXPECT_EQ(properties.load_immutable("bad..name", "one.toml").status,
            Status::INVALID_PATH);
}

}  // namespace
}  // namespace puc::properties
