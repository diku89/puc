#pragma once

/**
 * @file config.hpp
 * @brief Root-scoped TOML configuration loading and read-only value access.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace puc::config {

namespace detail {
class DocumentStorage;
}  // namespace detail

/** Result of locating and parsing one configuration file. */
enum class Status {
  OK,               /**< The configuration was loaded and parsed. */
  NOT_FOUND,        /**< The requested optional configuration does not exist. */
  INVALID_ROOT,     /**< The configured root is not a readable directory. */
  INVALID_PATH,     /**< A path was empty, absolute, or escaped the root. */
  NOT_REGULAR_FILE, /**< The requested path does not name a regular file. */
  IO_ERROR,         /**< The file could not be opened, read, or closed. */
  PARSE_ERROR,      /**< The file is not valid TOML. */
};

/** Return whether a configuration operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for a configuration status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::NOT_FOUND:
      return "configuration was not found";
    case Status::INVALID_ROOT:
      return "configuration root is not a directory";
    case Status::INVALID_PATH:
      return "configuration path is invalid";
    case Status::NOT_REGULAR_FILE:
      return "configuration is not a regular file";
    case Status::IO_ERROR:
      return "configuration could not be read";
    case Status::PARSE_ERROR:
      return "configuration is not valid TOML";
  }
  return "unknown configuration status";
}

/** Public type of a TOML value. */
enum class ValueType {
  NONE,             /**< Missing value. */
  STRING,           /**< UTF-8 string. */
  INTEGER,          /**< Signed 64-bit integer. */
  FLOAT,            /**< Double-precision floating-point number. */
  BOOLEAN,          /**< Boolean value. */
  DATE,             /**< Local calendar date. */
  TIME,             /**< Local wall-clock time. */
  DATE_TIME,        /**< Local date and time. */
  OFFSET_DATE_TIME, /**< Date and time carrying a UTC offset. */
  ARRAY,            /**< Ordered array of values. */
  TABLE,            /**< String-keyed table. */
};

/** Source position attached to a parsed value. */
struct SourceLocation {
  std::string_view source; /**< Configuration name supplied to the parser. */
  std::size_t line   = 0;  /**< One-based line, or zero when unavailable. */
  std::size_t column = 0;  /**< One-based column, or zero when unavailable. */

  /** Compare source name and coordinates. */
  constexpr bool operator==(const SourceLocation&) const noexcept = default;
};

/** TOML local-date value. */
struct Date {
  int year  = 0; /**< Four-digit year. */
  int month = 0; /**< One-based month. */
  int day   = 0; /**< One-based day of month. */

  /** Compare all date components. */
  constexpr bool operator==(const Date&) const noexcept = default;
};

/** TOML local-time value. */
struct Time {
  int hour        = 0; /**< Hour in the range 0 through 23. */
  int minute      = 0; /**< Minute in the range 0 through 59. */
  int second      = 0; /**< Second in the range 0 through 60. */
  int microsecond = 0; /**< Fractional second in microseconds. */

  /** Compare all time components. */
  constexpr bool operator==(const Time&) const noexcept = default;
};

/** TOML local or offset date-time value. */
struct DateTime {
  Date date; /**< Calendar portion. */
  Time time; /**< Wall-clock portion. */
  std::optional<int>
      utc_offset_minutes; /**< UTC offset, absent for a local date-time. */

  /** Compare date, time, and optional offset. */
  constexpr bool operator==(const DateTime&) const noexcept = default;
};

/**
 * Cheap, owning view of one value in an immutable parsed document.
 *
 * Copies share ownership of the parsed document, so a Value remains valid
 * after the Document or LoadResult from which it came has been destroyed.
 */
class Value {
 public:
  /** Construct a missing value. */
  Value() noexcept = default;

  /** Return whether this view names an existing value. */
  explicit operator bool() const noexcept { return datum_ != nullptr; }

  /** Return the public TOML type, or ValueType::NONE when missing. */
  ValueType type() const noexcept;

  /** Return the value's source name and coordinates. */
  SourceLocation location() const noexcept;

  /** Return an array or table's entry count, and zero for scalar values. */
  std::size_t size() const noexcept;

  /** Return an array element, or a missing Value for an invalid access. */
  Value at(std::size_t index) const noexcept;

  /**
   * Traverse a dot-separated table path.
   *
   * For example, `find("terminal.input.escape_timeout")` performs three
   * allocation-free exact-key lookups. Empty path components are rejected.
   */
  Value find(std::string_view dotted_path) const noexcept;

  /**
   * Return one exact table field without interpreting dots in its key.
   *
   * This is useful for TOML quoted keys such as `"server.name"`.
   */
  Value find_key(std::string_view key) const noexcept;

