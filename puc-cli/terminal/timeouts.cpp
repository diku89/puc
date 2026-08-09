/**
 * @file timeouts.cpp
 * @brief Layered TOML loading for terminal timeout defaults.
 */

#include "puc-cli/terminal/timeouts.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

#include "utils/config/config.hpp"
#include "utils/logger/logger.hpp"

/** @cond TERMINAL_TIMEOUTS_LOGGER_MODULE */
LOGGER_MODULE("Terminal Timeouts");
/** @endcond */

namespace puc::terminal {
namespace {

Status configuration_status(config::Status status) noexcept {
  return status == config::Status::PARSE_ERROR
             ? Status::CONFIGURATION_PARSE_FAILED
             : Status::CONFIGURATION_LOAD_FAILED;
}

bool read_positive_duration(const config::LoadResult& loaded,
                            std::string_view path,
                            std::chrono::milliseconds& output) {
  const std::optional<std::int64_t> value = loaded.find(path).as_integer();
  if (!value.has_value() || *value <= 0) {
    Logger<ERROR> << "Terminal timeout '" << path
                  << "' must be a positive integer number of milliseconds";
    return false;
  }
  output = std::chrono::milliseconds{*value};
  return true;
}

}  // namespace

Status load_timeout_settings(const config::Config& configurations,
                             TimeoutSettings& output) {
  const config::LoadResult loaded =
      configurations.load(kTimeoutConfigurationPath);
  if (loaded.status != config::Status::OK) {
    Logger<ERROR> << "Could not load terminal timeout configuration: "
                  << config::status_message(loaded.status);
    return configuration_status(loaded.status);
  }

  TimeoutSettings candidate;
  if (!read_positive_duration(loaded, "timeouts.input_sequence_ms",
                              candidate.input_sequence) ||
      !read_positive_duration(loaded, "timeouts.multiple_click_ms",
                              candidate.multiple_click)) {
    return Status::CONFIGURATION_PARSE_FAILED;
  }
  output = candidate;
  return Status::OK;
}

}  // namespace puc::terminal
