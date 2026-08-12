#pragma once

/**
 * @file properties.hpp
 * @brief Application-owned immutable documents and mutable properties.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace puc::properties {

namespace detail {
class DocumentHandle;
class ValueHandle;
}  // namespace detail

/** Result of loading, querying, mutating, or reloading properties. */
enum class Status {
  OK,                 /**< The requested operation completed. */
  NOT_FOUND,          /**< A source or property does not exist. */
  INVALID_ROOT,       /**< A configured root is not a readable directory. */
  INVALID_PATH,       /**< A source or property path is invalid. */
  NOT_REGULAR_FILE,   /**< A source does not name a regular file. */
  IO_ERROR,           /**< A source could not be read. */
  PARSE_ERROR,        /**< A source is not valid TOML. */
  DUPLICATE_SOURCE,   /**< A logical source name is already registered. */
  DUPLICATE_PROPERTY, /**< Two sources expose the same full property path. */
  IMMUTABLE_PROPERTY, /**< A caller attempted to change immutable data. */
  UNSUPPORTED_VALUE,  /**< A mutable default is not a supported scalar. */
  TYPE_MISMATCH,      /**< A new value does not match its declared type. */
  INVALID_VALUE,      /**< Text cannot be parsed as the declared type. */
};

/** Return whether a properties operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable human-readable text for a properties result. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::NOT_FOUND:
      return "property or source was not found";
    case Status::INVALID_ROOT:
      return "properties root is not a directory";
    case Status::INVALID_PATH:
      return "property or source path is invalid";
    case Status::NOT_REGULAR_FILE:
      return "properties source is not a regular file";
    case Status::IO_ERROR:
      return "properties source could not be read";
    case Status::PARSE_ERROR:
      return "properties source is not valid TOML";
    case Status::DUPLICATE_SOURCE:
      return "properties source name is already registered";
    case Status::DUPLICATE_PROPERTY:
      return "property path is exposed by more than one source";
    case Status::IMMUTABLE_PROPERTY:
      return "property is immutable";
    case Status::UNSUPPORTED_VALUE:
      return "mutable property default is not a supported scalar";
    case Status::TYPE_MISMATCH:
      return "property value has the wrong type";
    case Status::INVALID_VALUE:
      return "property value text is invalid";
  }
  return "unknown properties status";
}

/** Public type of one TOML-backed value. */
enum class ValueType {
  NONE,
  STRING,
  INTEGER,
  FLOAT,
  BOOLEAN,
  DATE,
  TIME,
  DATE_TIME,
  OFFSET_DATE_TIME,
  ARRAY,
  TABLE,
};

/** Owned source position attached to a parsed value. */
struct SourceLocation {
  std::string source;     /**< Source name supplied by the loader. */
  std::size_t line   = 0; /**< One-based line, or zero when unavailable. */
  std::size_t column = 0; /**< One-based column, or zero when unavailable. */

  /** Compare source and coordinates. */
  bool operator==(const SourceLocation&) const = default;
};

/** TOML local-date value. */
struct Date {
  int year  = 0;
  int month = 0;
  int day   = 0;

  /** Compare calendar fields. */
  constexpr bool operator==(const Date&) const noexcept = default;
};

/** TOML local-time value. */
struct Time {
  int hour        = 0;
  int minute      = 0;
  int second      = 0;
  int microsecond = 0;

  /** Compare wall-clock fields. */
  constexpr bool operator==(const Time&) const noexcept = default;
};

/** TOML local or offset date-time value. */
struct DateTime {
  Date date;
  Time time;
  std::optional<int> utc_offset_minutes;

  /** Compare date, time, and offset. */
  constexpr bool operator==(const DateTime&) const noexcept = default;
};

/** Immutable, owning view of one value in a loaded document. */
class Value {
 public:
  /** Construct a missing value. */
  Value() noexcept = default;

  /** Return whether this view names an existing value. */
  explicit operator bool() const noexcept;

  /** Return the value type, or ValueType::NONE when missing. */
  ValueType type() const noexcept;

  /** Return owned source coordinates for this value. */
  SourceLocation location() const;

  /** Return an array or table's entry count, otherwise zero. */
  std::size_t size() const noexcept;

  /** Return an array element, or a missing value. */
  Value at(std::size_t index) const;

  /** Traverse a dot-separated table path. */
  Value find(std::string_view dotted_path) const;

  /** Return one exact table field without interpreting dots. */
  Value find_key(std::string_view key) const;

  /** Return a table key by insertion order, or an empty view. */
  std::string_view key_at(std::size_t index) const noexcept;

