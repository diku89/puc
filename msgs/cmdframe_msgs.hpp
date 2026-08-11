#pragma once

/**
 * @file cmdframe_msgs.hpp
 * @brief Typed notification text published to the command-mode view.
 */

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "msgs/codec.hpp"

namespace puc::msg {

/**
 * Channel publishing the newest notification from an executing command.
 *
 * \channel{//cmdframe/notify||Publishes the newest UTF-8 command notification
 * for display beneath the command editor. Pending stale notifications may be
 * replaced before delivery.||
 * \ref puc::command::send_notification "Command notification producers".||
 * The command-mode controller presenting \ref puc::tui::CmdFrame
 * "CmdFrame".}
 */
inline constexpr std::string_view kCmdFrameNotifyChannel = "//cmdframe/notify";

/**
 * UTF-8 text displayed beneath the active or completed command.
 *
 * Empty text is valid and lets a producer clear an earlier notification. The
 * transport retains the complete owned string without terminal styling; the
 * consumer chooses presentation and wrapping.
 *
 * \msg{puc::msg::CmdFrameNotification||Carries one complete UTF-8 status or
 * result string emitted by a command.||
 * \ref puc::command::send_notification "Command notification producers".||
 * The command-mode controller presenting \ref puc::tui::CmdFrame
 * "CmdFrame".}
 */
struct CmdFrameNotification {
  std::string text; /**< Complete owned UTF-8 notification. */

  /** Compare complete notification text. */
  bool operator==(const CmdFrameNotification&) const = default;
};

}  // namespace puc::msg

namespace std {

/** Format CmdFrameNotification as lossless JSON. */
template <>
struct formatter<puc::msg::CmdFrameNotification, char> {
  /** Accept only the formatter's empty format specification. */
  constexpr auto parse(format_parse_context& context) {
    return context.begin();
  }

  /** Write notification bytes as hexadecimal JSON text. */
  template <typename FormatContext>
  auto format(const puc::msg::CmdFrameNotification& notification,
              FormatContext& context) const {
    auto output           = std::format_to(context.out(), "{{\"text_hex\":\"");
    constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char byte : notification.text) {
      *output++ = kHex[byte >> 4U];
      *output++ = kHex[byte & 0x0fU];
    }
    return std::format_to(output, "\"}}");
  }
};

}  // namespace std

namespace puc::msg {

/** Portable UTF-8 payload codec for CmdFrameNotification. */
class CmdFrameNotificationCodec final : public Codec<CmdFrameNotification> {
 public:
  /** Construct the codec under MessageId::CMD_FRAME_NOTIFICATION. */
  constexpr CmdFrameNotificationCodec() noexcept
      : Codec(MessageId::CMD_FRAME_NOTIFICATION) {}

 private:
  Status encode_payload(const CmdFrameNotification& notification,
                        std::vector<std::uint8_t>& output) const override;
  Status decode_payload(std::span<const std::uint8_t> payload,
                        CmdFrameNotification& output) const override;
};

/** Register the command-frame notification schema in a collection. */
Status register_cmdframe_codecs(MessageCodecCollection& codecs);

}  // namespace puc::msg
