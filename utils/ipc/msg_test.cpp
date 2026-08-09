/**
 * @file msg_test.cpp
 * @brief Tests for the portable version-zero IPC wire format.
 */

#include "utils/ipc/msg.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gtest/gtest.h"

namespace puc::ipc {
namespace {

constexpr std::array kPayload = {std::uint8_t{1}, std::uint8_t{2},
                                 std::uint8_t{3}};

Message basic_message(bool checksum = false) {
  return Message{
      .header  = MessageHeader{.channel_id = 1U, .message_id = 0x01020304U},
      .payload = kPayload,
      .include_checksum = checksum,
  };
}

TEST(MessageCodecTest, EmitsDocumentedNetworkByteOrderLayout) {
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(basic_message(), encoded), Status::OK);
  constexpr std::array<std::uint8_t, 27U> expected = {
      std::uint8_t{'P'},
      std::uint8_t{'U'},
      std::uint8_t{'C'},
      std::uint8_t{'I'},
      0x00U,
      0x00U,
      0x00U,
      0x18U,
      0x00U,
      0x00U,
      0x00U,
      0x01U,
      0x01U,
      0x02U,
      0x03U,
      0x04U,
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0x00U,
      0x03U,
      0x01U,
      0x02U,
      0x03U,
  };
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{expected.begin(), expected.end()}));
}

TEST(MessageCodecTest, RoundTripsEveryOptionalHeaderWithoutCopyingViews) {
  SessionId session{.length = 4U};
  session.data[0]                = 0xdeU;
  session.data[1]                = 0xadU;
  session.data[2]                = 0xbeU;
  session.data[3]                = 0xefU;
  constexpr std::array extension = {std::uint8_t{9}, std::uint8_t{8}};
  constexpr std::array payload   = {std::uint8_t{7}, std::uint8_t{6},
                                    std::uint8_t{5}, std::uint8_t{4}};
  const Message message{
      .header           = MessageHeader{.channel_id = 42U, .message_id = 99U},
      .session_id       = session,
      .timestamp        = Timestamp{.unix_time_ns = 123456789U},
      .multipart        = MultipartHeader{.total_parts = 3U, .part_index = 1U},
      .extension        = extension,
      .payload          = payload,
      .include_checksum = true,
  };
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(message, encoded), Status::OK);

  DecodedMessage decoded;
  std::size_t consumed = 0U;
  ASSERT_EQ(deserialize_message(encoded, decoded, consumed), Status::OK);
  EXPECT_EQ(consumed, encoded.size());
  EXPECT_EQ(decoded.wire_header.version, kWireVersion);
  EXPECT_TRUE(decoded.wire_header.has_session_id);
  EXPECT_TRUE(decoded.wire_header.has_timestamp);
  EXPECT_TRUE(decoded.wire_header.is_multipart);
  EXPECT_TRUE(decoded.wire_header.has_extension);
  EXPECT_TRUE(decoded.wire_header.has_checksum);
  EXPECT_EQ(decoded.header, (MessageHeader{.channel_id     = 42U,
                                           .message_id     = 99U,
                                           .payload_length = payload.size()}));
  EXPECT_EQ(decoded.session_id, session);
  EXPECT_EQ(decoded.timestamp, Timestamp{.unix_time_ns = 123456789U});
  EXPECT_EQ(decoded.multipart,
            (MultipartHeader{.total_parts = 3U, .part_index = 1U}));
  EXPECT_EQ(std::vector<std::uint8_t>(decoded.extension.begin(),
                                      decoded.extension.end()),
            (std::vector<std::uint8_t>{extension.begin(), extension.end()}));
  EXPECT_EQ(
      std::vector<std::uint8_t>(decoded.payload.begin(), decoded.payload.end()),
      (std::vector<std::uint8_t>{payload.begin(), payload.end()}));
  ASSERT_TRUE(decoded.checksum.has_value());
  EXPECT_GE(decoded.payload.data(), encoded.data());
  EXPECT_LT(decoded.payload.data(), encoded.data() + encoded.size());
}

TEST(MessageCodecTest, ProducesStandardSha256ChecksumBytes) {
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(basic_message(true), encoded), Status::OK);
  constexpr std::array<std::uint8_t, kChecksumBytes> expected_digest = {
      0x9dU, 0x57U, 0x1aU, 0x49U, 0x63U, 0xc7U, 0x8dU, 0x2aU,
      0x53U, 0xaeU, 0xa6U, 0x98U, 0x28U, 0x1aU, 0x20U, 0xadU,
      0x2bU, 0xe5U, 0x3eU, 0x25U, 0xaaU, 0xc8U, 0xf3U, 0x0bU,
      0xe3U, 0x35U, 0xb0U, 0xa4U, 0xc0U, 0x22U, 0xebU, 0x06U,
  };
  ASSERT_GE(encoded.size(), expected_digest.size());
  EXPECT_TRUE(std::equal(
      expected_digest.begin(), expected_digest.end(),
      encoded.end() - static_cast<std::ptrdiff_t>(expected_digest.size())));
}

