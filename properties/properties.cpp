/**
 * @file properties.cpp
 * @brief Application properties ownership and mutation implementation.
 */

#include "properties/properties.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "utils/config/config.hpp"

namespace puc::properties {

namespace detail {

/** Type-erased boundary retaining one private config value view. */
class ValueHandle final {
 public:
  explicit ValueHandle(config::Value value) : value_(std::move(value)) {}

  config::Value value_;
};

/** Type-erased boundary retaining one private config document. */
class DocumentHandle final {
 public:
  explicit DocumentHandle(config::Document document)
      : document_(std::move(document)) {}

  config::Document document_;
};

}  // namespace detail

namespace {

/** Translate low-level loader failures without exposing its public types. */
Status properties_status(config::Status status) noexcept {
  switch (status) {
    case config::Status::OK:
      return Status::OK;
    case config::Status::NOT_FOUND:
      return Status::NOT_FOUND;
    case config::Status::INVALID_ROOT:
      return Status::INVALID_ROOT;
    case config::Status::INVALID_PATH:
      return Status::INVALID_PATH;
    case config::Status::NOT_REGULAR_FILE:
      return Status::NOT_REGULAR_FILE;
    case config::Status::IO_ERROR:
      return Status::IO_ERROR;
    case config::Status::PARSE_ERROR:
      return Status::PARSE_ERROR;
  }
  return Status::IO_ERROR;
}

/** Translate a private TOML type to the properties vocabulary. */
ValueType value_type(config::ValueType type) noexcept {
  switch (type) {
    case config::ValueType::NONE:
      return ValueType::NONE;
    case config::ValueType::STRING:
      return ValueType::STRING;
    case config::ValueType::INTEGER:
      return ValueType::INTEGER;
    case config::ValueType::FLOAT:
      return ValueType::FLOAT;
    case config::ValueType::BOOLEAN:
      return ValueType::BOOLEAN;
    case config::ValueType::DATE:
      return ValueType::DATE;
    case config::ValueType::TIME:
      return ValueType::TIME;
    case config::ValueType::DATE_TIME:
      return ValueType::DATE_TIME;
    case config::ValueType::OFFSET_DATE_TIME:
      return ValueType::OFFSET_DATE_TIME;
    case config::ValueType::ARRAY:
      return ValueType::ARRAY;
    case config::ValueType::TABLE:
      return ValueType::TABLE;
  }
  return ValueType::NONE;
}

/** Validate a namespace used as the prefix of command-visible properties. */
bool valid_source_name(std::string_view name) noexcept {
  if (name.empty() || name.front() == '.' || name.back() == '.') {
    return false;
  }
  bool previous_dot = false;
  for (const unsigned char byte : name) {
    const bool valid = (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '_' ||
                       byte == '-' || byte == '.';
    if (!valid || (byte == '.' && previous_dot)) {
      return false;
    }
    previous_dot = byte == '.';
  }
  return true;
}

/** Join one source namespace and source-local scalar path. */
std::string qualified_name(std::string_view source, std::string_view local) {
  if (local.empty()) {
    return std::string{source};
  }
  std::string output;
  output.reserve(source.size() + 1U + local.size());
  output.append(source);
  output.push_back('.');
  output.append(local);
  return output;
}

/** Convert one low-level scalar into an independently owned value. */
std::optional<Scalar> scalar_value(const config::Value& value) {
  switch (value.type()) {
    case config::ValueType::STRING:
      if (const auto scalar = value.as_string()) {
        return Scalar{std::string{*scalar}};
      }
      break;
    case config::ValueType::INTEGER:
      if (const auto scalar = value.as_integer()) {
        return Scalar{*scalar};
      }
      break;
    case config::ValueType::FLOAT:
      if (const auto scalar = value.as_float()) {
        return Scalar{*scalar};
      }
      break;
    case config::ValueType::BOOLEAN:
      if (const auto scalar = value.as_boolean()) {
        return Scalar{*scalar};
      }
      break;
    case config::ValueType::DATE:
      if (const auto scalar = value.as_date()) {
        return Scalar{Date{
            .year = scalar->year, .month = scalar->month, .day = scalar->day}};
      }
      break;
    case config::ValueType::TIME:
      if (const auto scalar = value.as_time()) {
        return Scalar{Time{.hour        = scalar->hour,
                           .minute      = scalar->minute,
                           .second      = scalar->second,
                           .microsecond = scalar->microsecond}};
      }
      break;
    case config::ValueType::DATE_TIME:
    case config::ValueType::OFFSET_DATE_TIME:
      if (const auto scalar = value.as_date_time()) {
        return Scalar{DateTime{
            .date               = Date{.year  = scalar->date.year,
                                       .month = scalar->date.month,
                                       .day   = scalar->date.day},
            .time               = Time{.hour        = scalar->time.hour,
                                       .minute      = scalar->time.minute,
                                       .second      = scalar->time.second,
                                       .microsecond = scalar->time.microsecond},
            .utc_offset_minutes = scalar->utc_offset_minutes,
        }};
      }
      break;
    case config::ValueType::NONE:
    case config::ValueType::ARRAY:
    case config::ValueType::TABLE:
      break;
  }
  return std::nullopt;
}

/** Collect scalar leaves using dotted table paths and indexed arrays. */
Status collect_scalars(const config::Value& value, std::string path,
                       bool mutable_source,
                       std::map<std::string, Scalar>& output) {
  if (value.type() == config::ValueType::TABLE) {
    for (std::size_t index = 0U; index < value.size(); ++index) {
      const std::string_view key = value.key_at(index);
      std::string child          = path;
      if (!child.empty()) {
        child.push_back('.');
      }
      child.append(key);
      const Status status = collect_scalars(
          value.value_at(index), std::move(child), mutable_source, output);
      if (!is_ok(status)) {
        return status;
      }
    }
    return Status::OK;
  }
  if (value.type() == config::ValueType::ARRAY) {
    if (mutable_source) {
      return Status::UNSUPPORTED_VALUE;
    }
    for (std::size_t index = 0U; index < value.size(); ++index) {
      std::string child = path + '[' + std::to_string(index) + ']';
      const Status status =
          collect_scalars(value.at(index), std::move(child), false, output);
      if (!is_ok(status)) {
        return status;
      }
    }
    return Status::OK;
  }
  const std::optional<Scalar> scalar = scalar_value(value);
  if (!scalar.has_value() || path.empty()) {
    return Status::UNSUPPORTED_VALUE;
  }
  if (!output.emplace(std::move(path), *scalar).second) {
    return Status::DUPLICATE_PROPERTY;
  }
  return Status::OK;
}

/** Return whether two scalars carry the same declared type. */
bool same_type(const Scalar& left, const Scalar& right) noexcept {
  return left.index() == right.index();
}

/** Escape one string as a compact TOML basic string. */
std::string quoted_string(std::string_view value) {
  std::string output{"\""};
  for (const char byte : value) {
    switch (byte) {
      case '\\':
        output.append("\\\\");
        break;
      case '"':
        output.append("\\\"");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        output.push_back(byte);
        break;
    }
  }
  output.push_back('"');
  return output;
}

/** Format a two-digit date/time component. */
void append_two_digits(std::ostringstream& output, int value) {
  output << std::setw(2) << std::setfill('0') << value;
}

/** Parse a complete decimal integer. */
bool parse_integer(std::string_view text, std::int64_t& output) noexcept {
  if (text.empty()) {
    return false;
  }
  bool negative      = false;
  std::size_t offset = 0U;
  if (text.front() == '+' || text.front() == '-') {
    negative = text.front() == '-';
    offset   = 1U;
  }
  int base = 10;
  if (text.size() >= offset + 2U && text[offset] == '0') {
    switch (text[offset + 1U]) {
      case 'x':
      case 'X':
        base = 16;
        break;
      case 'o':
      case 'O':
        base = 8;
        break;
      case 'b':
      case 'B':
        base = 2;
        break;
      default:
        break;
    }
  }
  if (base != 10) {
    offset += 2U;
    if (offset == text.size()) {
      return false;
    }
    std::uint64_t magnitude = 0U;
    const char* beginning   = text.data() + offset;
    const char* end         = text.data() + text.size();
    const auto result       = std::from_chars(beginning, end, magnitude, base);
    if (result.ec != std::errc{} || result.ptr != end) {
      return false;
    }
    constexpr std::uint64_t kNegativeLimit = std::uint64_t{1} << 63U;
    const std::uint64_t limit =
        negative ? kNegativeLimit
                 : static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max());
    if (magnitude > limit) {
      return false;
    }
    if (negative) {
      output = magnitude == kNegativeLimit
                   ? std::numeric_limits<std::int64_t>::min()
                   : -static_cast<std::int64_t>(magnitude);
    } else {
      output = static_cast<std::int64_t>(magnitude);
    }
    return true;
  }
  const char* beginning = text.data();
  const char* end       = text.data() + text.size();
  const auto result     = std::from_chars(beginning, end, output, 10);
  return result.ec == std::errc{} && result.ptr == end;
}

/** Parse a complete floating-point value. */
bool parse_float(std::string_view text, double& output) noexcept {
  if (text.empty()) {
    return false;
  }
  const char* beginning = text.data();
  const char* end       = text.data() + text.size();
  const auto result =
      std::from_chars(beginning, end, output, std::chars_format::general);
  return result.ec == std::errc{} && result.ptr == end;
}

}  // namespace

Value::operator bool() const noexcept {
  return handle_ != nullptr && static_cast<bool>(handle_->value_);
}

ValueType Value::type() const noexcept {
  return handle_ == nullptr ? ValueType::NONE
                            : value_type(handle_->value_.type());
}

SourceLocation Value::location() const {
  if (handle_ == nullptr) {
    return {};
  }
  const config::SourceLocation location = handle_->value_.location();
  return SourceLocation{.source = std::string{location.source},
                        .line   = location.line,
                        .column = location.column};
}

std::size_t Value::size() const noexcept {
  return handle_ == nullptr ? 0U : handle_->value_.size();
}

Value Value::at(std::size_t index) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->value_.at(index))};
}

