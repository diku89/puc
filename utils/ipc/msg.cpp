/**
 * @file msg.cpp
 * @brief Version-zero IPC wire codec and SHA-256 integrity checking.
 */

#include "utils/ipc/msg.hpp"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "utils/hash/sha256.hpp"
#include "utils/logger/logger.hpp"

/** @cond IPC_MESSAGE_LOGGER_MODULE */
LOGGER_MODULE("IPC Message Codec");
/** @endcond */

namespace puc::ipc {
namespace {

constexpr std::array<std::uint8_t, 4U> kMagic = {'P', 'U', 'C', 'I'};
constexpr std::size_t kBaseHeaderBytes        = 24U;
constexpr std::uint8_t kSessionFlag           = 1U << 0U;
constexpr std::uint8_t kTimestampFlag         = 1U << 1U;
constexpr std::uint8_t kMultipartFlag         = 1U << 2U;
constexpr std::uint8_t kChecksumFlag          = 1U << 3U;
constexpr std::uint8_t kExtensionFlag         = 1U << 4U;
constexpr std::uint8_t kKnownFlags            = kSessionFlag | kTimestampFlag |
                                                kMultipartFlag | kChecksumFlag |
                                                kExtensionFlag;

/** Append an unsigned integer in network byte order. */
template <std::unsigned_integral Integer>
void append_integer(std::vector<std::uint8_t>& output, Integer value) {
  for (std::size_t offset = sizeof(Integer); offset > 0U; --offset) {
    const std::size_t shift = (offset - 1U) * 8U;
    output.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

/** Read an unsigned integer in network byte order. */
template <std::unsigned_integral Integer>
Integer read_integer(std::span<const std::uint8_t> data,
                     std::size_t offset) noexcept {
  Integer value = 0U;
  for (std::size_t index = 0U; index < sizeof(Integer); ++index) {
    value = static_cast<Integer>((value << 8U) | data[offset + index]);
  }
  return value;
}

/** Compute SHA-256 over exactly `bytes`. */
Checksum sha256(std::span<const std::uint8_t> bytes) noexcept {
  return Checksum{.data = hashing::sha256(bytes).bytes};
}

/** Compare digest bytes without data-dependent early return. */
bool checksum_matches(const Checksum& expected,
                      std::span<const std::uint8_t> actual) noexcept {
  std::uint8_t difference = 0U;
  for (std::size_t index = 0U; index < expected.data.size(); ++index) {
    difference |= expected.data[index] ^ actual[index];
  }
  return difference == 0U;
}

}  // namespace

Status serialize_message(const Message& message,
                         std::size_t maximum_payload_bytes,
                         std::vector<std::uint8_t>& output) {
  output.clear();
  if (message.header.channel_id == 0U || maximum_payload_bytes == 0U) {
    Logger<ERROR> << "Cannot serialize a message with channel id zero";
    return Status::INVALID_ARGUMENT;
  }
  if (message.payload.size() > maximum_payload_bytes) {
    return Status::MESSAGE_TOO_LARGE;
  }
  if (message.session_id.has_value() &&
      (message.session_id->length == 0U ||
       message.session_id->length > kMaximumSessionIdBytes)) {
    return Status::INVALID_ARGUMENT;
  }
  if (message.multipart.has_value() &&
      (message.multipart->total_parts == 0U ||
       message.multipart->part_index >= message.multipart->total_parts)) {
    return Status::INVALID_ARGUMENT;
  }

  std::size_t header_bytes = kBaseHeaderBytes;
  if (message.session_id.has_value()) {
    header_bytes += 1U + message.session_id->length;
  }
  if (message.timestamp.has_value()) {
    header_bytes += sizeof(std::uint64_t);
  }
  if (message.multipart.has_value()) {
    header_bytes += 2U * sizeof(std::uint32_t);
  }
  if (message.extension.size() >
      std::numeric_limits<std::uint16_t>::max() - header_bytes) {
    return Status::MESSAGE_TOO_LARGE;
  }
  header_bytes += message.extension.size();

  std::uint8_t flags = 0U;
  flags |= message.session_id.has_value() ? kSessionFlag : 0U;
  flags |= message.timestamp.has_value() ? kTimestampFlag : 0U;
  flags |= message.multipart.has_value() ? kMultipartFlag : 0U;
  flags |= message.include_checksum ? kChecksumFlag : 0U;
  flags |= message.extension.empty() ? 0U : kExtensionFlag;
  const std::size_t checksum_bytes =
      message.include_checksum ? kChecksumBytes : 0U;
  output.reserve(header_bytes + message.payload.size() + checksum_bytes);
  output.insert(output.end(), kMagic.begin(), kMagic.end());
  output.push_back(kWireVersion);
  output.push_back(flags);
  append_integer(output, static_cast<std::uint16_t>(header_bytes));
  append_integer(output, message.header.channel_id);
  append_integer(output, message.header.message_id);
  append_integer(output, static_cast<std::uint64_t>(message.payload.size()));

  if (message.session_id.has_value()) {
    output.push_back(message.session_id->length);
    output.insert(
        output.end(), message.session_id->data.begin(),
        message.session_id->data.begin() + message.session_id->length);
  }
  if (message.timestamp.has_value()) {
    append_integer(output, message.timestamp->unix_time_ns);
  }
  if (message.multipart.has_value()) {
    append_integer(output, message.multipart->total_parts);
    append_integer(output, message.multipart->part_index);
  }
  output.insert(output.end(), message.extension.begin(),
                message.extension.end());
  output.insert(output.end(), message.payload.begin(), message.payload.end());
  if (message.include_checksum) {
    const Checksum checksum = sha256(output);
    output.insert(output.end(), checksum.data.begin(), checksum.data.end());
  }
  return Status::OK;
}

Status deserialize_message(std::span<const std::uint8_t> data,
                           std::size_t maximum_payload_bytes,
                           DecodedMessage& output,
                           std::size_t& consumed_bytes) noexcept {
  output = {};
  DecodedMessage decoded;
  consumed_bytes = 0U;
  if (maximum_payload_bytes == 0U) return Status::INVALID_ARGUMENT;
  if (data.size() < kBaseHeaderBytes) {
    return Status::TRUNCATED_MESSAGE;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), data.begin())) {
    return Status::MALFORMED_MESSAGE;
  }
  const std::uint8_t version = data[4U];
  if (version != kWireVersion) {
    return Status::UNSUPPORTED_VERSION;
  }
  const std::uint8_t flags = data[5U];
  if ((flags & static_cast<std::uint8_t>(~kKnownFlags)) != 0U) {
    return Status::MALFORMED_MESSAGE;
  }
  const std::size_t header_bytes = read_integer<std::uint16_t>(data, 6U);
  if (header_bytes < kBaseHeaderBytes) {
    return Status::MALFORMED_MESSAGE;
  }
  if (data.size() < header_bytes) {
    return Status::TRUNCATED_MESSAGE;
  }

