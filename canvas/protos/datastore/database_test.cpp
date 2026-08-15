#include "canvas/protos/datastore/database.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace puc::canvas::datastore {
namespace {

using namespace std::chrono_literals;

std::filesystem::path test_database() {
  return std::filesystem::temp_directory_path() /
         ("puc-database-test-" +
          std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
          ".sqlite");
}

}  // namespace

TEST(DatabaseTest, SerializesCompleteLogicalOperationsAcrossTransactions) {
  const std::filesystem::path path = test_database();
  std::filesystem::remove(path);
  static constexpr std::array migrations{
      Migration{.version = 1U,
                .sql     = "CREATE TABLE sample(value INTEGER) STRICT;"},
  };
  static constexpr std::array sets{
      MigrationSet{.datastore = "sample", .migrations = migrations},
  };
  Database database;
  ASSERT_EQ(database.initialize(path, sets), Status::OK);

  std::mutex mutex;
  std::condition_variable changed;
  bool transaction_open     = false;
  bool allow_commit         = false;
  bool second_started       = false;
  bool second_completed     = false;
  Status transaction_status = Status::OK;
  Status second_status      = Status::OK;
  std::thread transaction{[&] {
    const Database::Operation operation = database.acquire();
    transaction_status                  = database.begin_immediate();
    if (is_ok(transaction_status)) {
      transaction_status = database.execute("INSERT INTO sample VALUES(1);");
    }
    {
      const std::lock_guard lock(mutex);
      transaction_open = true;
    }
    changed.notify_all();
    {
      std::unique_lock lock(mutex);
      changed.wait(lock, [&] { return allow_commit; });
    }
    if (is_ok(transaction_status)) transaction_status = database.commit();
  }};
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return transaction_open; }));
  }
  std::thread second{[&] {
    {
      const std::lock_guard lock(mutex);
      second_started = true;
    }
    changed.notify_all();
    second_status = database.execute("INSERT INTO sample VALUES(2);");
    {
      const std::lock_guard lock(mutex);
      second_completed = true;
    }
    changed.notify_all();
  }};

  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return second_started; }));
    EXPECT_FALSE(second_completed);
  }
  {
    const std::lock_guard lock(mutex);
    allow_commit = true;
  }
  changed.notify_all();
  transaction.join();
  second.join();
  EXPECT_EQ(transaction_status, Status::OK);
  EXPECT_EQ(second_status, Status::OK);
  EXPECT_TRUE(second_completed);

  Statement count;
  ASSERT_EQ(database.prepare("SELECT count(*) FROM sample;", count),
            Status::OK);
  ASSERT_EQ(count.step(), Status::OK);
  EXPECT_EQ(count.integer(0), 2);
  database.close();
  std::filesystem::remove(path);
}

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
