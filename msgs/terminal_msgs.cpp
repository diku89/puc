/**
 * @file terminal_msgs.cpp
 * @brief Portable normalized terminal-input event encoding.
 */

#include "msgs/terminal_msgs.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "utils/utf8/utf8.hpp"

namespace puc::msg {
namespace {

enum class WireEvent : std::uint8_t {
  KEY = 1U,
  TEXT,
  MOUSE,
  SCROLL,
  PASTE,
  FOCUS,
  CLIPBOARD,
  COMMAND,
  RESPONSE,
  UNKNOWN,
};

constexpr std::uint16_t kKnownModifierBits    = 0x00ffU;
constexpr std::uint32_t kMaximumUnicodeScalar = 0x10ffffU;

bool valid_scalar(std::uint32_t value) noexcept {
  return value <= kMaximumUnicodeScalar &&
         !(value >= 0xd800U && value <= 0xdfffU);
}

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>(value >> 24U));
  output.push_back(static_cast<std::uint8_t>(value >> 16U));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
  output.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t read_u16(std::span<const std::uint8_t> input) noexcept {
  return static_cast<std::uint16_t>(input[0]) << 8U |
         static_cast<std::uint16_t>(input[1]);
}

std::uint32_t read_u32(std::span<const std::uint8_t> input) noexcept {
  return static_cast<std::uint32_t>(input[0]) << 24U |
         static_cast<std::uint32_t>(input[1]) << 16U |
         static_cast<std::uint32_t>(input[2]) << 8U |
         static_cast<std::uint32_t>(input[3]);
}

bool append_string(std::vector<std::uint8_t>& output, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
  return true;
}

bool read_string(std::span<const std::uint8_t> payload, std::size_t& offset,
                 std::string& output) {
  if (payload.size() - offset < 4U) {
    return false;
  }
  const std::uint32_t size = read_u32(payload.subspan(offset, 4U));
  offset += 4U;
  if (static_cast<std::size_t>(size) > payload.size() - offset) {
    return false;
  }
  output.assign(reinterpret_cast<const char*>(payload.data() + offset), size);
  offset += size;
  return true;
}

bool valid_key(const TerminalKeyEvent& event) noexcept {
  return (event.named || valid_scalar(event.key)) && event.action <= 2U &&
         (event.modifiers & ~kKnownModifierBits) == 0U &&
         (!event.shifted_key.has_value() || valid_scalar(*event.shifted_key)) &&
         (!event.base_layout_key.has_value() ||
          valid_scalar(*event.base_layout_key)) &&
         utf8::is_valid(event.text);
}

}  // namespace

