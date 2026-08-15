#pragma once

/**
 * @file msg.hpp
 * @brief Portable, versioned IPC message serialization.
 *
 * Version zero starts with ASCII `PUCI`, a version byte, optional-section
 * flags, a network-order 16-bit header size, network-order channel and message
 * identifiers, and a network-order 64-bit payload length. Optional session,
 * timestamp, multipart, and opaque extension fields precede the payload. An
 * optional SHA-256 checksum follows it. No native C++ layout enters the wire.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "utils/ipc/channel.hpp"
#include "utils/ipc/status.hpp"

namespace puc::ipc {

/** First and currently supported IPC wire-format version. */
inline constexpr std::uint8_t kWireVersion = 0U;

/** Maximum number of opaque bytes in a session identifier. */
inline constexpr std::size_t kMaximumSessionIdBytes = 16U;

/** Number of bytes in the SHA-256 integrity checksum. */
inline constexpr std::size_t kChecksumBytes = 32U;

/** Decoded fixed preamble fields and optional-section presence flags. */
struct WireHeader {
  std::uint8_t version       = kWireVersion; /**< Wire-format version. */
  std::uint16_t header_bytes = 0U; /**< Preamble and all optional headers. */
  bool has_session_id = false;     /**< A SessionId follows the base header. */
  bool has_timestamp  = false;     /**< A Timestamp follows any session id. */
  bool is_multipart   = false;     /**< A MultipartHeader is present. */
  bool has_checksum   = false;     /**< A SHA-256 digest follows the payload. */
  bool has_extension  = false;     /**< Opaque extension header bytes exist. */

  /** Compare decoded wire metadata. */
  constexpr bool operator==(const WireHeader&) const noexcept = default;
};

/** Opaque, variable-length session identifier. */
struct SessionId {
  std::uint8_t length = 0U; /**< Number of meaningful bytes in `data`. */
  std::array<std::uint8_t, kMaximumSessionIdBytes> data{}; /**< Id bytes. */

  /** Compare only the declared identifier bytes, ignoring unused storage. */
  constexpr bool operator==(const SessionId& other) const noexcept {
    if (length != other.length) {
      return false;
    }
    const std::size_t stored_bytes =
        length < data.size() ? length : data.size();
    for (std::size_t index = 0U; index < stored_bytes; ++index) {
      if (data[index] != other.data[index]) {
        return false;
      }
    }
    return true;
  }
};

/** Nanoseconds since 1970-01-01 00:00:00 UTC. */
struct Timestamp {
  std::uint64_t unix_time_ns = 0U; /**< Unsigned Unix timestamp. */

  /** Compare timestamps. */
  constexpr bool operator==(const Timestamp&) const noexcept = default;
};

/** Position of one independently transported part of a logical message. */
struct MultipartHeader {
  std::uint32_t total_parts = 0U; /**< Total number of parts, at least one. */
  std::uint32_t part_index  = 0U; /**< Zero-based index below total_parts. */

  /** Compare multipart metadata. */
  constexpr bool operator==(const MultipartHeader&) const noexcept = default;
};

/** Routing metadata common to every message. */
struct MessageHeader {
  ChannelId channel_id         = 0U; /**< Nonzero Directory channel id. */
  std::uint32_t message_id     = 0U; /**< Sender-selected channel-local id. */
  std::uint64_t payload_length = 0U; /**< Payload bytes encoded on the wire. */

  /** Compare routing metadata. */
  constexpr bool operator==(const MessageHeader&) const noexcept = default;
};

/** SHA-256 digest attached to a checksummed message. */
struct Checksum {
  std::array<std::uint8_t, kChecksumBytes> data{}; /**< Digest bytes. */

  /** Compare checksum bytes. */
  constexpr bool operator==(const Checksum&) const noexcept = default;
};

/**
 * Semantic message supplied to `serialize_message()`.
 *
 * Optional metadata is encoded in a fixed order. `payload` and `extension`
 * are borrowed only for the duration of serialization. Setting
 * `include_checksum` appends a SHA-256 digest covering every preceding byte,
 * including the preamble and payload.
 */
struct Message {
  MessageHeader header; /**< Routing fields; payload_length is ignored. */
  std::optional<SessionId> session_id;      /**< Optional sender session. */
  std::optional<Timestamp> timestamp;       /**< Optional creation time. */
  std::optional<MultipartHeader> multipart; /**< Optional part position. */
  std::span<const std::uint8_t> extension;  /**< Opaque future header bytes. */
  std::span<const std::uint8_t> payload;    /**< Borrowed payload bytes. */
  bool include_checksum = false; /**< Whether to append integrity metadata. */
};

/**
 * Zero-copy view returned by `deserialize_message()`.
 *
 * The payload and extension spans borrow the input buffer and remain valid
 * only while that buffer's storage remains unchanged and alive.
 */
struct DecodedMessage {
  WireHeader wire_header; /**< Parsed preamble flags and size. */
  MessageHeader header;   /**< Parsed routing fields and payload length. */
  std::optional<SessionId> session_id;      /**< Parsed session metadata. */
  std::optional<Timestamp> timestamp;       /**< Parsed timestamp metadata. */
  std::optional<MultipartHeader> multipart; /**< Parsed part metadata. */
  std::span<const std::uint8_t> extension;  /**< Borrowed extension bytes. */
  std::span<const std::uint8_t> payload;    /**< Borrowed payload bytes. */
  std::optional<Checksum> checksum; /**< Verified checksum when present. */
};

/**
 * Serialize one semantic message into an owned byte vector.
 *
 * Output is cleared before validation and remains empty on failure.
 * Multibyte integers are encoded in network byte order; no C++ object layout
 * is copied into the wire representation.
 *
 * @param message Semantic message and non-owning payload view.
 * @param maximum_payload_bytes Configured upper bound for payload bytes.
 * @param output Destination replaced with one complete encoded message.
 * @return Status::OK, Status::INVALID_ARGUMENT for inconsistent metadata, or
 *         Status::MESSAGE_TOO_LARGE when a configured wire limit is exceeded.
 */
Status serialize_message(const Message& message,
                         std::size_t maximum_payload_bytes,
                         std::vector<std::uint8_t>& output);

/**
 * Parse and validate the first complete message in `data`.
 *
 * Trailing bytes are permitted so callers can decode concatenated messages;
 * `consumed_bytes` reports exactly how much input belonged to the first one.
 * Output and consumed_bytes are reset before parsing and remain reset on any
 * failure.
 *
 * @param data Input which may contain one message followed by more bytes.
 * @param maximum_payload_bytes Configured upper bound for payload bytes.
 * @param output Decoded non-owning views into `data` on success.
 * @param consumed_bytes Number of bytes consumed on success; zero on failure.
 * @return Status::OK or a precise non-throwing wire error status.
 */
Status deserialize_message(std::span<const std::uint8_t> data,
                           std::size_t maximum_payload_bytes,
                           DecodedMessage& output,
                           std::size_t& consumed_bytes) noexcept;

}  // namespace puc::ipc
