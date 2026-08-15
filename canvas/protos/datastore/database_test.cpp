#include "canvas/protos/datastore/database.hpp"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>

namespace puc::canvas::datastore {
namespace {

std::filesystem::path test_database() {
  return std::filesystem::temp_directory_path() /
         ("puc-database-test-" +
          std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
          ".sqlite");
}

}  // namespace

TEST(DatabaseTest, AppliesEachMigrationOnce) {
  const std::filesystem::path path = test_database();
  std::filesystem::remove(path);
  static constexpr std::array migrations{
      Migration{.version = 1U,
                .sql     = "CREATE TABLE sample(value INTEGER) STRICT;"},
  };
  static constexpr std::array sets{
      MigrationSet{.datastore = "sample", .migrations = migrations},
  };
  {
    Database database;
    ASSERT_EQ(database.initialize(path, sets), Status::OK);
  }
  {
    Database database;
    ASSERT_EQ(database.initialize(path, sets), Status::OK);
    Statement count;
    ASSERT_EQ(database.prepare("SELECT count(*) FROM puc_migrations;", count),
              Status::OK);
    ASSERT_EQ(count.step(), Status::OK);
    EXPECT_EQ(count.integer(0), 1);
  }
  std::filesystem::remove(path);
}

TEST(DatabaseTest, RejectsChangedAndEmptyMigrationCatalogs) {
  const std::filesystem::path path = test_database();
  std::filesystem::remove(path);
  static constexpr std::array original{
      Migration{.version = 1U,
                .sql     = "CREATE TABLE original(value INTEGER) STRICT;"},
  };
  static constexpr std::array original_set{
      MigrationSet{.datastore = "sample", .migrations = original},
  };
  Database first;
  ASSERT_EQ(first.initialize(path, original_set), Status::OK);
  first.close();

  static constexpr std::array changed{
      Migration{.version = 1U,
                .sql     = "CREATE TABLE changed(value INTEGER) STRICT;"},
  };
  static constexpr std::array changed_set{
      MigrationSet{.datastore = "sample", .migrations = changed},
  };
  Database second;
  EXPECT_EQ(second.initialize(path, changed_set), Status::MIGRATION_CHANGED);

  const std::array<Migration, 0U> empty{};
  const std::array empty_set{
      MigrationSet{.datastore = "empty", .migrations = empty},
  };
  Database third;
  EXPECT_EQ(third.initialize(path, empty_set), Status::INVALID_ARGUMENT);

  const std::array duplicate_sets{original_set[0], original_set[0]};
  Database fourth;
  EXPECT_EQ(fourth.initialize(path, duplicate_sets), Status::INVALID_ARGUMENT);
  std::filesystem::remove(path);
}

}  // namespace puc::canvas::datastore
