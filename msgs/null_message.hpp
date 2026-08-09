#pragma once

/**
 * @file null_message.hpp
 * @brief Empty message value and payload codec.
 */

#include <cstdint>
#include <format>
#include <span>
#include <string_view>
#include <vector>

#include "msgs/codec.hpp"

namespace puc::msg {

/**
 * Message carrying no typed data and no payload bytes.
 *
 * \msg{puc::msg::NullMessage||Represents an explicit typed no-data payload and
 * gives generic dispatch a valid schema at message ID zero.||Components that
 * need a typed no-data envelope.||Generic dispatch and components expecting an
 * explicit no-data payload.}
 */
struct NullMessage {
  /** Compare empty messages. */
  constexpr bool operator==(const NullMessage&) const noexcept = default;
};

}  // namespace puc::msg

namespace std {

/** Format NullMessage as its complete JSON representation. */
template <>
struct formatter<puc::msg::NullMessage, char>
    : formatter<std::string_view, char> {
  /** Write an empty JSON object using the standard string formatter. */
  template <typename FormatContext>
  auto format(const puc::msg::NullMessage&, FormatContext& context) const {
    return formatter<std::string_view, char>::format("{}", context);
  }
};

}  // namespace std

namespace puc::msg {

/** Codec for the zero-byte NullMessage payload schema. */
class NullMessageCodec final : public Codec<NullMessage> {
 public:
  /** Construct the codec under MessageId::NULL_MESSAGE. */
  constexpr NullMessageCodec() noexcept : Codec(MessageId::NULL_MESSAGE) {}

 private:
  /** Encode the null value as an empty payload. */
  Status encode_payload(const NullMessage&,
                        std::vector<std::uint8_t>& output) const override {
    output.clear();
    return Status::OK;
  }

  /** Accept only the null schema's empty payload. */
  Status decode_payload(std::span<const std::uint8_t> payload,
                        NullMessage& output) const override {
    output = {};
    return payload.empty() ? Status::OK : Status::MALFORMED_PAYLOAD;
  }
};

}  // namespace puc::msg
