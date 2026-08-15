/**
 * @file config_test.cpp
 * @brief Tests for root-scoped TOML loading, merging, and value lifetime.
 */

#include "utils/config/config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "gtest/gtest.h"

namespace puc::config {
namespace {

class ConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* test_tmpdir = std::getenv("TEST_TMPDIR");
    ASSERT_NE(test_tmpdir, nullptr);
    const ::testing::TestInfo* test_info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    ASSERT_NE(test_info, nullptr);

    test_root_          = std::filesystem::path{test_tmpdir} /
                          (std::string{"config_test_"} + test_info->name());
    primary_root_       = test_root_ / "primary";
    user_override_root_ = test_root_ / "user_overrides";

    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(test_root_, error));
    error.clear();
    ASSERT_TRUE(std::filesystem::create_directories(primary_root_, error));
    ASSERT_FALSE(error);
    ASSERT_TRUE(
        std::filesystem::create_directories(user_override_root_, error));
    ASSERT_FALSE(error);
  }

  void write_config(const std::filesystem::path& root,
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

  std::filesystem::path test_root_;
  std::filesystem::path primary_root_;
  std::filesystem::path user_override_root_;
};

TEST_F(ConfigTest, LoadsEveryTomlValueKindThroughParserIndependentViews) {
  write_config(primary_root_, "types.toml", R"toml(
title = "PUC"
count = 42
ratio = 1.5
enabled = true
date = 2026-08-08
time = 13:14:15.123456
local = 2026-08-08T13:14:15
offset = 2026-08-08T13:14:15-07:00
items = ["one", "two"]

[nested]
answer = 42
)toml");

  const Config config{primary_root_, user_override_root_};
  const LoadResult loaded = config.load("types.toml");
  ASSERT_EQ(loaded.status, Status::OK);
  ASSERT_TRUE(loaded.document);

  const Value root = loaded.document.root();
  EXPECT_EQ(root.type(), ValueType::TABLE);
  EXPECT_EQ(root.find("title").as_string(), std::string_view{"PUC"});
  EXPECT_EQ(root.find("count").as_integer(), 42);
  EXPECT_EQ(root.find("ratio").as_float(), 1.5);
  EXPECT_EQ(root.find("enabled").as_boolean(), true);
  EXPECT_EQ(root.find("date").as_date(),
            (Date{.year = 2026, .month = 8, .day = 8}));
  EXPECT_EQ(
      root.find("time").as_time(),
      (Time{.hour = 13, .minute = 14, .second = 15, .microsecond = 123456}));
  EXPECT_EQ(root.find("local").as_date_time(),
            (DateTime{.date = Date{.year = 2026, .month = 8, .day = 8},
                      .time = Time{.hour = 13, .minute = 14, .second = 15}}));
  EXPECT_EQ(root.find("offset").as_date_time(),
            (DateTime{.date = Date{.year = 2026, .month = 8, .day = 8},
                      .time = Time{.hour = 13, .minute = 14, .second = 15},
                      .utc_offset_minutes = -420}));

  const Value items = root.find("items");
  ASSERT_EQ(items.type(), ValueType::ARRAY);
  ASSERT_EQ(items.size(), 2U);
  EXPECT_EQ(items.at(0).as_string(), std::string_view{"one"});
  EXPECT_EQ(items.at(1).as_string(), std::string_view{"two"});
  EXPECT_FALSE(items.at(2));

  const Value nested = root.find("nested");
  ASSERT_EQ(nested.type(), ValueType::TABLE);
  EXPECT_EQ(nested.find("answer").as_integer(), 42);
  EXPECT_EQ(root.find("nested.answer").as_integer(), 42);
  EXPECT_EQ(loaded.find("nested.answer").as_integer(), 42);
  EXPECT_FALSE(nested.find("missing"));
  EXPECT_EQ(root.find("title").location().line, 2U);
  EXPECT_TRUE(root.find("title").location().source.ends_with("types.toml"));
}

