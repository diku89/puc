/**
 * @file codec_test.cpp
 * @brief Tests for typed payload codecs, JSON formatting, and IPC dispatch.
 */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "msgs/msgs.hpp"
#include "utils/ipc/msg.hpp"

namespace puc::msg::test {

struct Point {
  std::uint16_t x = 0U;
  std::uint16_t y = 0U;

  constexpr bool operator==(const Point&) const noexcept = default;
};

}  // namespace puc::msg::test

template <>
struct std::formatter<puc::msg::test::Point, char> {
  constexpr auto parse(std::format_parse_context& context) {
    return context.begin();
  }

  template <typename FormatContext>
  auto format(const puc::msg::test::Point& point,
              FormatContext& context) const {
    return std::format_to(context.out(), "{{\"x\":{},\"y\":{}}}", point.x,
                          point.y);
  }
};

namespace puc::msg {
namespace {

constexpr MessageId kPointMessageId = static_cast<MessageId>(41U);

/** Deliberate IPC envelope limit used by collection tests. */
constexpr std::size_t kTestMaximumPayloadBytes = 1024U;

class PointCodec final : public Codec<test::Point> {
 public:
  explicit constexpr PointCodec(MessageId message_id = kPointMessageId) noexcept
      : Codec(message_id) {}

 private:
  Status encode_payload(const test::Point& point,
                        std::vector<std::uint8_t>& output) const override {
    output.push_back(static_cast<std::uint8_t>(point.x >> 8U));
    output.push_back(static_cast<std::uint8_t>(point.x));
    output.push_back(static_cast<std::uint8_t>(point.y >> 8U));
    output.push_back(static_cast<std::uint8_t>(point.y));
    if (point.x == std::numeric_limits<std::uint16_t>::max()) {
      return Status::PAYLOAD_ENCODING_FAILED;
    }
    return Status::OK;
  }

  Status decode_payload(std::span<const std::uint8_t> payload,
                        test::Point& output) const override {
    output = test::Point{.x = 99U, .y = 99U};
    if (payload.size() != 4U) {
      return Status::MALFORMED_PAYLOAD;
    }
    output.x = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[0]) << 8U | payload[1]);
    output.y = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(payload[2]) << 8U | payload[3]);
    return Status::OK;
  }
};

std::vector<std::uint8_t> wire_message(MessageId message_id,
                                       std::span<const std::uint8_t> payload,
                                       bool include_checksum = false) {
  const ipc::Message message{
      .header           = ipc::MessageHeader{.channel_id = 7U,
                                             .message_id = to_wire_id(message_id)},
      .payload          = payload,
      .include_checksum = include_checksum,
  };
  std::vector<std::uint8_t> encoded;
  if (!ipc::is_ok(
          ipc::serialize_message(message, kTestMaximumPayloadBytes, encoded))) {
    return {};
  }
  return encoded;
}

TEST(MessageCodecStatusTest, OnlyOkIsSuccessful) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::INVALID_ARGUMENT));
  EXPECT_FALSE(is_ok(Status::MALFORMED_PAYLOAD));
  EXPECT_FALSE(is_ok(Status::INCOMPLETE_IPC_MESSAGE));
  EXPECT_FALSE(is_ok(Status::INVALID_IPC_MESSAGE));
}

TEST(MessageCodecStatusTest, EveryDeclaredStatusHasHumanReadableText) {
  constexpr std::array statuses{
      Status::OK,
      Status::INVALID_ARGUMENT,
      Status::DUPLICATE_MESSAGE_ID,
      Status::MESSAGE_ID_NOT_FOUND,
      Status::CODEC_TYPE_MISMATCH,
      Status::PAYLOAD_ENCODING_FAILED,
      Status::MALFORMED_PAYLOAD,
      Status::INCOMPLETE_IPC_MESSAGE,
      Status::INVALID_IPC_MESSAGE,
  };
  for (const Status status : statuses) {
    EXPECT_FALSE(status_message(status).empty());
    EXPECT_NE(status_message(status), "unknown payload codec status");
  }
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown payload codec status");
}

TEST(NullMessageCodecTest, StructFormatsDirectlyAsJson) {
  static_assert(MessageValue<NullMessage>);
  EXPECT_EQ(std::format("{}", NullMessage{}), "{}");
  EXPECT_EQ(NullMessageCodec{}.to_json(NullMessage{}), "{}");
}

TEST(NullMessageCodecTest, EmptyPayloadRoundTrips) {
  NullMessageCodec codec;
  std::vector<std::uint8_t> encoded = {1U};
  EXPECT_EQ(codec.serialize(NullMessage{}, encoded), Status::OK);
  EXPECT_TRUE(encoded.empty());

  NullMessage decoded;
  EXPECT_EQ(codec.deserialize(encoded, decoded), Status::OK);
  EXPECT_EQ(decoded, NullMessage{});
}