Status TerminalInputEventCodec::encode_payload(
    const TerminalInputEvent& event, std::vector<std::uint8_t>& output) const {
  return std::visit(
      [&output](const auto& value) -> Status {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, TerminalKeyEvent>) {
          if (!valid_key(value)) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::KEY));
          output.push_back(value.named ? 1U : 0U);
          append_u32(output, value.key);
          append_u16(output, value.modifiers);
          output.push_back(value.action);
          std::uint8_t option_bits = 0U;
          option_bits |= value.shifted_key.has_value() ? 1U : 0U;
          option_bits |= value.base_layout_key.has_value() ? 2U : 0U;
          output.push_back(option_bits);
          if (value.shifted_key.has_value()) {
            append_u32(output, *value.shifted_key);
          }
          if (value.base_layout_key.has_value()) {
            append_u32(output, *value.base_layout_key);
          }
          return append_string(output, value.text)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        } else if constexpr (std::is_same_v<Type, TerminalTextEvent>) {
          if (!utf8::is_valid(value.utf8)) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::TEXT));
          return append_string(output, value.utf8)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        } else if constexpr (std::is_same_v<Type, TerminalMouseEvent>) {
          if (value.button > 7U || value.action > 3U ||
              (value.modifiers & ~kKnownModifierBits) != 0U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::MOUSE));
          append_u32(output, value.x);
          append_u32(output, value.y);
          output.push_back(value.button);
          output.push_back(value.action);
          append_u16(output, value.modifiers);
          return Status::OK;
        } else if constexpr (std::is_same_v<Type, TerminalScrollEvent>) {
          if ((value.modifiers & ~kKnownModifierBits) != 0U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::SCROLL));
          append_u32(output, value.x);
          append_u32(output, value.y);
          append_u32(output, static_cast<std::uint32_t>(value.delta_x));
          append_u32(output, static_cast<std::uint32_t>(value.delta_y));
          append_u16(output, value.modifiers);
          return Status::OK;
        } else if constexpr (std::is_same_v<Type, TerminalPasteEvent>) {
          if (value.phase > 3U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::PASTE));
          output.push_back(value.phase);
          return append_string(output, value.data)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        } else if constexpr (std::is_same_v<Type, TerminalFocusEvent>) {
          output.push_back(static_cast<std::uint8_t>(WireEvent::FOCUS));
          output.push_back(value.focused ? 1U : 0U);
          return Status::OK;
        } else if constexpr (std::is_same_v<Type, TerminalClipboardEvent>) {
          if (value.selection > 1U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::CLIPBOARD));
          output.push_back(value.selection);
          return append_string(output, value.data)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        } else if constexpr (std::is_same_v<Type, TerminalCommandEvent>) {
          if (value.command > 11U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::COMMAND));
          append_u16(output, value.command);
          return Status::OK;
        } else if constexpr (std::is_same_v<Type, TerminalResponseEvent>) {
          if (value.kind > 5U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::RESPONSE));
          output.push_back(value.kind);
          append_u32(output, value.value);
          return append_string(output, value.bytes)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        } else {
          if (value.reason > 4U) {
            return Status::PAYLOAD_ENCODING_FAILED;
          }
          output.push_back(static_cast<std::uint8_t>(WireEvent::UNKNOWN));
          output.push_back(value.reason);
          return append_string(output, value.bytes)
                     ? Status::OK
                     : Status::PAYLOAD_ENCODING_FAILED;
        }
      },
      event.data);
}