TEST_F(ConfigTest, UserFileOverridesPrimaryScalarsAndNestedTableFields) {
  write_config(primary_root_, "input_keys.toml", R"toml(
name = "system"
untouched = 7
modes = ["system"]

[display]
color = "blue"
width = 80

[[mapping]]
name = "system"
)toml");
  write_config(user_override_root_, "input_keys.toml", R"toml(
name = "user"
modes = ["user"]

[display]
width = 120

[[mapping]]
name = "user"
)toml");

  const Config config{primary_root_, user_override_root_};
  const LoadResult loaded = config.load("input_keys.toml");
  ASSERT_EQ(loaded.status, Status::OK);
  const Value root = loaded.document.root();
  EXPECT_EQ(root.find("name").as_string(), std::string_view{"user"});
  EXPECT_EQ(root.find("untouched").as_integer(), 7);
  ASSERT_EQ(root.find("modes").size(), 1U);
  EXPECT_EQ(root.find("modes").at(0).as_string(), std::string_view{"user"});
  EXPECT_TRUE(root.find("name").location().source.find("user_overrides") !=
              std::string_view::npos);

  const Value display = root.find("display");
  EXPECT_EQ(display.find("color").as_string(), std::string_view{"blue"});
  EXPECT_EQ(display.find("width").as_integer(), 120);

  const Value mappings = root.find("mapping");
  ASSERT_EQ(mappings.type(), ValueType::ARRAY);
  ASSERT_EQ(mappings.size(), 2U);
  EXPECT_EQ(mappings.at(0).find("name").as_string(),
            std::string_view{"system"});
  EXPECT_EQ(mappings.at(1).find("name").as_string(), std::string_view{"user"});
}

TEST_F(ConfigTest, EitherRootCanSupplyTheOnlyExistingConfiguration) {
  write_config(primary_root_, "primary_only.toml", "value = \"primary\"\n");
  write_config(user_override_root_, "override_only.toml",
               "value = \"override\"\n");
  const Config config{primary_root_, user_override_root_};

  const LoadResult primary = config.load("primary_only.toml");
  ASSERT_EQ(primary.status, Status::OK);
  EXPECT_EQ(primary.document.root().find("value").as_string(),
            std::string_view{"primary"});

  const LoadResult user_override = config.load("override_only.toml");
  ASSERT_EQ(user_override.status, Status::OK);
  EXPECT_EQ(user_override.document.root().find("value").as_string(),
            std::string_view{"override"});
}

TEST_F(ConfigTest, MissingFilesAndMissingOverrideDirectoryAreOptional) {
  const Config config{primary_root_, user_override_root_};
  EXPECT_EQ(config.load("missing.toml").status, Status::NOT_FOUND);

  std::error_code error;
  ASSERT_EQ(std::filesystem::remove_all(user_override_root_, error), 1U);
  ASSERT_FALSE(error);
  write_config(primary_root_, "present.toml", "value = 1\n");
  EXPECT_EQ(config.load("present.toml").status, Status::OK);
}

TEST_F(ConfigTest, MalformedOverrideDoesNotSilentlyReturnPrimaryValues) {
  write_config(primary_root_, "settings.toml", "value = 1\n");
  write_config(user_override_root_, "settings.toml", "value = [\n");
  const Config config{primary_root_, user_override_root_};

  const LoadResult loaded = config.load("settings.toml");
  EXPECT_EQ(loaded.status, Status::PARSE_ERROR);
  EXPECT_FALSE(loaded.document);
}

TEST_F(ConfigTest, RejectsPathsThatCanBypassEitherConfiguredRoot) {
  const Config config{primary_root_, user_override_root_};
  EXPECT_EQ(config.load("").status, Status::INVALID_PATH);
  EXPECT_EQ(config.load(".").status, Status::INVALID_PATH);
  EXPECT_EQ(config.load("../outside.toml").status, Status::INVALID_PATH);
  EXPECT_EQ(config.load(primary_root_ / "absolute.toml").status,
            Status::INVALID_PATH);
}

