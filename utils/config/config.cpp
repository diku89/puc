/**
 * @file config.cpp
 * @brief Root-scoped TOML loading and parser-independent value views.
 */

#include "utils/config/config.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "tomlc17.h"
#include "utils/logger/logger.hpp"

/** @cond CONFIG_LOGGER_MODULE */
LOGGER_MODULE("Config");
/** @endcond */

namespace puc::config {

namespace detail {

/** Own one tomlc17 result while keeping its representation private. */
class DocumentStorage {
 public:
  /** Take ownership of one successful tomlc17 parse result. */
  explicit DocumentStorage(toml_result_t result) noexcept : result_(result) {}

  DocumentStorage(const DocumentStorage&)            = delete;
  DocumentStorage& operator=(const DocumentStorage&) = delete;

  ~DocumentStorage() { toml_free(result_); }

  /** Return the parsed root datum as an opaque pointer. */
  const void* root() const noexcept { return &result_.toptab; }

  /** Return the owned result for an internal deep merge. */
  const toml_result_t& result() const noexcept { return result_; }

 private:
  toml_result_t result_;
};

}  // namespace detail

namespace {

/** Internal parser result retaining storage without exposing tomlc17. */
struct ParsedResult {
  Status status = Status::NOT_FOUND;
  std::shared_ptr<const detail::DocumentStorage> storage;
};

/** Convert an opaque public view back to the private C parser datum. */
const toml_datum_t* datum(const void* value) noexcept {
  return static_cast<const toml_datum_t*>(value);
}

/** Test whether a requested path is cleanly relative to the configured root. */
bool valid_relative_path(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() ||
      path.has_root_directory()) {
    return false;
  }
  for (const std::filesystem::path& component : path) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

/** Log a filesystem error without disclosing file contents. */
void log_filesystem_error(std::string_view operation,
                          const std::filesystem::path& path,
                          const std::error_code& error) {
  Logger<ERROR> << "Could not " << operation << " configuration path '"
                << path.string() << "': " << error.message();
}

/** Convert a successful C parser result into shared immutable storage. */
ParsedResult own_result(toml_result_t result) {
  if (!result.ok) {
    const std::string message{result.errmsg};
    toml_free(result);
    Logger<ERROR> << message;
    return ParsedResult{.status = Status::PARSE_ERROR};
  }
  return ParsedResult{
      .status  = Status::OK,
      .storage = std::make_shared<detail::DocumentStorage>(result),
  };
}

/** Load one physical file below one lexically validated root. */
ParsedResult load_one(const std::filesystem::path& root,
                      const std::filesystem::path& relative_path) {
  std::error_code error;
  const bool root_is_directory = std::filesystem::is_directory(root, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return ParsedResult{.status = Status::NOT_FOUND};
    }
    log_filesystem_error("inspect", root, error);
    return ParsedResult{.status = Status::INVALID_ROOT};
  }
  if (!root_is_directory) {
    Logger<ERROR> << "Configuration root '" << root.string()
                  << "' is not a directory";
    return ParsedResult{.status = Status::INVALID_ROOT};
  }

  const std::filesystem::path candidate = root / relative_path;
  error.clear();
  const std::filesystem::file_status candidate_status =
      std::filesystem::status(candidate, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      return ParsedResult{.status = Status::NOT_FOUND};
    }
    log_filesystem_error("inspect", candidate, error);
    return ParsedResult{.status = Status::IO_ERROR};
  }
  if (!std::filesystem::exists(candidate_status)) {
    Logger<DEBUG> << "Optional configuration '" << candidate.string()
                  << "' was not found";
    return ParsedResult{.status = Status::NOT_FOUND};
  }
  if (!std::filesystem::is_regular_file(candidate_status)) {
    Logger<ERROR> << "Configuration path '" << candidate.string()
                  << "' is not a regular file";
    return ParsedResult{.status = Status::NOT_REGULAR_FILE};
  }

  const std::string path_string = candidate.string();
  errno                         = 0;
  FILE* file                    = std::fopen(path_string.c_str(), "rb");
  if (file == nullptr) {
    Logger<ERROR> << "Could not open configuration '" << path_string
                  << "': " << std::strerror(errno);
    return ParsedResult{.status = Status::IO_ERROR};
  }

  toml_result_t parsed   = toml_parse_file_named(file, path_string.c_str());
  const int close_result = std::fclose(file);
  if (close_result != 0) {
    Logger<ERROR> << "Could not close configuration '" << path_string
                  << "': " << std::strerror(errno);
    toml_free(parsed);
    return ParsedResult{.status = Status::IO_ERROR};
  }
  return own_result(parsed);
}

}  // namespace

Value::Value(std::shared_ptr<const detail::DocumentStorage> storage,
             const void* datum) noexcept
    : storage_(std::move(storage)), datum_(datum) {}