Value Value::find(std::string_view dotted_path) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->value_.find(dotted_path))};
}

Value Value::find_key(std::string_view key) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->value_.find_key(key))};
}

std::string_view Value::key_at(std::size_t index) const noexcept {
  return handle_ == nullptr ? std::string_view{}
                            : handle_->value_.key_at(index);
}

Value Value::value_at(std::size_t index) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->value_.value_at(index))};
}

std::optional<std::string_view> Value::as_string() const noexcept {
  return handle_ == nullptr ? std::nullopt : handle_->value_.as_string();
}

std::optional<std::int64_t> Value::as_integer() const noexcept {
  return handle_ == nullptr ? std::nullopt : handle_->value_.as_integer();
}

std::optional<double> Value::as_float() const noexcept {
  return handle_ == nullptr ? std::nullopt : handle_->value_.as_float();
}

std::optional<bool> Value::as_boolean() const noexcept {
  return handle_ == nullptr ? std::nullopt : handle_->value_.as_boolean();
}

std::optional<Date> Value::as_date() const noexcept {
  if (handle_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<config::Date> value = handle_->value_.as_date();
  return value.has_value() ? std::optional<Date>{Date{.year  = value->year,
                                                      .month = value->month,
                                                      .day   = value->day}}
                           : std::nullopt;
}

std::optional<Time> Value::as_time() const noexcept {
  if (handle_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<config::Time> value = handle_->value_.as_time();
  return value.has_value()
             ? std::optional<Time>{Time{.hour        = value->hour,
                                        .minute      = value->minute,
                                        .second      = value->second,
                                        .microsecond = value->microsecond}}
             : std::nullopt;
}

std::optional<DateTime> Value::as_date_time() const noexcept {
  if (handle_ == nullptr) {
    return std::nullopt;
  }
  const std::optional<config::DateTime> value = handle_->value_.as_date_time();
  return value.has_value()
             ? std::optional<DateTime>{DateTime{
                   .date               = Date{.year  = value->date.year,
                                              .month = value->date.month,
                                              .day   = value->date.day},
                   .time               = Time{.hour        = value->time.hour,
                                              .minute      = value->time.minute,
                                              .second      = value->time.second,
                                              .microsecond = value->time.microsecond},
                   .utc_offset_minutes = value->utc_offset_minutes,
               }}
             : std::nullopt;
}

Value::Value(std::shared_ptr<const detail::ValueHandle> handle) noexcept
    : handle_(std::move(handle)) {}

Document::operator bool() const noexcept {
  return handle_ != nullptr && static_cast<bool>(handle_->document_);
}

Value Document::root() const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->document_.root())};
}

