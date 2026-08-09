#pragma once

/**
 * @file timeouts.hpp
 * @brief Clock-free timeout inputs and configured terminal timing policy.
 */

#include <chrono>
#include <cstdint>
#include <optional>

#include "puc-cli/terminal/status.hpp"

namespace puc::config {
class Config;
}

namespace puc::terminal {

/** System-default and user-overridable terminal input timing policy. */
struct TimeoutSettings {
  std::chrono::milliseconds input_sequence{50};  /**< Incomplete input wait. */
  std::chrono::milliseconds multiple_click{500}; /**< Click-chain wait. */

  /** Compare both configured durations. */
  constexpr bool operator==(const TimeoutSettings&) const noexcept = default;
};

/**
 * One explicit timeout delivered to a clock-free state machine.
 *
 * A producer schedules the token returned by the target state machine. The
 * monotonically changing generation makes an already-scheduled timeout stale
 * when newer input rearms or cancels that machine's wait.
 */
struct TimeoutInput {
  std::uint64_t generation = 0U; /**< Nonzero target-issued generation. */

  /** Compare generation identity. */
  constexpr bool operator==(const TimeoutInput&) const noexcept = default;
};

/** Fixed convention-over-configuration path for terminal timing policy. */
inline constexpr const char* kTimeoutConfigurationPath =
    "terminal_timeouts.toml";

/**
 * Load terminal timing policy through Config's primary/override hierarchy.
 *
 * Both integer millisecond values are required and must be positive. Loading
 * is transactional: `output` changes only after the entire file validates.
 */
Status load_timeout_settings(const config::Config& configurations,
                             TimeoutSettings& output);

}  // namespace puc::terminal
