/**
 * @file timeouts.cpp
 * @brief Layered TOML loading for terminal timeout defaults.
 */

#include "puc-cli/tui/terminal/timeouts.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "properties/properties.hpp"
#include "utils/logger/logger.hpp"

/** @cond TERMINAL_TIMEOUTS_LOGGER_MODULE */
LOGGER_MODULE("Terminal Timeouts");
/** @endcond */

namespace puc::terminal {
namespace {

Status configuration_status(properties::Status status) noexcept {
  return status == properties::Status::PARSE_ERROR
             ? Status::CONFIGURATION_PARSE_FAILED
             : Status::CONFIGURATION_LOAD_FAILED;
}

bool read_positive_duration(const properties::Properties& properties,
                            std::string_view name,
                            std::chrono::milliseconds& output) {
  properties::Property property;
  const properties::Status status = properties.get(name, property);
  const auto* value               = std::get_if<std::int64_t>(
      properties::is_ok(status) ? &property.value : nullptr);
  if (value == nullptr || *value <= 0) {
    Logger<ERROR> << "Terminal timeout '" << name
                  << "' must be a positive integer number of milliseconds";
    return false;
  }
  output = std::chrono::milliseconds{*value};
  return true;
}

}  // namespace

Status load_timeout_settings(properties::Properties& properties,
                             TimeoutSettings& output) {
  const properties::Status loaded =
      properties.load_mutable_defaults("terminal", kTimeoutConfigurationPath);
  if (loaded != properties::Status::OK &&
      loaded != properties::Status::DUPLICATE_SOURCE) {
    Logger<ERROR> << "Could not load terminal timeout configuration: "
                  << properties::status_message(loaded);
    return configuration_status(loaded);
  }

  TimeoutSettings candidate;
  if (!read_positive_duration(properties, "terminal.timeouts.input_sequence_ms",
                              candidate.input_sequence) ||
      !read_positive_duration(properties, "terminal.timeouts.multiple_click_ms",
                              candidate.multiple_click)) {
    return Status::CONFIGURATION_PARSE_FAILED;
  }
  output = candidate;
  return Status::OK;
}

}  // namespace puc::terminal