  /** Return a table value by insertion order, or a missing value. */
  Value value_at(std::size_t index) const;

  /** Return the string only when this value is a string. */
  std::optional<std::string_view> as_string() const noexcept;

  /** Return the integer only when this value is an integer. */
  std::optional<std::int64_t> as_integer() const noexcept;

  /** Return the number only when this value is a float. */
  std::optional<double> as_float() const noexcept;

  /** Return the Boolean only when this value is a Boolean. */
  std::optional<bool> as_boolean() const noexcept;

  /** Return the date only when this value is a date. */
  std::optional<Date> as_date() const noexcept;

  /** Return the time only when this value is a time. */
  std::optional<Time> as_time() const noexcept;

  /** Return either date-time form while preserving its optional offset. */
  std::optional<DateTime> as_date_time() const noexcept;

 private:
  friend class Document;
  friend class Properties;

  explicit Value(std::shared_ptr<const detail::ValueHandle> handle) noexcept;

  std::shared_ptr<const detail::ValueHandle> handle_;
};

/** Immutable TOML document retained by the properties service. */
class Document {
 public:
  /** Construct an empty document. */
  Document() noexcept = default;

  /** Return whether this document owns a parsed root table. */
  explicit operator bool() const noexcept;

  /** Return the root table, or a missing value. */
  Value root() const;

  /** Traverse a dot-separated path from the root table. */
  Value find(std::string_view dotted_path) const;

  /** Find one exact root key without interpreting dots. */
  Value find_key(std::string_view key) const;

 private:
  friend class Properties;

  explicit Document(
      std::shared_ptr<const detail::DocumentHandle> handle) noexcept;

  std::shared_ptr<const detail::DocumentHandle> handle_;
};

/** Result of registering or retrieving one immutable source. */
struct LoadResult {
  Status status = Status::NOT_FOUND;
  Document document;

  /** Return whether this result contains a document. */
  explicit operator bool() const noexcept { return is_ok(status); }

  /** Traverse a path in the loaded document. */
  Value find(std::string_view path) const { return document.find(path); }
};

/** Whether a property is fixed by its source or mutable at runtime. */
enum class Mutability {
  IMMUTABLE,
  USER_MUTABLE,
};

/** Scalar types accepted by the runtime mutation API. */
using Scalar =
    std::variant<std::string, std::int64_t, double, bool, Date, Time, DateTime>;

/** One stable snapshot returned by get() or list(). */
struct Property {
  std::string name;
  Scalar value;
  Mutability mutability = Mutability::IMMUTABLE;
  bool user_modified    = false;

  /** Compare the complete snapshot. */
  bool operator==(const Property&) const = default;
};

/** Return the TOML-compatible type represented by a scalar. */
ValueType scalar_type(const Scalar& value) noexcept;

/** Render one scalar in stable TOML-compatible text. */
std::string scalar_text(const Scalar& value);

/**
 * Sole application gateway to TOML-backed configuration and runtime settings.
 *
 * Immutable sources retain parsed documents for domain-specific consumers.
 * Mutable sources contribute scalar defaults to the same global dotted-key
 * directory. Runtime changes are kept in memory and survive reload; reload
 * refreshes immutable documents and unmodified mutable defaults
 * transactionally.
 */
class Properties final {
 public:
  /** Construct the sole low-level Config owner for these roots. */
  Properties(std::filesystem::path primary_root,
             std::filesystem::path user_overrides_root);

  Properties(const Properties&)            = delete;
  Properties& operator=(const Properties&) = delete;
  Properties(Properties&&) noexcept;
  Properties& operator=(Properties&&) noexcept;

  /** Release cached sources before the private loader. */
  ~Properties();

  /** Register and cache one immutable TOML document. */
  LoadResult load_immutable(std::string source_name,
                            std::filesystem::path relative_path);

  /** Register scalar runtime properties initialized from one TOML document. */
  Status load_mutable_defaults(std::string source_name,
                               std::filesystem::path relative_path);

  /** Return a previously registered immutable document. */
  LoadResult document(std::string_view source_name) const;

  /** Return one scalar by its fully qualified dotted path. */
  Status get(std::string_view name, Property& output) const;

  /** Return every scalar whose full name starts with prefix. */
  std::vector<Property> list(std::string_view prefix = {}) const;

  /** Parse and assign one user-mutable scalar while preserving its type. */
  Status set(std::string_view name, std::string_view value);

  /** Reload every registered source as one transaction. */
  Status reload();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace puc::properties