TEST(MessageCodecTest, AllowsEmptyPayloadsAndTrailingWireData) {
  Message message = basic_message();
  message.payload = {};
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(message, encoded), Status::OK);
  const std::size_t message_size = encoded.size();
  encoded.insert(encoded.end(), {0xaaU, 0xbbU, 0xccU});

  DecodedMessage decoded;
  std::size_t consumed = 0U;
  ASSERT_EQ(deserialize_message(encoded, decoded, consumed), Status::OK);
  EXPECT_EQ(consumed, message_size);
  EXPECT_TRUE(decoded.payload.empty());
}

TEST(MessageCodecTest, RejectsInvalidSemanticMessagesBeforeAllocatingOutput) {
  std::vector<std::uint8_t> output = {9U, 9U};
  Message message                  = basic_message();
  message.header.channel_id        = 0U;
  EXPECT_EQ(serialize_message(message, output), Status::INVALID_ARGUMENT);
  EXPECT_TRUE(output.empty());

  message            = basic_message();
  message.session_id = SessionId{};
  EXPECT_EQ(serialize_message(message, output), Status::INVALID_ARGUMENT);

  SessionId too_long{
      .length = static_cast<std::uint8_t>(kMaximumSessionIdBytes + 1U)};
  message.session_id = too_long;
  EXPECT_EQ(serialize_message(message, output), Status::INVALID_ARGUMENT);

  message           = basic_message();
  message.multipart = MultipartHeader{.total_parts = 0U, .part_index = 0U};
  EXPECT_EQ(serialize_message(message, output), Status::INVALID_ARGUMENT);
  message.multipart = MultipartHeader{.total_parts = 2U, .part_index = 2U};
  EXPECT_EQ(serialize_message(message, output), Status::INVALID_ARGUMENT);
}

TEST(MessageCodecTest, ReportsEveryTruncatedPrefixOfAValidMessage) {
  SessionId session{.length = 1U};
  session.data[0] = 7U;
  const Message message{
      .header           = MessageHeader{.channel_id = 1U, .message_id = 2U},
      .session_id       = session,
      .timestamp        = Timestamp{.unix_time_ns = 3U},
      .multipart        = MultipartHeader{.total_parts = 1U, .part_index = 0U},
      .payload          = kPayload,
      .include_checksum = true,
  };
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(message, encoded), Status::OK);
  for (std::size_t length = 0U; length < encoded.size(); ++length) {
    DecodedMessage decoded;
    std::size_t consumed = 99U;
    EXPECT_EQ(deserialize_message(
                  std::span<const std::uint8_t>{encoded}.first(length), decoded,
                  consumed),
              Status::TRUNCATED_MESSAGE)
        << "prefix length " << length;
    EXPECT_EQ(consumed, 0U);
  }
}

TEST(MessageCodecTest, DistinguishesMalformedUnsupportedAndOversizedInput) {
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(basic_message(), encoded), Status::OK);
  DecodedMessage decoded;
  std::size_t consumed = 0U;

  auto changed = encoded;
  changed[0]   = 'X';
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
  changed    = encoded;
  changed[4] = 1U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::UNSUPPORTED_VERSION);
  changed    = encoded;
  changed[5] = 0x80U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
  changed    = encoded;
  changed[6] = 0U;
  changed[7] = 23U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
  changed     = encoded;
  changed[8]  = 0U;
  changed[9]  = 0U;
  changed[10] = 0U;
  changed[11] = 0U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
  changed     = encoded;
  changed[20] = 1U;
  changed[21] = 0U;
  changed[22] = 0U;
  changed[23] = 1U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MESSAGE_TOO_LARGE);
}

TEST(MessageCodecTest, RejectsContradictoryOptionalHeaderFlagsAndSizes) {
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(basic_message(), encoded), Status::OK);
  DecodedMessage decoded;
  std::size_t consumed = 0U;

  auto changed = encoded;
  changed[5]   = 0x01U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
  changed    = encoded;
  changed[5] = 0x10U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);

  changed = encoded;
  changed.insert(changed.begin() + 24, 0U);
  changed[6] = 0U;
  changed[7] = 25U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);

  changed = encoded;
  changed.insert(changed.begin() + 24, 8U, 0U);
  changed[5] = 0x04U;
  changed[6] = 0U;
  changed[7] = 32U;
  EXPECT_EQ(deserialize_message(changed, decoded, consumed),
            Status::MALFORMED_MESSAGE);
}

TEST(MessageCodecTest, DetectsPayloadAndChecksumCorruptionAndResetsOutput) {
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(serialize_message(basic_message(true), encoded), Status::OK);
  encoded[24U] ^= 0xffU;

  DecodedMessage decoded;
  decoded.header.channel_id = 99U;
  std::size_t consumed      = 99U;
  EXPECT_EQ(deserialize_message(encoded, decoded, consumed),
            Status::CHECKSUM_MISMATCH);
  EXPECT_EQ(decoded.header.channel_id, 0U);
  EXPECT_TRUE(decoded.payload.empty());
  EXPECT_EQ(consumed, 0U);
}

}  // namespace
}  // namespace puc::ipc
