/**
 * @file cmdframe_msgs.cpp
 * @brief CmdFrame notification payload encoding.
 */

#include "msgs/cmdframe_msgs.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "msgs/status.hpp"
#include "utils/logger/logger.hpp"
#include "utils/utf8/utf8.hpp"

/** @cond CMD_FRAME_MESSAGES_LOGGER_MODULE */
LOGGER_MODULE("CmdFrame Messages");
/** @endcond */

namespace puc::msg {

Status CmdFrameNotificationCodec::encode_payload(
    const CmdFrameNotification& notification,
    std::vector<std::uint8_t>& output) const {
  if (!utf8::is_valid(notification.text)) {
    return Status::PAYLOAD_ENCODING_FAILED;
  }
  output.assign(notification.text.begin(), notification.text.end());
  return Status::OK;
}

Status CmdFrameNotificationCodec::decode_payload(
    std::span<const std::uint8_t> payload, CmdFrameNotification& output) const {
  if (payload.empty()) {
    output.text.clear();
    return Status::OK;
  }
  const std::string_view text{reinterpret_cast<const char*>(payload.data()),
                              payload.size()};
  if (!utf8::is_valid(text)) {
    return Status::MALFORMED_PAYLOAD;
  }
  output.text.assign(text);
  return Status::OK;
}

Status register_cmdframe_codecs(MessageCodecCollection& codecs) {
  const Status status =
      codecs.register_codec(std::make_unique<CmdFrameNotificationCodec>());
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not register CmdFrame notification codec: "
                  << status_message(status);
  }
  return status;
}

}  // namespace puc::msg
