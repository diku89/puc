/** @file database.cpp @brief SQLite ownership and migration implementation. */

#include "canvas/protos/datastore/database.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "utils/hash/sha256.hpp"

namespace puc::canvas::datastore {
namespace {

constexpr std::string_view kCreateMigrations = R"sql(
CREATE TABLE IF NOT EXISTS puc_migrations (
  datastore TEXT NOT NULL,
  version INTEGER NOT NULL CHECK(version > 0),
  checksum BLOB NOT NULL CHECK(length(checksum) = 32),
  applied_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY(datastore, version)
) STRICT;
)sql";

Status sqlite_status(int status) noexcept {
  return status == SQLITE_OK ? Status::OK : Status::SQL_ERROR;
}

}  // namespace

Statement::Statement(Statement&& other) noexcept
    : statement_(std::exchange(other.statement_, nullptr)) {}

Statement& Statement::operator=(Statement&& other) noexcept {
  if (this != &other) {
    if (statement_ != nullptr) sqlite3_finalize(statement_);
    statement_ = std::exchange(other.statement_, nullptr);
  }
  return *this;
}

Statement::~Statement() {
  if (statement_ != nullptr) sqlite3_finalize(statement_);
}

Status Statement::bind(std::int32_t index, std::string_view value) noexcept {
  return sqlite_status(sqlite3_bind_text(statement_, index, value.data(),
                                         static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT));
}

Status Statement::bind(std::int32_t index,
                       std::span<const std::uint8_t> value) noexcept {
  return sqlite_status(sqlite3_bind_blob(statement_, index, value.data(),
                                         static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT));
}

Status Statement::bind(std::int32_t index, std::int64_t value) noexcept {
  return sqlite_status(sqlite3_bind_int64(statement_, index, value));
}

Status Statement::bind_null(std::int32_t index) noexcept {
  return sqlite_status(sqlite3_bind_null(statement_, index));
}

Status Statement::step() noexcept {
  const int result = sqlite3_step(statement_);
  if (result == SQLITE_ROW) return Status::OK;
  if (result == SQLITE_DONE) return Status::NOT_FOUND;
  return result == SQLITE_CONSTRAINT ? Status::ALREADY_EXISTS
                                     : Status::SQL_ERROR;
}

Status Statement::reset() noexcept {
  const int reset_status = sqlite3_reset(statement_);
  const int clear_status = sqlite3_clear_bindings(statement_);
  return reset_status == SQLITE_OK && clear_status == SQLITE_OK
             ? Status::OK
             : Status::SQL_ERROR;
}

std::int64_t Statement::integer(std::int32_t column) const noexcept {
  return sqlite3_column_int64(statement_, column);
}

std::string Statement::text(std::int32_t column) const {
  const auto* data = sqlite3_column_text(statement_, column);
  const int size   = sqlite3_column_bytes(statement_, column);
  return data == nullptr ? std::string{}
                         : std::string{reinterpret_cast<const char*>(data),
                                       static_cast<std::size_t>(size)};
}

std::string Statement::blob(std::int32_t column) const {
  const void* data = sqlite3_column_blob(statement_, column);
  const int size   = sqlite3_column_bytes(statement_, column);
  return data == nullptr ? std::string{}
                         : std::string{static_cast<const char*>(data),
                                       static_cast<std::size_t>(size)};
}

bool Statement::is_null(std::int32_t column) const noexcept {
  return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

Database::Database(Database&& other) noexcept {
  const std::lock_guard lock(other.operation_mutex_);
  database_    = std::exchange(other.database_, nullptr);
  initialized_ = std::exchange(other.initialized_, false);
  last_error_  = std::move(other.last_error_);
}

Database& Database::operator=(Database&& other) noexcept {
  if (this != &other) {
    const std::scoped_lock lock(operation_mutex_, other.operation_mutex_);
    if (database_ != nullptr) sqlite3_close_v2(database_);
    database_    = std::exchange(other.database_, nullptr);
    initialized_ = std::exchange(other.initialized_, false);
    last_error_  = std::move(other.last_error_);
  }
  return *this;
}

Database::~Database() { close(); }

Status Database::initialize(const std::filesystem::path& path,
                            std::span<const MigrationSet> migration_sets) {
  const std::lock_guard lock(operation_mutex_);
  if (database_ != nullptr || initialized_) return Status::INVALID_STATE;
  if (path.empty() || migration_sets.empty()) return Status::INVALID_ARGUMENT;
  for (std::size_t set_index = 0U; set_index < migration_sets.size();
       ++set_index) {
    const MigrationSet& set = migration_sets[set_index];
    if (set.datastore.empty() || set.migrations.empty()) {
      return Status::INVALID_ARGUMENT;
    }
    for (std::size_t earlier = 0U; earlier < set_index; ++earlier) {
      if (migration_sets[earlier].datastore == set.datastore) {
        return Status::INVALID_ARGUMENT;
      }
    }
    for (std::size_t index = 0U; index < set.migrations.size(); ++index) {
      if (set.migrations[index].version != index + 1U ||
          set.migrations[index].sql.empty()) {
        return Status::INVALID_ARGUMENT;
      }
    }
  }

  const int flags =
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(path.c_str(), &database_, flags, nullptr) != SQLITE_OK) {
    record_error("open");
    close();
    return Status::OPEN_FAILED;
  }
  sqlite3_busy_timeout(database_, 5000);
  if (!is_ok(execute("PRAGMA foreign_keys = ON;"))) {
    close();
    return Status::SQL_ERROR;
  }
  const Status migration_status = apply_migrations(migration_sets);
  if (!is_ok(migration_status)) {
    close();
    return migration_status;
  }
  initialized_ = true;
  return Status::OK;
}

