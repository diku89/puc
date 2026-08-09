#pragma once

/**
 * @file status.hpp
 * @brief Non-throwing status codes for terminal transport and protocols.
 */

#include <string_view>

namespace puc {
namespace terminal {

/**
 * Result codes returned by terminal operations.
 *
 * `Status::OK` is the only success value. Protocol errors that can be safely
 * represented as input events do not fail a decoder call; statuses are
 * reserved for invalid API use, resource limits, and operating-system errors.
 */
enum class Status {
  OK,                     /**< Operation completed successfully. */
  INVALID_ARGUMENT,       /**< A supplied argument is invalid. */
  ALREADY_ACTIVE,         /**< The terminal session is already active. */
  NOT_ACTIVE,             /**< The terminal session is not active. */
  TERMINAL_NOT_AVAILABLE, /**< A configured descriptor is not a terminal. */
  TERMINAL_CONFIG_FAILED, /**< Terminal attributes could not be changed. */
  TERMINAL_QUERY_FAILED, /**< Terminal state or dimensions could not be read. */
  TERMINAL_READ_FAILED,  /**< Bytes could not be read from the terminal. */
  TERMINAL_WRITE_FAILED, /**< Bytes could not be written to the terminal. */
  CHANNEL_SETUP_FAILED,  /**< Screen command/event channels are unavailable. */
  INPUT_LIMIT_EXCEEDED,  /**< Buffered untrusted input exceeded its limit. */
  OUTPUT_LIMIT_EXCEEDED, /**< Encoded output would exceed its limit. */
  CONFIGURATION_LOAD_FAILED,  /**< Required terminal configuration is absent or
                                 unreadable. */
  CONFIGURATION_PARSE_FAILED, /**< TOML configuration is invalid. */
  TERMINFO_LOAD_FAILED, /**< The selected terminfo entry could not be loaded. */
  UNSUPPORTED,          /**< The terminal does not support the operation. */
};

/** Test whether a status represents success. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/**
 * Return a stable human-readable description of a status.
 *
 * @param[in] status Status value to describe.
 * @return Static diagnostic text that contains no sensitive input data.
 */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid argument";
    case Status::ALREADY_ACTIVE:
      return "terminal session is already active";
    case Status::NOT_ACTIVE:
      return "terminal session is not active";
    case Status::TERMINAL_NOT_AVAILABLE:
      return "terminal is not available";
    case Status::TERMINAL_CONFIG_FAILED:
      return "terminal mode could not be configured";
    case Status::TERMINAL_QUERY_FAILED:
      return "terminal state could not be queried";
    case Status::TERMINAL_READ_FAILED:
      return "terminal input could not be read";
    case Status::TERMINAL_WRITE_FAILED:
      return "terminal output could not be written";
    case Status::CHANNEL_SETUP_FAILED:
      return "terminal event channels could not be configured";
    case Status::INPUT_LIMIT_EXCEEDED:
      return "terminal input exceeded its configured limit";
    case Status::OUTPUT_LIMIT_EXCEEDED:
      return "terminal output exceeded its configured limit";
    case Status::CONFIGURATION_LOAD_FAILED:
      return "terminal configuration could not be loaded";
    case Status::CONFIGURATION_PARSE_FAILED:
      return "terminal input configuration is invalid";
    case Status::TERMINFO_LOAD_FAILED:
      return "terminal information could not be loaded";
    case Status::UNSUPPORTED:
      return "terminal operation is not supported";
  }
  return "unknown terminal status";
}

}  // namespace terminal
}  // namespace puc
