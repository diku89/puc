/**
 * @file screen_msgs.cpp
 * @brief Portable encoding for one-way screen commands and resize events.
 */

#include "msgs/screen_msgs.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "msgs/status.hpp"
#include "utils/logger/logger.hpp"

/** @cond SCREEN_MESSAGES_LOGGER_MODULE */
LOGGER_MODULE("Screen Messages");
/** @endcond */

namespace puc::msg {
namespace {

enum class WireCommand : std::uint8_t {
  TAKE          = 1U,
  RELEASE       = 2U,
  PRESENT       = 3U,
  SET_CLIPBOARD = 4U,
};

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t read_u32(std::span<const std::uint8_t> input) noexcept {
  return static_cast<std::uint32_t>(input[0]) << 24U |
         static_cast<std::uint32_t>(input[1]) << 16U |
         static_cast<std::uint32_t>(input[2]) << 8U |
         static_cast<std::uint32_t>(input[3]);
}

bool valid_clipboard_selection(ScreenClipboardSelection selection) noexcept {
  return selection == ScreenClipboardSelection::PRIMARY ||
         selection == ScreenClipboardSelection::CLIPBOARD;
}

}  // namespace

Status ScreenCommandCodec::encode_payload(
    const ScreenCommand& command, std::vector<std::uint8_t>& output) const {
  if (const auto* take = std::get_if<ScreenTakeCommand>(&command.data)) {
    if (take->initial_bytes.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        take->final_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::PAYLOAD_ENCODING_FAILED;
    }
    output.reserve(9U + take->initial_bytes.size() + take->final_bytes.size());
    output.push_back(static_cast<std::uint8_t>(WireCommand::TAKE));
    append_u32(output, static_cast<std::uint32_t>(take->initial_bytes.size()));
    output.insert(output.end(), take->initial_bytes.begin(),
                  take->initial_bytes.end());
    append_u32(output, static_cast<std::uint32_t>(take->final_bytes.size()));
    output.insert(output.end(), take->final_bytes.begin(),
                  take->final_bytes.end());
    return Status::OK;
  }
  if (std::holds_alternative<ScreenReleaseCommand>(command.data)) {
    output.push_back(static_cast<std::uint8_t>(WireCommand::RELEASE));
    return Status::OK;
  }

  if (const auto* clipboard =
          std::get_if<ScreenSetClipboardCommand>(&command.data)) {
    if (!valid_clipboard_selection(clipboard->selection) ||
        clipboard->text.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::PAYLOAD_ENCODING_FAILED;
    }
    output.reserve(6U + clipboard->text.size());
    output.push_back(static_cast<std::uint8_t>(WireCommand::SET_CLIPBOARD));
    output.push_back(static_cast<std::uint8_t>(clipboard->selection));
    append_u32(output, static_cast<std::uint32_t>(clipboard->text.size()));
    output.insert(output.end(), clipboard->text.begin(), clipboard->text.end());
    return Status::OK;
  }

  const std::string& bytes = std::get<ScreenPresentCommand>(command.data).bytes;
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Status::PAYLOAD_ENCODING_FAILED;
  }
  output.reserve(5U + bytes.size());
  output.push_back(static_cast<std::uint8_t>(WireCommand::PRESENT));
  append_u32(output, static_cast<std::uint32_t>(bytes.size()));
  output.insert(output.end(), bytes.begin(), bytes.end());
  return Status::OK;
}

Status ScreenCommandCodec::decode_payload(std::span<const std::uint8_t> payload,
                                          ScreenCommand& output) const {
  if (payload.empty()) {
    return Status::MALFORMED_PAYLOAD;
  }
  switch (static_cast<WireCommand>(payload.front())) {
    case WireCommand::TAKE: {
      if (payload.size() < 9U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::uint32_t size = read_u32(payload.subspan(1U, 4U));
      if (static_cast<std::size_t>(size) > payload.size() - 9U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::size_t final_size_offset = 5U + static_cast<std::size_t>(size);
      const std::uint32_t final_size =
          read_u32(payload.subspan(final_size_offset, 4U));
      if (payload.size() - final_size_offset - 4U != final_size) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = ScreenTakeCommand{
          .initial_bytes =
              std::string{reinterpret_cast<const char*>(payload.data() + 5U),
                          size},
          .final_bytes =
              std::string{reinterpret_cast<const char*>(payload.data() +
                                                        final_size_offset + 4U),
                          final_size},
      };
      return Status::OK;
    }
    case WireCommand::RELEASE:
      if (payload.size() != 1U) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = ScreenReleaseCommand{};
      return Status::OK;
    case WireCommand::PRESENT: {
      if (payload.size() < 5U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::uint32_t size = read_u32(payload.subspan(1U, 4U));
      if (payload.size() - 5U != size) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = ScreenPresentCommand{
          .bytes =
              std::string{reinterpret_cast<const char*>(payload.data() + 5U),
                          size},
      };
      return Status::OK;
    }
    case WireCommand::SET_CLIPBOARD: {
      if (payload.size() < 6U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const auto selection = static_cast<ScreenClipboardSelection>(payload[1U]);
      const std::uint32_t size = read_u32(payload.subspan(2U, 4U));
      if (!valid_clipboard_selection(selection) ||
          payload.size() - 6U != size) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = ScreenSetClipboardCommand{
          .selection = selection,
          .text =
              std::string{reinterpret_cast<const char*>(payload.data() + 6U),
                          size},
      };
      return Status::OK;
    }
  }
  return Status::MALFORMED_PAYLOAD;
}

Status ScreenResizeEventCodec::encode_payload(
    const ScreenResizeEvent& event, std::vector<std::uint8_t>& output) const {
  if (event.width == 0U || event.height == 0U) {
    return Status::PAYLOAD_ENCODING_FAILED;
  }
  output.reserve(16U);
  append_u32(output, event.width);
  append_u32(output, event.height);
  append_u32(output, event.pixel_width);
  append_u32(output, event.pixel_height);
  return Status::OK;
}

Status ScreenResizeEventCodec::decode_payload(
    std::span<const std::uint8_t> payload, ScreenResizeEvent& output) const {
  if (payload.size() != 16U) {
    return Status::MALFORMED_PAYLOAD;
  }
  output = ScreenResizeEvent{
      .width        = read_u32(payload.subspan(0U, 4U)),
      .height       = read_u32(payload.subspan(4U, 4U)),
      .pixel_width  = read_u32(payload.subspan(8U, 4U)),
      .pixel_height = read_u32(payload.subspan(12U, 4U)),
  };
  return output.width == 0U || output.height == 0U ? Status::MALFORMED_PAYLOAD
                                                   : Status::OK;
}

Status register_screen_codecs(MessageCodecCollection& codecs) {
  Status status = codecs.register_codec(std::make_unique<ScreenCommandCodec>());
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not register screen command codec: "
                  << status_message(status);
    return status;
  }
  status = codecs.register_codec(std::make_unique<ScreenResizeEventCodec>());
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not register screen resize codec: "
                  << status_message(status);
  }
  return status;
}

}  // namespace puc::msg