void Database::close() noexcept {
  const std::lock_guard lock(operation_mutex_);
  if (database_ != nullptr) sqlite3_close_v2(database_);
  database_    = nullptr;
  initialized_ = false;
}

Status Database::execute(std::string_view sql) noexcept {
  const std::lock_guard lock(operation_mutex_);
  if (database_ == nullptr) return Status::INVALID_STATE;
  std::string owned{sql};
  char* error = nullptr;
  const int result =
      sqlite3_exec(database_, owned.c_str(), nullptr, nullptr, &error);
  if (result == SQLITE_OK) return Status::OK;
  last_error_ = error == nullptr ? sqlite3_errmsg(database_) : error;
  sqlite3_free(error);
  return Status::SQL_ERROR;
}

Status Database::prepare(std::string_view sql, Statement& output) noexcept {
  const std::lock_guard lock(operation_mutex_);
  output = Statement{};
  if (database_ == nullptr) return Status::INVALID_STATE;
  sqlite3_stmt* statement = nullptr;
  const int result        = sqlite3_prepare_v2(
      database_, sql.data(), static_cast<int>(sql.size()), &statement, nullptr);
  if (result != SQLITE_OK) {
    record_error("prepare");
    return Status::SQL_ERROR;
  }
  output = Statement{statement};
  return Status::OK;
}

Status Database::begin_immediate() noexcept {
  return execute("BEGIN IMMEDIATE;");
}

Status Database::commit() noexcept { return execute("COMMIT;"); }

void Database::rollback() noexcept { static_cast<void>(execute("ROLLBACK;")); }

bool Database::ready() const noexcept {
  const std::lock_guard lock(operation_mutex_);
  return database_ != nullptr && initialized_;
}

std::string Database::last_error() const {
  const std::lock_guard lock(operation_mutex_);
  return last_error_;
}

Status Database::apply_migrations(
    std::span<const MigrationSet> migration_sets) {
  if (!is_ok(begin_immediate())) return Status::MIGRATION_ERROR;
  if (!is_ok(execute(kCreateMigrations))) {
    rollback();
    return Status::MIGRATION_ERROR;
  }

  Statement lookup;
  Statement record;
  if (!is_ok(prepare("SELECT checksum FROM puc_migrations "
                     "WHERE datastore = ?1 AND version = ?2;",
                     lookup)) ||
      !is_ok(prepare("INSERT INTO puc_migrations(datastore, version, checksum) "
                     "VALUES(?1, ?2, ?3);",
                     record))) {
    rollback();
    return Status::MIGRATION_ERROR;
  }

  for (const MigrationSet& set : migration_sets) {
    for (const Migration& migration : set.migrations) {
      const hashing::Hash256 checksum = hashing::sha256(migration.sql);
      if (!is_ok(lookup.bind(1, set.datastore)) ||
          !is_ok(
              lookup.bind(2, static_cast<std::int64_t>(migration.version)))) {
        rollback();
        return Status::MIGRATION_ERROR;
      }
      const Status found = lookup.step();
      if (found == Status::OK) {
        const std::string stored = lookup.blob(0);
        static_cast<void>(lookup.reset());
        if (stored.size() != checksum.bytes.size() ||
            std::memcmp(stored.data(), checksum.bytes.data(),
                        checksum.bytes.size()) != 0) {
          rollback();
          return Status::MIGRATION_CHANGED;
        }
        continue;
      }
      static_cast<void>(lookup.reset());
      if (found != Status::NOT_FOUND || !is_ok(execute(migration.sql)) ||
          !is_ok(record.bind(1, set.datastore)) ||
          !is_ok(
              record.bind(2, static_cast<std::int64_t>(migration.version))) ||
          !is_ok(record.bind(3, checksum.bytes)) ||
          record.step() != Status::NOT_FOUND) {
        rollback();
        return Status::MIGRATION_ERROR;
      }
      static_cast<void>(record.reset());
    }
  }
  if (!is_ok(commit())) {
    rollback();
    return Status::MIGRATION_ERROR;
  }
  return Status::OK;
}

void Database::record_error(std::string_view prefix) noexcept {
  last_error_ = std::string{prefix};
  last_error_.append(": ");
  last_error_.append(database_ == nullptr ? "no database"
                                          : sqlite3_errmsg(database_));
}

}  // namespace puc::canvas::datastore
