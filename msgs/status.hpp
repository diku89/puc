#pragma once

/**
 * @file status.hpp
 * @brief Human-readable message payload codec result codes.
 */

#include <string_view>

namespace puc::msg {

/** Result of a payload codec or codec-registry operation. */
enum class Status {
  OK,                   /**< The operation completed successfully. */
  INVALID_ARGUMENT,     /**< A supplied value violates the API contract. */
  DUPLICATE_MESSAGE_ID, /**< A codec already owns the requested message id. */
  MESSAGE_ID_NOT_FOUND, /**< No codec is registered for the message id. */
  CODEC_TYPE_MISMATCH,  /**< The id's codec owns a different C++ value type. */
  PAYLOAD_ENCODING_FAILED, /**< A typed value could not be encoded. */
  MALFORMED_PAYLOAD,       /**< Payload bytes do not match the codec schema. */
  INCOMPLETE_IPC_MESSAGE,  /**< More bytes are needed for the IPC envelope. */
  INVALID_IPC_MESSAGE,     /**< The surrounding IPC message is not valid. */
};

/** Return whether a payload codec operation succeeded. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for a payload codec status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid argument";
    case Status::DUPLICATE_MESSAGE_ID:
      return "payload message id is already registered";
    case Status::MESSAGE_ID_NOT_FOUND:
      return "payload message id was not found";
    case Status::CODEC_TYPE_MISMATCH:
      return "payload codec owns a different value type";
    case Status::PAYLOAD_ENCODING_FAILED:
      return "payload could not be encoded";
    case Status::MALFORMED_PAYLOAD:
      return "payload does not match its message schema";
    case Status::INCOMPLETE_IPC_MESSAGE:
      return "IPC message is incomplete";
    case Status::INVALID_IPC_MESSAGE:
      return "IPC message could not be decoded";
  }
  return "unknown payload codec status";
}

}  // namespace puc::msg
