#pragma once

/**
 * @file status.hpp
 * @brief Non-throwing result codes shared by IPC components.
 */

#include <cstddef>
#include <string_view>

namespace puc::ipc {

/** Result of an IPC operation. */
enum class Status {
  OK,                     /**< The operation completed successfully. */
  INVALID_ARGUMENT,       /**< A supplied value violates the API contract. */
  INVALID_CHANNEL_NAME,   /**< A channel name is not a canonical IPC path. */
  INVALID_TRANSPORT_PATH, /**< A socket or FIFO path cannot be used. */
  DUPLICATE_CHANNEL,      /**< A directory already owns the channel name. */
  CHANNEL_NOT_FOUND,      /**< A directory has no channel under that name. */
  CHANNEL_UNAVAILABLE, /**< A channel could not be initialized or was closed. */
  NOT_CONNECTED,       /**< A stream channel has no connected peer. */
  MESSAGE_TOO_LARGE,   /**< A message exceeds the channel or wire limit. */
  PARTIAL_TRANSFER,    /**< Only some destinations accepted a fan-out. */
  IO_ERROR,            /**< A filesystem, socket, read, or write failed. */
  END_OF_STREAM,       /**< A connected peer closed its byte stream. */
  MALFORMED_MESSAGE,   /**< Wire bytes violate the versioned format. */
  TRUNCATED_MESSAGE,   /**< More bytes are required to complete a message. */
  UNSUPPORTED_VERSION, /**< The wire version is not implemented. */
  CHECKSUM_MISMATCH,   /**< A message checksum does not match its bytes. */
  IDENTIFIER_EXHAUSTED, /**< No unused directory or subscription id remains. */
};

/** Return whether a status represents success. */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/** Return stable, human-readable text for an IPC status. */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid argument";
    case Status::INVALID_CHANNEL_NAME:
      return "channel name is invalid";
    case Status::INVALID_TRANSPORT_PATH:
      return "IPC transport path is invalid";
    case Status::DUPLICATE_CHANNEL:
      return "channel name is already registered";
    case Status::CHANNEL_NOT_FOUND:
      return "channel was not found";
    case Status::CHANNEL_UNAVAILABLE:
      return "channel is unavailable";
    case Status::NOT_CONNECTED:
      return "channel has no connected peer";
    case Status::MESSAGE_TOO_LARGE:
      return "message exceeds its configured limit";
    case Status::PARTIAL_TRANSFER:
      return "only some channel destinations accepted the message";
    case Status::IO_ERROR:
      return "IPC input or output failed";
    case Status::END_OF_STREAM:
      return "IPC peer closed the stream";
    case Status::MALFORMED_MESSAGE:
      return "IPC message is malformed";
    case Status::TRUNCATED_MESSAGE:
      return "IPC message is incomplete";
    case Status::UNSUPPORTED_VERSION:
      return "IPC wire version is unsupported";
    case Status::CHECKSUM_MISMATCH:
      return "IPC message checksum does not match";
    case Status::IDENTIFIER_EXHAUSTED:
      return "IPC identifier space is exhausted";
  }
  return "unknown IPC status";
}

/** Status and accepted payload byte count returned by a transmission. */
struct TransferResult {
  Status status     = Status::OK; /**< Operation result. */
  std::size_t bytes = 0; /**< Payload bytes accepted on full success. */

  /** Compare the complete transfer result. */
  constexpr bool operator==(const TransferResult&) const noexcept = default;
};

}  // namespace puc::ipc
