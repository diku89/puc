/**
 * @file msg.cpp
 * @brief Version-zero IPC wire codec and SHA-256 integrity checking.
 */

#include "utils/ipc/msg.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>

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

/** Minimal streaming SHA-256 implementation used only by the wire codec. */
class Sha256 {
 public:
  /** Add bytes to the digest. */
  void update(std::span<const std::uint8_t> bytes) noexcept {
    byte_count_ += bytes.size();
    for (const std::uint8_t byte : bytes) {
      buffer_[buffer_size_++] = byte;
      if (buffer_size_ == buffer_.size()) {
        process_block(buffer_);
        buffer_size_ = 0U;
      }
    }
  }

  /** Finish padding and return the digest. */
  Checksum finish() noexcept {
    const std::uint64_t bit_count = byte_count_ * 8U;
    buffer_[buffer_size_++]       = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                buffer_.end(), 0U);
      process_block(buffer_);
      buffer_size_ = 0U;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
              buffer_.begin() + 56, 0U);
    for (std::size_t index = 0U; index < sizeof(bit_count); ++index) {
      const std::size_t shift = (sizeof(bit_count) - index - 1U) * 8U;
      buffer_[56U + index]    = static_cast<std::uint8_t>(bit_count >> shift);
    }
    process_block(buffer_);

    Checksum result;
    for (std::size_t word = 0U; word < state_.size(); ++word) {
      for (std::size_t byte = 0U; byte < sizeof(std::uint32_t); ++byte) {
        const std::size_t shift = (sizeof(std::uint32_t) - byte - 1U) * 8U;
        result.data[word * sizeof(std::uint32_t) + byte] =
            static_cast<std::uint8_t>(state_[word] >> shift);
      }
    }
    return result;
  }

 private:
  /** SHA-256 compression constants. */
  static constexpr std::array<std::uint32_t, 64U> kRoundConstants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
  };

  /** Compress one complete 64-byte block into state_. */
  void process_block(const std::array<std::uint8_t, 64U>& block) noexcept {
    std::array<std::uint32_t, 64U> schedule{};
    for (std::size_t index = 0U; index < 16U; ++index) {
      schedule[index] = read_integer<std::uint32_t>(block, index * 4U);
    }
    for (std::size_t index = 16U; index < schedule.size(); ++index) {
      const std::uint32_t first = std::rotr(schedule[index - 15U], 7) ^
                                  std::rotr(schedule[index - 15U], 18) ^
                                  (schedule[index - 15U] >> 3U);
      const std::uint32_t second = std::rotr(schedule[index - 2U], 17) ^
                                   std::rotr(schedule[index - 2U], 19) ^
                                   (schedule[index - 2U] >> 10U);
      schedule[index] =
          schedule[index - 16U] + first + schedule[index - 7U] + second;
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (std::size_t index = 0U; index < schedule.size(); ++index) {
      const std::uint32_t sum_one =
          std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const std::uint32_t choice = (e & f) ^ (~e & g);
      const std::uint32_t temporary_one =
          h + sum_one + choice + kRoundConstants[index] + schedule[index];
      const std::uint32_t sum_zero =
          std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const std::uint32_t majority      = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temporary_two = sum_zero + majority;
      h                                 = g;
      g                                 = f;
      f                                 = e;
      e                                 = d + temporary_one;
      d                                 = c;
      c                                 = b;
      b                                 = a;
      a                                 = temporary_one + temporary_two;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  /** Current digest state. */
  std::array<std::uint32_t, 8U> state_ = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  std::array<std::uint8_t, 64U> buffer_{}; /**< Partial input block. */
  std::size_t buffer_size_  = 0U;          /**< Meaningful bytes in buffer_. */
  std::uint64_t byte_count_ = 0U;          /**< Total unpadded input bytes. */
};

/** Compute SHA-256 over exactly `bytes`. */
Checksum sha256(std::span<const std::uint8_t> bytes) noexcept {
  Sha256 hasher;
  hasher.update(bytes);
  return hasher.finish();
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
                         std::vector<std::uint8_t>& output) {
  output.clear();
  if (message.header.channel_id == 0U) {
    Logger<ERROR> << "Cannot serialize a message with channel id zero";
    return Status::INVALID_ARGUMENT;
  }
  if (message.payload.size() > kMaximumPayloadBytes) {
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
                           DecodedMessage& output,
                           std::size_t& consumed_bytes) noexcept {
  output = {};
  DecodedMessage decoded;
  consumed_bytes = 0U;
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
  if (decoded.header.payload_length > kMaximumPayloadBytes) {
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