Value Document::find(std::string_view dotted_path) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->document_.find(dotted_path))};
}

Value Document::find_key(std::string_view key) const {
  return handle_ == nullptr ? Value{}
                            : Value{std::make_shared<detail::ValueHandle>(
                                  handle_->document_.find_key(key))};
}

Document::Document(
    std::shared_ptr<const detail::DocumentHandle> handle) noexcept
    : handle_(std::move(handle)) {}

ValueType scalar_type(const Scalar& value) noexcept {
  return std::visit(
      []<typename Type>(const Type& scalar) {
        if constexpr (std::is_same_v<Type, std::string>) {
          return ValueType::STRING;
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
          return ValueType::INTEGER;
        } else if constexpr (std::is_same_v<Type, double>) {
          return ValueType::FLOAT;
        } else if constexpr (std::is_same_v<Type, bool>) {
          return ValueType::BOOLEAN;
        } else if constexpr (std::is_same_v<Type, Date>) {
          return ValueType::DATE;
        } else if constexpr (std::is_same_v<Type, Time>) {
          return ValueType::TIME;
        } else {
          return scalar.utc_offset_minutes.has_value()
                     ? ValueType::OFFSET_DATE_TIME
                     : ValueType::DATE_TIME;
        }
      },
      value);
}

std::string scalar_text(const Scalar& value) {
  return std::visit(
      []<typename Type>(const Type& scalar) -> std::string {
        if constexpr (std::is_same_v<Type, std::string>) {
          return quoted_string(scalar);
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
          return std::to_string(scalar);
        } else if constexpr (std::is_same_v<Type, double>) {
          std::ostringstream output;
          output << std::setprecision(std::numeric_limits<double>::max_digits10)
                 << scalar;
          return output.str();
        } else if constexpr (std::is_same_v<Type, bool>) {
          return scalar ? "true" : "false";
        } else if constexpr (std::is_same_v<Type, Date>) {
          std::ostringstream output;
          output << std::setw(4) << std::setfill('0') << scalar.year << '-';
          append_two_digits(output, scalar.month);
          output << '-';
          append_two_digits(output, scalar.day);
          return output.str();
        } else if constexpr (std::is_same_v<Type, Time>) {
          std::ostringstream output;
          append_two_digits(output, scalar.hour);
          output << ':';
          append_two_digits(output, scalar.minute);
          output << ':';
          append_two_digits(output, scalar.second);
          if (scalar.microsecond != 0) {
            output << '.' << std::setw(6) << std::setfill('0')
                   << scalar.microsecond;
          }
          return output.str();
        } else {
          std::string output = scalar_text(Scalar{scalar.date}) + 'T' +
                               scalar_text(Scalar{scalar.time});
          if (scalar.utc_offset_minutes.has_value()) {
            const int minutes = *scalar.utc_offset_minutes;
            if (minutes == 0) {
              output.push_back('Z');
            } else {
              const int absolute = minutes < 0 ? -minutes : minutes;
              std::ostringstream offset;
              offset << (minutes < 0 ? '-' : '+');
              append_two_digits(offset, absolute / 60);
              offset << ':';
              append_two_digits(offset, absolute % 60);
              output.append(offset.str());
            }
          }
          return output;
        }
      },
      value);
}