ValueType Value::type() const noexcept {
  if (datum_ == nullptr) {
    return ValueType::NONE;
  }
  switch (datum(datum_)->type) {
    case TOML_UNKNOWN:
      return ValueType::NONE;
    case TOML_STRING:
      return ValueType::STRING;
    case TOML_INT64:
      return ValueType::INTEGER;
    case TOML_FP64:
      return ValueType::FLOAT;
    case TOML_BOOLEAN:
      return ValueType::BOOLEAN;
    case TOML_DATE:
      return ValueType::DATE;
    case TOML_TIME:
      return ValueType::TIME;
    case TOML_DATETIME:
      return ValueType::DATE_TIME;
    case TOML_DATETIMETZ:
      return ValueType::OFFSET_DATE_TIME;
    case TOML_ARRAY:
      return ValueType::ARRAY;
    case TOML_TABLE:
      return ValueType::TABLE;
  }
  return ValueType::NONE;
}

SourceLocation Value::location() const noexcept {
  if (datum_ == nullptr) {
    return {};
  }
  const toml_datum_t* value = datum(datum_);
  return SourceLocation{
      .source = value->source == nullptr ? std::string_view{}
                                         : std::string_view{value->source},
      .line = value->lineno < 0 ? 0U : static_cast<std::size_t>(value->lineno),
      .column = value->colno < 0 ? 0U : static_cast<std::size_t>(value->colno),
  };
}

std::size_t Value::size() const noexcept {
  if (datum_ == nullptr) {
    return 0U;
  }
  const toml_datum_t* value = datum(datum_);
  if (value->type == TOML_ARRAY) {
    return value->u.arr.size < 0 ? 0U
                                 : static_cast<std::size_t>(value->u.arr.size);
  }
  if (value->type == TOML_TABLE) {
    return value->u.tab.size < 0 ? 0U
                                 : static_cast<std::size_t>(value->u.tab.size);
  }
  return 0U;
}

Value Value::at(std::size_t index) const noexcept {
  if (datum_ == nullptr) {
    return {};
  }
  const toml_datum_t* value = datum(datum_);
  if (value->type != TOML_ARRAY || index >= size()) {
    return {};
  }
  return Value{storage_, &value->u.arr.elem[index]};
}

Value Value::find(std::string_view dotted_path) const noexcept {
  if (dotted_path.empty()) {
    return {};
  }
  Value current      = *this;
  std::size_t offset = 0;
  while (offset < dotted_path.size()) {
    const std::size_t separator = dotted_path.find('.', offset);
    const std::size_t end =
        separator == std::string_view::npos ? dotted_path.size() : separator;
    if (end == offset) {
      return {};
    }
    current = current.find_key(dotted_path.substr(offset, end - offset));
    if (!current) {
      return {};
    }
    if (separator == std::string_view::npos) {
      return current;
    }
    offset = separator + 1U;
    if (offset == dotted_path.size()) {
      return {};
    }
  }
  return {};
}

Value Value::find_key(std::string_view key) const noexcept {
  if (datum_ == nullptr) {
    return {};
  }
  const toml_datum_t* value = datum(datum_);
  if (value->type != TOML_TABLE) {
    return {};
  }
  for (std::size_t index = 0; index < size(); ++index) {
    const int length = value->u.tab.len[index];
    if (length >= 0 && static_cast<std::size_t>(length) == key.size() &&
        (key.empty() ||
         std::memcmp(value->u.tab.key[index], key.data(), key.size()) == 0)) {
      return Value{storage_, &value->u.tab.value[index]};
    }
  }
  return {};
}

std::string_view Value::key_at(std::size_t index) const noexcept {
  if (datum_ == nullptr) {
    return {};
  }
  const toml_datum_t* value = datum(datum_);
  if (value->type != TOML_TABLE || index >= size() ||
      value->u.tab.len[index] < 0) {
    return {};
  }
  return std::string_view{value->u.tab.key[index],
                          static_cast<std::size_t>(value->u.tab.len[index])};
}

Value Value::value_at(std::size_t index) const noexcept {
  if (datum_ == nullptr) {
    return {};
  }
  const toml_datum_t* value = datum(datum_);
  if (value->type != TOML_TABLE || index >= size()) {
    return {};
  }
  return Value{storage_, &value->u.tab.value[index]};
}

std::optional<std::string_view> Value::as_string() const noexcept {
  if (datum_ == nullptr || datum(datum_)->type != TOML_STRING ||
      datum(datum_)->u.str.ptr == nullptr || datum(datum_)->u.str.len < 0) {
    return std::nullopt;
  }
  return std::string_view{datum(datum_)->u.str.ptr,
                          static_cast<std::size_t>(datum(datum_)->u.str.len)};
}

std::optional<std::int64_t> Value::as_integer() const noexcept {
  return datum_ != nullptr && datum(datum_)->type == TOML_INT64
             ? std::optional<std::int64_t>{datum(datum_)->u.int64}
             : std::nullopt;
}

std::optional<double> Value::as_float() const noexcept {
  return datum_ != nullptr && datum(datum_)->type == TOML_FP64
             ? std::optional<double>{datum(datum_)->u.fp64}
             : std::nullopt;
}