  decoded.wire_header = WireHeader{
      .version        = version,
      .header_bytes   = static_cast<std::uint16_t>(header_bytes),
      .has_session_id = (flags & kSessionFlag) != 0U,
      .has_timestamp  = (flags & kTimestampFlag) != 0U,
      .is_multipart   = (flags & kMultipartFlag) != 0U,
      .has_checksum   = (flags & kChecksumFlag) != 0U,
      .has_extension  = (flags & kExtensionFlag) != 0U,
  };
  decoded.header.channel_id     = read_integer<ChannelId>(data, 8U);
  decoded.header.message_id     = read_integer<std::uint32_t>(data, 12U);
  decoded.header.payload_length = read_integer<std::uint64_t>(data, 16U);
  if (decoded.header.channel_id == 0U) {
    return Status::MALFORMED_MESSAGE;
  }
  if (decoded.header.payload_length > maximum_payload_bytes) {
    return Status::MESSAGE_TOO_LARGE;
  }

  std::size_t cursor = kBaseHeaderBytes;
  if (decoded.wire_header.has_session_id) {
    if (cursor >= header_bytes) {
      return Status::MALFORMED_MESSAGE;
    }
    SessionId session;
    session.length = data[cursor++];
    if (session.length == 0U || session.length > kMaximumSessionIdBytes ||
        session.length > header_bytes - cursor) {
      return Status::MALFORMED_MESSAGE;
    }
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(cursor),
                session.length, session.data.begin());
    cursor += session.length;
    decoded.session_id = session;
  }
  if (decoded.wire_header.has_timestamp) {
    if (header_bytes - cursor < sizeof(std::uint64_t)) {
      return Status::MALFORMED_MESSAGE;
    }
    decoded.timestamp =
        Timestamp{.unix_time_ns = read_integer<std::uint64_t>(data, cursor)};
    cursor += sizeof(std::uint64_t);
  }
  if (decoded.wire_header.is_multipart) {
    if (header_bytes - cursor < 2U * sizeof(std::uint32_t)) {
      return Status::MALFORMED_MESSAGE;
    }
    decoded.multipart = MultipartHeader{
        .total_parts = read_integer<std::uint32_t>(data, cursor),
        .part_index =
            read_integer<std::uint32_t>(data, cursor + sizeof(std::uint32_t)),
    };
    cursor += 2U * sizeof(std::uint32_t);
    if (decoded.multipart->total_parts == 0U ||
        decoded.multipart->part_index >= decoded.multipart->total_parts) {
      return Status::MALFORMED_MESSAGE;
    }
  }
  if (decoded.wire_header.has_extension) {
    if (cursor == header_bytes) {
      return Status::MALFORMED_MESSAGE;
    }
    decoded.extension = data.subspan(cursor, header_bytes - cursor);
    cursor            = header_bytes;
  } else if (cursor != header_bytes) {
    return Status::MALFORMED_MESSAGE;
  }

  const std::size_t payload_bytes =
      static_cast<std::size_t>(decoded.header.payload_length);
  const std::size_t checksum_bytes =
      decoded.wire_header.has_checksum ? kChecksumBytes : 0U;
  if (payload_bytes >
      std::numeric_limits<std::size_t>::max() - header_bytes - checksum_bytes) {
    return Status::MESSAGE_TOO_LARGE;
  }
  const std::size_t total_bytes = header_bytes + payload_bytes + checksum_bytes;
  if (data.size() < total_bytes) {
    return Status::TRUNCATED_MESSAGE;
  }
  decoded.payload = data.subspan(header_bytes, payload_bytes);
  if (decoded.wire_header.has_checksum) {
    const Checksum expected = sha256(data.first(total_bytes - kChecksumBytes));
    const std::span<const std::uint8_t> encoded =
        data.subspan(total_bytes - kChecksumBytes, kChecksumBytes);
    if (!checksum_matches(expected, encoded)) {
      Logger<WARN> << "Rejected IPC message with a mismatched checksum";
      return Status::CHECKSUM_MISMATCH;
    }
    Checksum checksum;
    std::copy(encoded.begin(), encoded.end(), checksum.data.begin());
    decoded.checksum = checksum;
  }
  output         = decoded;
  consumed_bytes = total_bytes;
  return Status::OK;
}

}  // namespace puc::ipc