  /** Return a table key by insertion order, or an empty view when invalid. */
  std::string_view key_at(std::size_t index) const noexcept;

  /** Return a table value by insertion order, or a missing Value. */
  Value value_at(std::size_t index) const noexcept;

  /** Return the string value only when the type is STRING. */
  std::optional<std::string_view> as_string() const noexcept;

  /** Return the integer value only when the type is INTEGER. */
  std::optional<std::int64_t> as_integer() const noexcept;

  /** Return the floating-point value only when the type is FLOAT. */
  std::optional<double> as_float() const noexcept;

  /** Return the Boolean value only when the type is BOOLEAN. */
  std::optional<bool> as_boolean() const noexcept;

  /** Return the date value only when the type is DATE. */
  std::optional<Date> as_date() const noexcept;

  /** Return the time value only when the type is TIME. */
  std::optional<Time> as_time() const noexcept;

  /** Return either kind of date-time, preserving an optional UTC offset. */
  std::optional<DateTime> as_date_time() const noexcept;

 private:
  friend class Document;

  Value(std::shared_ptr<const detail::DocumentStorage> storage,
        const void* datum) noexcept;

  std::shared_ptr<const detail::DocumentStorage>
      storage_; /**< Keeps all borrowed strings and nodes alive. */
  const void* datum_ = nullptr; /**< Internal parser datum owned by storage_. */
};

/** Immutable TOML document returned by a successful load. */
class Document {
 public:
  /** Construct an empty document. */
  Document() noexcept = default;

  /** Return whether this document owns a parsed root table. */
  explicit operator bool() const noexcept {
    return static_cast<bool>(storage_);
  }

  /** Return the root table, or a missing Value for an empty document. */
  Value root() const noexcept;

  /** Traverse a dot-separated path beginning at the root table. */
  Value find(std::string_view dotted_path) const noexcept;

  /** Find one exact root key without interpreting dots. */
  Value find_key(std::string_view key) const noexcept;

 private:
  friend class Config;

  explicit Document(
      std::shared_ptr<const detail::DocumentStorage> storage) noexcept;

  std::shared_ptr<const detail::DocumentStorage> storage_;
};

/** Value returned by Config::load() and Config::parse(). */
struct LoadResult {
  Status status = Status::NOT_FOUND; /**< Load or parse result. */
  Document document; /**< Parsed document, populated only on success. */

  /** Return whether the result contains a parsed document. */
  explicit operator bool() const noexcept { return is_ok(status); }

  /** Traverse a dot-separated path in the successfully loaded document. */
  Value find(std::string_view dotted_path) const noexcept {
    return document.find(dotted_path);
  }

  /** Find one exact root key without interpreting dots. */
  Value find_key(std::string_view key) const noexcept {
    return document.find_key(key);
  }
};

/**
 * Convention-oriented loader scoped to primary and user-override directories.
 *
 * Each subsystem owns a fixed relative filename and asks this object to load
 * it. The same relative path is loaded from the primary root and, when it
 * exists, the user-overrides root. Absolute paths and lexical parent traversal
 * are rejected for both roots. Ordinary file symlinks remain usable, including
 * the symlinks through which Bazel exposes data dependencies in runfiles.
 */
class Config {
 public:
  /** Initialize a loader with primary and optional user-override roots. */
  Config(std::filesystem::path primary_root,
         std::filesystem::path user_overrides_root);

  /** Return the lexically normalized primary configuration root. */
  const std::filesystem::path& primary_root() const noexcept {
    return primary_root_;
  }

  /** Return the lexically normalized user-overrides root. */
  const std::filesystem::path& user_overrides_root() const noexcept {
    return user_overrides_root_;
  }

  /**
   * Load and merge one relative TOML file from the two configured roots.
   *
   * The primary file supplies system defaults. When the corresponding file is
   * present under the user-overrides root, its scalar and table values take
   * precedence. Arrays of tables retain primary entries followed by override
   * entries, allowing domain-specific loaders to apply later entries last. If
   * neither file exists, this returns Status::NOT_FOUND without logging an
   * error. The supplied path must be a non-empty relative path with no `.` or
   * `..` components.
   */
  LoadResult load(const std::filesystem::path& relative_path) const;

  /** Parse an in-memory TOML document through the same public abstraction. */
  static LoadResult parse(std::string_view toml,
                          std::string_view source_name = "<memory>");

 private:
  std::filesystem::path primary_root_; /**< System-default file boundary. */
  std::filesystem::path
      user_overrides_root_; /**< User-override file boundary. */
};

}  // namespace puc::config