std::optional<bool> Value::as_boolean() const noexcept {
  return datum_ != nullptr && datum(datum_)->type == TOML_BOOLEAN
             ? std::optional<bool>{datum(datum_)->u.boolean}
             : std::nullopt;
}

std::optional<Date> Value::as_date() const noexcept {
  if (datum_ == nullptr || datum(datum_)->type != TOML_DATE) {
    return std::nullopt;
  }
  const toml_datum_t* value = datum(datum_);
  return Date{
      .year  = value->u.ts.year,
      .month = value->u.ts.month,
      .day   = value->u.ts.day,
  };
}

std::optional<Time> Value::as_time() const noexcept {
  if (datum_ == nullptr || datum(datum_)->type != TOML_TIME) {
    return std::nullopt;
  }
  const toml_datum_t* value = datum(datum_);
  return Time{
      .hour        = value->u.ts.hour,
      .minute      = value->u.ts.minute,
      .second      = value->u.ts.second,
      .microsecond = value->u.ts.usec,
  };
}

std::optional<DateTime> Value::as_date_time() const noexcept {
  if (datum_ == nullptr || (datum(datum_)->type != TOML_DATETIME &&
                            datum(datum_)->type != TOML_DATETIMETZ)) {
    return std::nullopt;
  }
  const toml_datum_t* value = datum(datum_);
  return DateTime{
      .date =
          Date{
              .year  = value->u.ts.year,
              .month = value->u.ts.month,
              .day   = value->u.ts.day,
          },
      .time =
          Time{
              .hour        = value->u.ts.hour,
              .minute      = value->u.ts.minute,
              .second      = value->u.ts.second,
              .microsecond = value->u.ts.usec,
          },
      .utc_offset_minutes = value->type == TOML_DATETIMETZ
                                ? std::optional<int>{value->u.ts.tz}
                                : std::nullopt,
  };
}

Document::Document(
    std::shared_ptr<const detail::DocumentStorage> storage) noexcept
    : storage_(std::move(storage)) {}

Value Document::root() const noexcept {
  return storage_ == nullptr ? Value{} : Value{storage_, storage_->root()};
}

Value Document::find(std::string_view dotted_path) const noexcept {
  return root().find(dotted_path);
}

Value Document::find_key(std::string_view key) const noexcept {
  return root().find_key(key);
}

Config::Config(std::filesystem::path primary_root,
               std::filesystem::path user_overrides_root)
    : primary_root_(std::move(primary_root).lexically_normal()),
      user_overrides_root_(std::move(user_overrides_root).lexically_normal()) {}

LoadResult Config::load(const std::filesystem::path& relative_path) const {
  if (!valid_relative_path(relative_path)) {
    Logger<ERROR> << "Rejected configuration path '" << relative_path.string()
                  << "' because it is not a clean relative path";
    return LoadResult{.status = Status::INVALID_PATH};
  }

  ParsedResult primary = load_one(primary_root_, relative_path);
  if (primary.status != Status::OK && primary.status != Status::NOT_FOUND) {
    return LoadResult{.status = primary.status};
  }
  ParsedResult user_override = load_one(user_overrides_root_, relative_path);
  if (user_override.status != Status::OK &&
      user_override.status != Status::NOT_FOUND) {
    return LoadResult{.status = user_override.status};
  }

  if (primary.status == Status::NOT_FOUND &&
      user_override.status == Status::NOT_FOUND) {
    return LoadResult{.status = Status::NOT_FOUND};
  }
  if (primary.status == Status::NOT_FOUND) {
    return LoadResult{
        .status   = Status::OK,
        .document = Document{std::move(user_override.storage)},
    };
  }
  if (user_override.status == Status::NOT_FOUND) {
    return LoadResult{
        .status   = Status::OK,
        .document = Document{std::move(primary.storage)},
    };
  }

  toml_result_t merged =
      toml_merge(&primary.storage->result(), &user_override.storage->result());
  ParsedResult merged_result = own_result(merged);
  return LoadResult{
      .status   = merged_result.status,
      .document = is_ok(merged_result.status)
                      ? Document{std::move(merged_result.storage)}
                      : Document{},
  };
}

LoadResult Config::parse(std::string_view toml, std::string_view source_name) {
  if (toml.size() > static_cast<std::size_t>(INT_MAX)) {
    Logger<ERROR> << "Rejected oversized in-memory configuration '"
                  << source_name << "'";
    return LoadResult{.status = Status::PARSE_ERROR};
  }
  const std::string owned_toml{toml};
  const std::string source{source_name};
  ParsedResult parsed = own_result(toml_parse_named(
      owned_toml.c_str(), static_cast<int>(owned_toml.size()), source.c_str()));
  return LoadResult{
      .status   = parsed.status,
      .document = is_ok(parsed.status) ? Document{std::move(parsed.storage)}
                                       : Document{},
  };
}

}  // namespace puc::config
