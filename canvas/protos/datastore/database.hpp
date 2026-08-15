#pragma once

/**
 * @file database.hpp
 * @brief SQLite ownership and one-time migrations.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

#include "canvas/protos/datastore/status.hpp"

struct sqlite3;
struct sqlite3_stmt;

namespace puc::canvas::datastore {

/** One immutable, checksummed SQL migration in a datastore-owned sequence. */
struct Migration {
  std::uint32_t version = 0U; /**< Contiguous one-based datastore version. */
  std::string_view sql;       /**< SQL executed atomically at most once. */
};

/** Complete ordered migration catalog declared by one datastore. */
struct MigrationSet {
  std::string_view datastore; /**< Stable migration-table namespace. */
  std::span<const Migration> migrations; /**< Contiguous immutable entries. */
};

/** Move-only prepared-statement owner tied to its originating Database. */
class Statement final {
 public:
  /** Construct an empty statement suitable as a prepare() output. */
  Statement() noexcept                   = default;
  Statement(const Statement&)            = delete;
  Statement& operator=(const Statement&) = delete;
  /** Move ownership from another statement. */
  Statement(Statement&& other) noexcept;
  /** Finalize current state and move ownership from another statement. */
  Statement& operator=(Statement&& other) noexcept;
  /** Finalize the owned SQLite statement, if any. */
  ~Statement();

  /** Bind UTF-8 text at a one-based SQLite parameter index. */
  Status bind(std::int32_t index, std::string_view value) noexcept;
  /** Bind arbitrary bytes at a one-based SQLite parameter index. */
  Status bind(std::int32_t index, std::span<const std::uint8_t> value) noexcept;
  /** Bind one signed integer at a one-based SQLite parameter index. */
  Status bind(std::int32_t index, std::int64_t value) noexcept;
  /** Bind SQL NULL at a one-based SQLite parameter index. */
  Status bind_null(std::int32_t index) noexcept;

  /** Return OK for a row, NOT_FOUND for completion, or SQL_ERROR. */
  Status step() noexcept;
  /** Reset execution and clear every bound parameter. */
  Status reset() noexcept;

  /** Read one integer column from the current row. */
  std::int64_t integer(std::int32_t column) const noexcept;
  /** Copy one text column from the current row. */
  std::string text(std::int32_t column) const;
  /** Copy one blob column from the current row. */
  std::string blob(std::int32_t column) const;
  /** Return whether one column in the current row is SQL NULL. */
  bool is_null(std::int32_t column) const noexcept;

 private:
  friend class Database;
  explicit Statement(sqlite3_stmt* statement) noexcept
      : statement_(statement) {}
  sqlite3_stmt* statement_ = nullptr; /**< Exclusively owned native handle. */
};

/** Own one SQLite connection and enforce one-time initialization migrations. */
class Database final {
 public:
  /** Move-only guard serializing one complete logical database operation. */
  class Operation final {
   public:
    Operation(const Operation&)            = delete;
    Operation& operator=(const Operation&) = delete;
    /** Transfer ownership of the database operation lock. */
    Operation(Operation&&) noexcept = default;
    /** Transfer ownership of the database operation lock. */
    Operation& operator=(Operation&&) noexcept = default;

   private:
    friend class Database;
    explicit Operation(std::recursive_mutex& mutex) : lock_(mutex) {}
    std::unique_lock<std::recursive_mutex> lock_; /**< Held operation lock. */
  };

  /** Construct a closed database wrapper. */
  Database() noexcept                  = default;
  Database(const Database&)            = delete;
  Database& operator=(const Database&) = delete;
  /** Move an open or closed connection from another wrapper. */
  Database(Database&& other) noexcept;
  /** Close current state and move a connection from another wrapper. */
  Database& operator=(Database&& other) noexcept;
  /** Close the owned SQLite connection. */
  ~Database();

  /** Open once and apply every migration before becoming ready. */
  Status initialize(const std::filesystem::path& path,
                    std::span<const MigrationSet> migration_sets);

  /** Close the connection and clear its initialized state. */
  void close() noexcept;
  /** Return whether initialize() completed and the connection remains open. */
  bool ready() const noexcept;

  /**
   * Serialize one complete datastore method or explicit transaction.
   *
   * Datastore wrappers retain this guard from their first SQLite call through
   * statement destruction and commit or rollback. Individual Database methods
   * lock recursively so nested calls remain safe.
   */
  Operation acquire() const { return Operation{operation_mutex_}; }

  /** Execute one or more SQL statements without result rows. */
  Status execute(std::string_view sql) noexcept;
  /** Compile SQL into a move-only output statement. */
  Status prepare(std::string_view sql, Statement& output) noexcept;
  /** Begin a serialized SQLite write transaction. */
  Status begin_immediate() noexcept;
  /** Commit the active transaction. */
  Status commit() noexcept;
  /** Best-effort rollback of the active transaction. */
  void rollback() noexcept;

  /** Return a copy of the most recently captured SQLite diagnostic. */
  std::string last_error() const;

 private:
  /** Apply and record every missing migration in one write transaction. */
  Status apply_migrations(std::span<const MigrationSet> migration_sets);
  /** Capture SQLite's current error with an operation prefix. */
  void record_error(std::string_view prefix) noexcept;

  sqlite3* database_ = nullptr; /**< Exclusively owned native connection. */
  bool initialized_  = false; /**< Whether migrations completed successfully. */
  std::string last_error_;    /**< Most recently captured native diagnostic. */
  mutable std::recursive_mutex
      operation_mutex_; /**< Serializes connection-level logical operations. */
};

}  // namespace puc::canvas::datastore