class Properties::Impl final {
 public:
  struct Roots {
    std::filesystem::path primary;
    std::filesystem::path user;
  };

  struct Source {
    std::string name;
    std::filesystem::path relative_path;
    Mutability mutability = Mutability::IMMUTABLE;
    std::shared_ptr<const detail::DocumentHandle> document;
    std::map<std::string, Scalar> values;
    std::set<std::string> user_modified;
  };

  struct IndexedProperty {
    Property property;
    std::string source;
    std::string local_name;
  };

  Impl(std::filesystem::path primary_root,
       std::filesystem::path user_overrides_root)
      : Impl(normalize_roots(std::move(primary_root),
                             std::move(user_overrides_root))) {}

  explicit Impl(Roots roots)
      : config_(std::move(roots.primary), std::move(roots.user)) {}

  /** Resolve omitted roots to the process working directory and no overlay. */
  static Roots normalize_roots(std::filesystem::path primary,
                               std::filesystem::path user) {
    if (primary.empty()) {
      std::error_code error;
      primary = std::filesystem::current_path(error);
      if (error) {
        primary = ".";
      }
    }
    if (user.empty()) {
      user = primary / ".puc-no-user-overrides";
    }
    return Roots{.primary = std::move(primary), .user = std::move(user)};
  }

  Status read_source(std::string name, std::filesystem::path path,
                     Mutability mutability, Source& output) const {
    const config::LoadResult loaded = config_.load(path);
    const Status status             = properties_status(loaded.status);
    if (!is_ok(status)) {
      return status;
    }
    std::map<std::string, Scalar> values;
    const Status collect_status =
        collect_scalars(loaded.document.root(), {},
                        mutability == Mutability::USER_MUTABLE, values);
    if (!is_ok(collect_status)) {
      return collect_status;
    }
    output = Source{
        .name          = std::move(name),
        .relative_path = std::move(path),
        .mutability    = mutability,
        .document = std::make_shared<detail::DocumentHandle>(loaded.document),
        .values   = std::move(values),
        .user_modified = {},
    };
    return Status::OK;
  }