TEST(NullMessageCodecTest, NonemptyPayloadIsMalformed) {
  NullMessageCodec codec;
  constexpr std::array payload = {std::uint8_t{1U}};
  NullMessage decoded;
  std::string json = "stale";
  EXPECT_EQ(codec.deserialize(payload, decoded), Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(codec.decode_to_json(payload, json), Status::MALFORMED_PAYLOAD);
  EXPECT_TRUE(json.empty());
}

TEST(MessageCodecCollectionTest, StartsWithTheNullCodec) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  EXPECT_EQ(codecs.size(), 1U);

  NullMessage decoded;
  EXPECT_EQ(codecs.deserialize(MessageId::NULL_MESSAGE,
                               std::span<const std::uint8_t>{}, decoded),
            Status::OK);
}

TEST(MessageCodecCollectionTest, RejectsNullAndDuplicateRegistrations) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  EXPECT_EQ(codecs.register_codec(nullptr), Status::INVALID_ARGUMENT);
  EXPECT_EQ(codecs.register_codec(std::make_unique<NullMessageCodec>()),
            Status::DUPLICATE_MESSAGE_ID);
  EXPECT_EQ(codecs.size(), 1U);
}

TEST(MessageCodecCollectionTest, DispatchesTypedPortablePayloads) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);

  constexpr test::Point point{.x = 0x1234U, .y = 0xabcdU};
  std::vector<std::uint8_t> encoded;
  ASSERT_EQ(codecs.serialize(kPointMessageId, point, encoded), Status::OK);
  constexpr std::array expected = {std::uint8_t{0x12U}, std::uint8_t{0x34U},
                                   std::uint8_t{0xabU}, std::uint8_t{0xcdU}};
  EXPECT_EQ(encoded,
            (std::vector<std::uint8_t>{expected.begin(), expected.end()}));

  test::Point decoded;
  EXPECT_EQ(codecs.deserialize(kPointMessageId, encoded, decoded), Status::OK);
  EXPECT_EQ(decoded, point);

  std::string json;
  EXPECT_EQ(codecs.decode_payload_to_json(kPointMessageId, encoded, json),
            Status::OK);
  EXPECT_EQ(json, R"({"x":4660,"y":43981})");
}

TEST(MessageCodecCollectionTest, ReportsUnknownIdsAndTypeMismatches) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);

  std::vector<std::uint8_t> encoded = {9U};
  EXPECT_EQ(
      codecs.serialize(static_cast<MessageId>(999U), test::Point{}, encoded),
      Status::MESSAGE_ID_NOT_FOUND);
  EXPECT_TRUE(encoded.empty());

  NullMessage null_message;
  EXPECT_EQ(codecs.deserialize(kPointMessageId, std::span<const std::uint8_t>{},
                               null_message),
            Status::CODEC_TYPE_MISMATCH);
}

TEST(MessageCodecCollectionTest, FacadeClearsPartialCodecOutputsOnFailure) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);

  std::vector<std::uint8_t> encoded = {9U};
  const test::Point rejected{.x = std::numeric_limits<std::uint16_t>::max(),
                             .y = 2U};
  EXPECT_EQ(codecs.serialize(kPointMessageId, rejected, encoded),
            Status::PAYLOAD_ENCODING_FAILED);
  EXPECT_TRUE(encoded.empty());

  constexpr std::array malformed = {std::uint8_t{1U}};
  test::Point decoded{.x = 7U, .y = 8U};
  EXPECT_EQ(codecs.deserialize(kPointMessageId, malformed, decoded),
            Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(decoded, test::Point{});
}

TEST(MessageCodecCollectionTest, DecodesAndConsumesOneIpcMessageAtATime) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);

  constexpr test::Point point{.x = 7U, .y = 9U};
  std::vector<std::uint8_t> payload;
  ASSERT_EQ(codecs.serialize(kPointMessageId, point, payload), Status::OK);
  std::vector<std::uint8_t> first =
      wire_message(kPointMessageId, payload, true);
  ASSERT_FALSE(first.empty());
  const std::size_t first_size = first.size();
  const std::vector<std::uint8_t> second =
      wire_message(MessageId::NULL_MESSAGE, {});
  ASSERT_FALSE(second.empty());
  first.insert(first.end(), second.begin(), second.end());

  std::span<const std::uint8_t> remaining = first;
  std::string json;
  EXPECT_EQ(codecs.decode_to_json(remaining, json), Status::OK);
  EXPECT_EQ(json, R"({"x":7,"y":9})");
  EXPECT_EQ(remaining.size(), second.size());
  EXPECT_EQ(remaining.data(), first.data() + first_size);

  EXPECT_EQ(codecs.decode_to_json(remaining, json), Status::OK);
  EXPECT_EQ(json, "{}");
  EXPECT_TRUE(remaining.empty());
}