TEST_F(ConfigTest, RejectsDirectoriesAndAcceptsFileSymlinks) {
  const Config config{primary_root_, user_override_root_};
  std::error_code error;
  ASSERT_TRUE(
      std::filesystem::create_directory(primary_root_ / "directory", error));
  ASSERT_FALSE(error);
  EXPECT_EQ(config.load("directory").status, Status::NOT_REGULAR_FILE);

  const std::filesystem::path outside = test_root_ / "outside.toml";
  write_config(test_root_, "outside.toml", "value = 1\n");
  error.clear();
  std::filesystem::create_symlink(outside, primary_root_ / "link.toml", error);
  if (error) {
    GTEST_SKIP() << "symlinks are not available: " << error.message();
  }
  const LoadResult linked = config.load("link.toml");
  ASSERT_EQ(linked.status, Status::OK);
  EXPECT_EQ(linked.find("value").as_integer(), 1);
}

TEST_F(ConfigTest, ValuesKeepTheirDocumentAliveIndependently) {
  write_config(primary_root_, "lifetime.toml", "name = \"retained\"\n");
  Value retained;
  {
    const Config config{primary_root_, user_override_root_};
    const LoadResult loaded = config.load("lifetime.toml");
    ASSERT_EQ(loaded.status, Status::OK);
    retained = loaded.document.root().find("name");
  }
  EXPECT_EQ(retained.as_string(), std::string_view{"retained"});
}

TEST(ConfigParseTest, ParsesMemoryWithoutExposingTheTomlImplementation) {
  const LoadResult parsed =
      Config::parse("name = \"memory\"\n", "built_in_defaults.toml");
  ASSERT_EQ(parsed.status, Status::OK);
  const Value name = parsed.document.root().find("name");
  EXPECT_EQ(name.as_string(), std::string_view{"memory"});
  EXPECT_EQ(name.location().source, "built_in_defaults.toml");

  const LoadResult malformed = Config::parse("name = [");
  EXPECT_EQ(malformed.status, Status::PARSE_ERROR);
  EXPECT_FALSE(malformed.document);
}

TEST(ConfigValueTest, InvalidAccessAndTypeMismatchesAreNonThrowing) {
  const Value missing;
  EXPECT_EQ(missing.type(), ValueType::NONE);
  EXPECT_EQ(missing.size(), 0U);
  EXPECT_FALSE(missing.at(0));
  EXPECT_FALSE(missing.find("field"));
  EXPECT_FALSE(missing.find_key("field"));
  EXPECT_TRUE(missing.key_at(0).empty());
  EXPECT_FALSE(missing.value_at(0));
  EXPECT_FALSE(missing.as_string());
  EXPECT_FALSE(missing.as_integer());
  EXPECT_FALSE(missing.as_float());
  EXPECT_FALSE(missing.as_boolean());
  EXPECT_FALSE(missing.as_date());
  EXPECT_FALSE(missing.as_time());
  EXPECT_FALSE(missing.as_date_time());

  const LoadResult parsed = Config::parse("value = 42\n");
  ASSERT_EQ(parsed.status, Status::OK);
  const Value value = parsed.document.root().find("value");
  EXPECT_FALSE(value.as_string());
  EXPECT_FALSE(value.at(0));
  EXPECT_FALSE(value.find("nested"));
}

TEST(ConfigValueTest, DottedPathsAndLiteralDottedKeysRemainDistinct) {
  const LoadResult parsed = Config::parse(R"toml(
"top.middle.low.value" = 23

[top.middle.low]
value = 17
)toml");
  ASSERT_EQ(parsed.status, Status::OK);
  const Value root = parsed.document.root();
  EXPECT_EQ(root.find("top.middle.low.value").as_integer(), 17);
  EXPECT_EQ(root.find_key("top.middle.low.value").as_integer(), 23);
  EXPECT_FALSE(root.find(""));
  EXPECT_FALSE(root.find("top..low"));
  EXPECT_FALSE(root.find("top.middle."));
  EXPECT_FALSE(root.find("top.middle.missing"));
}

TEST(ConfigStatusTest, EveryStatusHasStableHumanReadableText) {
  constexpr Status statuses[] = {
      Status::OK,           Status::NOT_FOUND,        Status::INVALID_ROOT,
      Status::INVALID_PATH, Status::NOT_REGULAR_FILE, Status::IO_ERROR,
      Status::PARSE_ERROR,
  };
  for (const Status status : statuses) {
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status), "unknown configuration status");
  }
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown configuration status");
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::NOT_FOUND));
}

}  // namespace
}  // namespace puc::config