  static Status build_index(const std::map<std::string, Source>& sources,
                            std::map<std::string, IndexedProperty>& output) {
    output.clear();
    for (const auto& [source_name, source] : sources) {
      for (const auto& [local_name, value] : source.values) {
        const std::string full_name = qualified_name(source_name, local_name);
        const bool modified         = source.user_modified.contains(local_name);
        if (!output
                 .emplace(full_name,
                          IndexedProperty{
                              .property =
                                  Property{
                                      .name          = full_name,
                                      .value         = value,
                                      .mutability    = source.mutability,
                                      .user_modified = modified,
                                  },
                              .source     = source_name,
                              .local_name = local_name,
                          })
                 .second) {
          output.clear();
          return Status::DUPLICATE_PROPERTY;
        }
      }
    }
    return Status::OK;
  }

  config::Config config_;
  mutable std::shared_mutex mutex_;
  std::map<std::string, Source> sources_;
  std::map<std::string, IndexedProperty> properties_;
};

Properties::Properties(std::filesystem::path primary_root,
                       std::filesystem::path user_overrides_root)
    : impl_(std::make_unique<Impl>(std::move(primary_root),
                                   std::move(user_overrides_root))) {}

Properties::~Properties() = default;

Properties::Properties(Properties&&) noexcept = default;

Properties& Properties::operator=(Properties&&) noexcept = default;

LoadResult Properties::load_immutable(std::string source_name,
                                      std::filesystem::path relative_path) {
  if (!valid_source_name(source_name)) {
    return {.status = Status::INVALID_PATH, .document = {}};
  }
  {
    const std::shared_lock lock(impl_->mutex_);
    const auto existing = impl_->sources_.find(source_name);
    if (existing != impl_->sources_.end()) {
      if (existing->second.mutability != Mutability::IMMUTABLE ||
          existing->second.relative_path != relative_path) {
        return {.status = Status::DUPLICATE_SOURCE, .document = {}};
      }
      return {.status   = Status::OK,
              .document = Document{existing->second.document}};
    }
  }

  Impl::Source source;
  const Status status = impl_->read_source(source_name, relative_path,
                                           Mutability::IMMUTABLE, source);
  if (!is_ok(status)) {
    return {.status = status, .document = {}};
  }

  const auto document = source.document;
  const std::unique_lock lock(impl_->mutex_);
  const auto existing = impl_->sources_.find(source_name);
  if (existing != impl_->sources_.end()) {
    if (existing->second.mutability != Mutability::IMMUTABLE ||
        existing->second.relative_path != relative_path) {
      return {.status = Status::DUPLICATE_SOURCE, .document = {}};
    }
    return {.status   = Status::OK,
            .document = Document{existing->second.document}};
  }
  auto candidate_sources = impl_->sources_;
  candidate_sources.emplace(source_name, std::move(source));
  std::map<std::string, Impl::IndexedProperty> candidate_properties;
  const Status index_status =
      Impl::build_index(candidate_sources, candidate_properties);
  if (!is_ok(index_status)) {
    return {.status = index_status, .document = {}};
  }
  impl_->sources_    = std::move(candidate_sources);
  impl_->properties_ = std::move(candidate_properties);
  return {.status = Status::OK, .document = Document{document}};
}

Status Properties::load_mutable_defaults(std::string source_name,
                                         std::filesystem::path relative_path) {
  if (!valid_source_name(source_name)) {
    return Status::INVALID_PATH;
  }
  {
    const std::shared_lock lock(impl_->mutex_);
    const auto existing = impl_->sources_.find(source_name);
    if (existing != impl_->sources_.end()) {
      return existing->second.mutability == Mutability::USER_MUTABLE &&
                     existing->second.relative_path == relative_path
                 ? Status::OK
                 : Status::DUPLICATE_SOURCE;
    }
  }

  Impl::Source source;
  const Status status = impl_->read_source(source_name, relative_path,
                                           Mutability::USER_MUTABLE, source);
  if (!is_ok(status)) {
    return status;
  }

  const std::unique_lock lock(impl_->mutex_);
  const auto existing = impl_->sources_.find(source_name);
  if (existing != impl_->sources_.end()) {
    return existing->second.mutability == Mutability::USER_MUTABLE &&
                   existing->second.relative_path == relative_path
               ? Status::OK
               : Status::DUPLICATE_SOURCE;
  }
  auto candidate_sources = impl_->sources_;
  candidate_sources.emplace(source_name, std::move(source));
  std::map<std::string, Impl::IndexedProperty> candidate_properties;
  const Status index_status =
      Impl::build_index(candidate_sources, candidate_properties);
  if (!is_ok(index_status)) {
    return index_status;
  }
  impl_->sources_    = std::move(candidate_sources);
  impl_->properties_ = std::move(candidate_properties);
  return Status::OK;
}