Status TerminalInputEventCodec::decode_payload(
    std::span<const std::uint8_t> payload, TerminalInputEvent& output) const {
  if (payload.empty()) {
    return Status::MALFORMED_PAYLOAD;
  }
  std::size_t offset = 1U;
  switch (static_cast<WireEvent>(payload.front())) {
    case WireEvent::KEY: {
      if (payload.size() < 10U || payload[1] > 1U || payload[9] > 3U) {
        return Status::MALFORMED_PAYLOAD;
      }
      TerminalKeyEvent event{
          .named     = payload[1] != 0U,
          .key       = read_u32(payload.subspan(2U, 4U)),
          .modifiers = read_u16(payload.subspan(6U, 2U)),
          .action    = payload[8],
      };
      offset = 10U;
      if ((payload[9] & 1U) != 0U) {
        if (payload.size() - offset < 4U) {
          return Status::MALFORMED_PAYLOAD;
        }
        event.shifted_key = read_u32(payload.subspan(offset, 4U));
        offset += 4U;
      }
      if ((payload[9] & 2U) != 0U) {
        if (payload.size() - offset < 4U) {
          return Status::MALFORMED_PAYLOAD;
        }
        event.base_layout_key = read_u32(payload.subspan(offset, 4U));
        offset += 4U;
      }
      if (!read_string(payload, offset, event.text) ||
          offset != payload.size() || !valid_key(event)) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
    case WireEvent::TEXT: {
      TerminalTextEvent event;
      if (!read_string(payload, offset, event.utf8) ||
          offset != payload.size() || !utf8::is_valid(event.utf8)) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
    case WireEvent::MOUSE: {
      if (payload.size() != 13U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const TerminalMouseEvent event{
          .x         = read_u32(payload.subspan(1U, 4U)),
          .y         = read_u32(payload.subspan(5U, 4U)),
          .button    = payload[9],
          .action    = payload[10],
          .modifiers = read_u16(payload.subspan(11U, 2U)),
      };
      if (event.button > 7U || event.action > 3U ||
          (event.modifiers & ~kKnownModifierBits) != 0U) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = event;
      return Status::OK;
    }
    case WireEvent::SCROLL: {
      if (payload.size() != 19U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const TerminalScrollEvent event{
          .x = read_u32(payload.subspan(1U, 4U)),
          .y = read_u32(payload.subspan(5U, 4U)),
          .delta_x =
              static_cast<std::int32_t>(read_u32(payload.subspan(9U, 4U))),
          .delta_y =
              static_cast<std::int32_t>(read_u32(payload.subspan(13U, 4U))),
          .modifiers = read_u16(payload.subspan(17U, 2U)),
      };
      if ((event.modifiers & ~kKnownModifierBits) != 0U) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = event;
      return Status::OK;
    }
    case WireEvent::PASTE: {
      if (payload.size() < 6U || payload[1] > 3U) {
        return Status::MALFORMED_PAYLOAD;
      }
      TerminalPasteEvent event{.phase = payload[1]};
      offset = 2U;
      if (!read_string(payload, offset, event.data) ||
          offset != payload.size()) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
    case WireEvent::FOCUS:
      if (payload.size() != 2U || payload[1] > 1U) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = TerminalFocusEvent{.focused = payload[1] != 0U};
      return Status::OK;
    case WireEvent::CLIPBOARD: {
      if (payload.size() < 6U || payload[1] > 1U) {
        return Status::MALFORMED_PAYLOAD;
      }
      TerminalClipboardEvent event{.selection = payload[1]};
      offset = 2U;
      if (!read_string(payload, offset, event.data) ||
          offset != payload.size()) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
    case WireEvent::COMMAND: {
      if (payload.size() != 3U) {
        return Status::MALFORMED_PAYLOAD;
      }
      const std::uint16_t command = read_u16(payload.subspan(1U, 2U));
      if (command > 11U) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = TerminalCommandEvent{.command = command};
      return Status::OK;
    }
    case WireEvent::RESPONSE: {
      if (payload.size() < 10U || payload[1] > 5U) {
        return Status::MALFORMED_PAYLOAD;
      }
      TerminalResponseEvent event{.kind  = payload[1],
                                  .value = read_u32(payload.subspan(2U, 4U))};
      offset = 6U;
      if (!read_string(payload, offset, event.bytes) ||
          offset != payload.size()) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
    case WireEvent::UNKNOWN: {
      if (payload.size() < 6U || payload[1] > 4U) {
        return Status::MALFORMED_PAYLOAD;
      }
      TerminalUnknownEvent event{.reason = payload[1]};
      offset = 2U;
      if (!read_string(payload, offset, event.bytes) ||
          offset != payload.size()) {
        return Status::MALFORMED_PAYLOAD;
      }
      output.data = std::move(event);
      return Status::OK;
    }
  }
  return Status::MALFORMED_PAYLOAD;
}

Status register_terminal_codecs(MessageCodecCollection& codecs) {
  return codecs.register_codec(std::make_unique<TerminalInputEventCodec>());
}

std::string_view terminal_input_event_name(
    const TerminalInputEvent& event) noexcept {
  return std::visit(
      [](const auto& value) -> std::string_view {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, TerminalKeyEvent>) return "key";
        if constexpr (std::is_same_v<Type, TerminalTextEvent>) return "text";
        if constexpr (std::is_same_v<Type, TerminalMouseEvent>) return "mouse";
        if constexpr (std::is_same_v<Type, TerminalScrollEvent>)
          return "scroll";
        if constexpr (std::is_same_v<Type, TerminalPasteEvent>) return "paste";
        if constexpr (std::is_same_v<Type, TerminalFocusEvent>) return "focus";
        if constexpr (std::is_same_v<Type, TerminalClipboardEvent>)
          return "clipboard";
        if constexpr (std::is_same_v<Type, TerminalCommandEvent>)
          return "command";
        if constexpr (std::is_same_v<Type, TerminalResponseEvent>)
          return "response";
        return "unknown";
      },
      event.data);
}

}  // namespace puc::msg
