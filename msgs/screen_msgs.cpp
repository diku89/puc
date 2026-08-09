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

constexpr std::uint8_t kPreserveSignals = 1U << 0U;
constexpr std::uint8_t kAlternateScreen = 1U << 1U;
constexpr std::uint8_t kHideCursor      = 1U << 2U;
constexpr std::uint8_t kDisableAutoWrap = 1U << 3U;
constexpr std::uint8_t kBracketedPaste  = 1U << 4U;
constexpr std::uint8_t kFocusReporting  = 1U << 5U;
constexpr std::uint8_t kKnownOptionBits = kPreserveSignals | kAlternateScreen |
                                          kHideCursor | kDisableAutoWrap |
                                          kBracketedPaste | kFocusReporting;

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

bool valid_mouse_tracking(ScreenMouseTracking tracking) noexcept {
  switch (tracking) {
    case ScreenMouseTracking::NONE:
    case ScreenMouseTracking::BUTTONS:
    case ScreenMouseTracking::DRAG:
    case ScreenMouseTracking::MOTION:
      return true;
  }
  return false;
}

bool valid_clipboard_selection(ScreenClipboardSelection selection) noexcept {
  return selection == ScreenClipboardSelection::PRIMARY ||
         selection == ScreenClipboardSelection::CLIPBOARD;
}

}  // namespace

Status ScreenCommandCodec::encode_payload(
    const ScreenCommand& command, std::vector<std::uint8_t>& output) const {
  if (const auto* take = std::get_if<ScreenTakeCommand>(&command.data)) {
    if (!valid_mouse_tracking(take->options.mouse)) {
      return Status::PAYLOAD_ENCODING_FAILED;
    }
    std::uint8_t flags = 0U;
    flags |= take->options.preserve_signals ? kPreserveSignals : 0U;
    flags |= take->options.alternate_screen ? kAlternateScreen : 0U;
    flags |= take->options.hide_cursor ? kHideCursor : 0U;
    flags |= take->options.disable_auto_wrap ? kDisableAutoWrap : 0U;
    flags |= take->options.bracketed_paste ? kBracketedPaste : 0U;
    flags |= take->options.focus_reporting ? kFocusReporting : 0U;
    if (take->initial_bytes.size() >
            std::numeric_limits<std::uint32_t>::max() ||
        take->final_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Status::PAYLOAD_ENCODING_FAILED;
    }
    output.reserve(15U + take->initial_bytes.size() + take->final_bytes.size());
    output.push_back(static_cast<std::uint8_t>(WireCommand::TAKE));
    output.push_back(flags);
    output.push_back(static_cast<std::uint8_t>(take->options.mouse));
    append_u32(output, take->options.kitty_keyboard_flags);
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
      if (payload.size() < 15U || (payload[1] & ~kKnownOptionBits) != 0U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const auto mouse = static_cast<ScreenMouseTracking>(payload[2]);
      if (!valid_mouse_tracking(mouse)) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::uint32_t size = read_u32(payload.subspan(7U, 4U));
      if (payload.size() < 15U ||
          static_cast<std::size_t>(size) > payload.size() - 15U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::size_t final_size_offset =
          11U + static_cast<std::size_t>(size);
      const std::uint32_t final_size =
          read_u32(payload.subspan(final_size_offset, 4U));
      if (payload.size() - final_size_offset - 4U != final_size) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = ScreenTakeCommand{
          .options =
              ScreenSessionOptions{
                  .preserve_signals     = (payload[1] & kPreserveSignals) != 0U,
                  .alternate_screen     = (payload[1] & kAlternateScreen) != 0U,
                  .hide_cursor          = (payload[1] & kHideCursor) != 0U,
                  .disable_auto_wrap    = (payload[1] & kDisableAutoWrap) != 0U,
                  .bracketed_paste      = (payload[1] & kBracketedPaste) != 0U,
                  .focus_reporting      = (payload[1] & kFocusReporting) != 0U,
                  .mouse                = mouse,
                  .kitty_keyboard_flags = read_u32(payload.subspan(3U, 4U)),
              },
          .initial_bytes =
              std::string{reinterpret_cast<const char*>(payload.data() + 11U),
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