LoadResult Properties::document(std::string_view source_name) const {
  const std::shared_lock lock(impl_->mutex_);
  const auto source = impl_->sources_.find(std::string{source_name});
  if (source == impl_->sources_.end() ||
      source->second.mutability != Mutability::IMMUTABLE) {
    return {.status = Status::NOT_FOUND, .document = {}};
  }
  return {.status = Status::OK, .document = Document{source->second.document}};
}

Status Properties::get(std::string_view name, Property& output) const {
  const std::shared_lock lock(impl_->mutex_);
  const auto property = impl_->properties_.find(std::string{name});
  if (property == impl_->properties_.end()) {
    return Status::NOT_FOUND;
  }
  output = property->second.property;
  return Status::OK;
}

std::vector<Property> Properties::list(std::string_view prefix) const {
  std::vector<Property> output;
  const std::shared_lock lock(impl_->mutex_);
  auto property = impl_->properties_.lower_bound(std::string{prefix});
  while (property != impl_->properties_.end() &&
         property->first.starts_with(prefix)) {
    output.push_back(property->second.property);
    ++property;
  }
  return output;
}

Status Properties::set(std::string_view name, std::string_view value) {
  const std::unique_lock lock(impl_->mutex_);
  const auto indexed = impl_->properties_.find(std::string{name});
  if (indexed == impl_->properties_.end()) {
    return Status::NOT_FOUND;
  }
  if (indexed->second.property.mutability != Mutability::USER_MUTABLE) {
    return Status::IMMUTABLE_PROPERTY;
  }

  Scalar next       = indexed->second.property.value;
  const bool parsed = std::visit(
      [&value]<typename Type>(Type& scalar) {
        if constexpr (std::is_same_v<Type, std::string>) {
          scalar.assign(value);
          return true;
        } else if constexpr (std::is_same_v<Type, std::int64_t>) {
          return parse_integer(value, scalar);
        } else if constexpr (std::is_same_v<Type, double>) {
          return parse_float(value, scalar);
        } else if constexpr (std::is_same_v<Type, bool>) {
          if (value == "true") {
            scalar = true;
            return true;
          }
          if (value == "false") {
            scalar = false;
            return true;
          }
          return false;
        } else {
          return false;
        }
      },
      next);
  if (!parsed) {
    return Status::INVALID_VALUE;
  }

  Impl::Source& source = impl_->sources_.at(indexed->second.source);
  source.values[indexed->second.local_name] = next;
  source.user_modified.insert(indexed->second.local_name);
  indexed->second.property.value         = std::move(next);
  indexed->second.property.user_modified = true;
  return Status::OK;
}

Status Properties::reload() {
  const std::unique_lock lock(impl_->mutex_);
  std::map<std::string, Impl::Source> candidate_sources;
  for (const auto& [name, source] : impl_->sources_) {
    Impl::Source candidate;
    const Status status = impl_->read_source(name, source.relative_path,
                                             source.mutability, candidate);
    if (!is_ok(status)) {
      return status;
    }
    if (source.mutability == Mutability::USER_MUTABLE) {
      for (const std::string& local_name : source.user_modified) {
        const auto previous = source.values.find(local_name);
        const auto current  = candidate.values.find(local_name);
        if (previous != source.values.end() &&
            current != candidate.values.end() &&
            same_type(previous->second, current->second)) {
          current->second = previous->second;
          candidate.user_modified.insert(local_name);
        }
      }
    }
    candidate_sources.emplace(name, std::move(candidate));
  }

  std::map<std::string, Impl::IndexedProperty> candidate_properties;
  const Status status =
      Impl::build_index(candidate_sources, candidate_properties);
  if (!is_ok(status)) {
    return status;
  }
  impl_->sources_    = std::move(candidate_sources);
  impl_->properties_ = std::move(candidate_properties);
  return Status::OK;
}

}  // namespace puc::properties