TEST(MessageCodecCollectionTest, InvalidIpcMessageDoesNotConsumeInput) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  std::vector<std::uint8_t> encoded =
      wire_message(MessageId::NULL_MESSAGE, {}, true);
  ASSERT_FALSE(encoded.empty());
  encoded.back() ^= 0xffU;

  std::span<const std::uint8_t> remaining = encoded;
  const std::uint8_t* original_data       = remaining.data();
  const std::size_t original_size         = remaining.size();
  std::string json                        = "stale";
  EXPECT_EQ(codecs.decode_to_json(remaining, json),
            Status::INVALID_IPC_MESSAGE);
  EXPECT_EQ(remaining.data(), original_data);
  EXPECT_EQ(remaining.size(), original_size);
  EXPECT_TRUE(json.empty());
}

TEST(MessageCodecCollectionTest, IncompleteIpcMessageCanWaitForMoreInput) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  const std::vector<std::uint8_t> encoded =
      wire_message(MessageId::NULL_MESSAGE, {});
  ASSERT_GT(encoded.size(), 5U);

  std::span<const std::uint8_t> remaining =
      std::span<const std::uint8_t>{encoded}.first(5U);
  const std::uint8_t* original_data = remaining.data();
  std::string json                  = "stale";
  EXPECT_EQ(codecs.decode_to_json(remaining, json),
            Status::INCOMPLETE_IPC_MESSAGE);
  EXPECT_EQ(remaining.data(), original_data);
  EXPECT_EQ(remaining.size(), 5U);
  EXPECT_TRUE(json.empty());
}

TEST(MessageCodecCollectionTest, PayloadFailureDoesNotConsumeIpcMessage) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);
  constexpr std::array malformed = {std::uint8_t{1U}};
  const std::vector<std::uint8_t> encoded =
      wire_message(kPointMessageId, malformed);
  ASSERT_FALSE(encoded.empty());

  std::span<const std::uint8_t> remaining = encoded;
  std::string json;
  EXPECT_EQ(codecs.decode_to_json(remaining, json), Status::MALFORMED_PAYLOAD);
  EXPECT_EQ(remaining.size(), encoded.size());
  EXPECT_EQ(remaining.data(), encoded.data());
  EXPECT_TRUE(json.empty());
}

TEST(MessageCodecCollectionTest, UnknownIpcMessageIdDoesNotConsumeInput) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  const std::vector<std::uint8_t> encoded =
      wire_message(static_cast<MessageId>(999U), {});
  ASSERT_FALSE(encoded.empty());

  std::span<const std::uint8_t> remaining = encoded;
  std::string json;
  EXPECT_EQ(codecs.decode_to_json(remaining, json),
            Status::MESSAGE_ID_NOT_FOUND);
  EXPECT_EQ(remaining.size(), encoded.size());
}

TEST(MessageCodecCollectionTest, RegistrationAndConstDispatchAreConcurrent) {
  MessageCodecCollection codecs{kTestMaximumPayloadBytes};
  ASSERT_EQ(codecs.register_codec(std::make_unique<PointCodec>()), Status::OK);
  constexpr std::array payload = {std::uint8_t{0U}, std::uint8_t{7U},
                                  std::uint8_t{0U}, std::uint8_t{9U}};
  std::atomic<bool> succeeded{true};

  std::thread writer([&] {
    for (std::uint32_t id = 100U; id < 132U; ++id) {
      if (!is_ok(codecs.register_codec(
              std::make_unique<PointCodec>(static_cast<MessageId>(id))))) {
        succeeded = false;
      }
    }
  });
  std::vector<std::thread> readers;
  for (std::size_t index = 0U; index < 4U; ++index) {
    readers.emplace_back([&] {
      for (std::size_t iteration = 0U; iteration < 500U; ++iteration) {
        test::Point decoded;
        if (!is_ok(codecs.deserialize(kPointMessageId, payload, decoded)) ||
            decoded != test::Point{.x = 7U, .y = 9U}) {
          succeeded = false;
        }
      }
    });
  }
  writer.join();
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_TRUE(succeeded);
  EXPECT_EQ(codecs.size(), 34U);
}

}  // namespace
}  // namespace puc::msg
