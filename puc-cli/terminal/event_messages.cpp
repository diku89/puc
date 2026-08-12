/**
 * @file event_messages.cpp
 * @brief Terminal-event portable-message conversion implementation.
 */

#include "puc-cli/terminal/event_messages.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace puc::terminal {
namespace {

template <typename Enum>
constexpr auto ordinal(Enum value) noexcept {
  return static_cast<std::underlying_type_t<Enum>>(value);
}

template <typename Enum>
bool valid_ordinal(std::uint32_t value, Enum maximum) noexcept {
  return value <= static_cast<std::uint32_t>(ordinal(maximum));
}

bool fits_u32(std::size_t value) noexcept {
  return value <= std::numeric_limits<std::uint32_t>::max();
}

bool fits_i32(int value) noexcept {
  return value >= std::numeric_limits<std::int32_t>::min() &&
         value <= std::numeric_limits<std::int32_t>::max();
}

}  // namespace

msg::Status to_message(const Event& event, msg::TerminalInputEvent& output) {
  output = {};
  return std::visit(
      [&output](const auto& value) -> msg::Status {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, KeyEvent>) {
          msg::TerminalKeyEvent converted{
              .modifiers = value.modifiers.bits(),
              .action    = static_cast<std::uint8_t>(ordinal(value.action)),
              .shifted_key =
                  value.shifted_key.has_value()
                      ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                            *value.shifted_key)}
                      : std::nullopt,
              .base_layout_key =
                  value.base_layout_key.has_value()
                      ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(
                            *value.base_layout_key)}
                      : std::nullopt,
              .text = value.text,
          };
          if (const auto* named = std::get_if<NamedKey>(&value.key.value)) {
            converted.named = true;
            converted.key   = static_cast<std::uint32_t>(ordinal(*named));
          } else {
            converted.named = false;
            converted.key =
                static_cast<std::uint32_t>(std::get<char32_t>(value.key.value));
          }
          output.data = std::move(converted);
        } else if constexpr (std::is_same_v<Type, TextEvent>) {
          output.data = msg::TerminalTextEvent{.utf8 = value.utf8};
        } else if constexpr (std::is_same_v<Type, MouseEvent>) {
          if (!fits_u32(value.position.x) || !fits_u32(value.position.y)) {
            return msg::Status::PAYLOAD_ENCODING_FAILED;
          }
          output.data = msg::TerminalMouseEvent{
              .x         = static_cast<std::uint32_t>(value.position.x),
              .y         = static_cast<std::uint32_t>(value.position.y),
              .button    = static_cast<std::uint8_t>(ordinal(value.button)),
              .action    = static_cast<std::uint8_t>(ordinal(value.action)),
              .modifiers = value.modifiers.bits(),
          };
        } else if constexpr (std::is_same_v<Type, ScrollEvent>) {
          if (!fits_u32(value.position.x) || !fits_u32(value.position.y) ||
              !fits_i32(value.delta_x) || !fits_i32(value.delta_y)) {
            return msg::Status::PAYLOAD_ENCODING_FAILED;
          }
          output.data = msg::TerminalScrollEvent{
              .x         = static_cast<std::uint32_t>(value.position.x),
              .y         = static_cast<std::uint32_t>(value.position.y),
              .delta_x   = static_cast<std::int32_t>(value.delta_x),
              .delta_y   = static_cast<std::int32_t>(value.delta_y),
              .modifiers = value.modifiers.bits(),
          };
        } else if constexpr (std::is_same_v<Type, PasteEvent>) {
          output.data = msg::TerminalPasteEvent{
              .phase = static_cast<std::uint8_t>(ordinal(value.phase)),
              .data  = value.data,
          };
        } else if constexpr (std::is_same_v<Type, FocusEvent>) {
          output.data = msg::TerminalFocusEvent{.focused = value.focused};
        } else if constexpr (std::is_same_v<Type, ClipboardEvent>) {
          output.data = msg::TerminalClipboardEvent{
              .selection = static_cast<std::uint8_t>(ordinal(value.selection)),
              .data      = value.data,
          };
        } else if constexpr (std::is_same_v<Type, CommandEvent>) {
          output.data = msg::TerminalCommandEvent{
              .command = static_cast<std::uint16_t>(ordinal(value.command)),
          };
        } else if constexpr (std::is_same_v<Type, TerminalResponseEvent>) {
          output.data = msg::TerminalResponseEvent{
              .kind  = static_cast<std::uint8_t>(ordinal(value.kind)),
              .value = value.value,
              .bytes = value.bytes,
          };
        } else {
          output.data = msg::TerminalUnknownEvent{
              .reason = static_cast<std::uint8_t>(ordinal(value.reason)),
              .bytes  = value.bytes,
          };
        }
        return msg::Status::OK;
      },
      event);
}

msg::Status from_message(const msg::TerminalInputEvent& message,
                         Event& output) {
  output = KeyEvent{};
  return std::visit(
      [&output](const auto& value) -> msg::Status {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, msg::TerminalKeyEvent>) {
          if (value.action > ordinal(KeyAction::RELEASE) ||
              (value.named &&
               !valid_ordinal(value.key, NamedKey::ISO_LEVEL5_SHIFT))) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          KeyEvent converted{
              .modifiers = Modifiers::from_bits(value.modifiers),
              .action    = static_cast<KeyAction>(value.action),
              .shifted_key =
                  value.shifted_key.has_value()
                      ? std::optional<char32_t>{static_cast<char32_t>(
                            *value.shifted_key)}
                      : std::nullopt,
              .base_layout_key =
                  value.base_layout_key.has_value()
                      ? std::optional<char32_t>{static_cast<char32_t>(
                            *value.base_layout_key)}
                      : std::nullopt,
              .text = value.text,
          };
          converted.key = value.named
                              ? KeyCode{static_cast<NamedKey>(value.key)}
                              : KeyCode{static_cast<char32_t>(value.key)};
          output        = std::move(converted);
        } else if constexpr (std::is_same_v<Type, msg::TerminalTextEvent>) {
          output = TextEvent{.utf8 = value.utf8};
        } else if constexpr (std::is_same_v<Type, msg::TerminalMouseEvent>) {
          if (!valid_ordinal(value.button, MouseButton::AUXILIARY_4) ||
              !valid_ordinal(value.action, MouseAction::DRAG)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = MouseEvent{
              .position  = CellPosition{.x = value.x, .y = value.y},
              .button    = static_cast<MouseButton>(value.button),
              .action    = static_cast<MouseAction>(value.action),
              .modifiers = Modifiers::from_bits(value.modifiers),
          };
        } else if constexpr (std::is_same_v<Type, msg::TerminalScrollEvent>) {
          output = ScrollEvent{
              .position  = CellPosition{.x = value.x, .y = value.y},
              .delta_x   = value.delta_x,
              .delta_y   = value.delta_y,
              .modifiers = Modifiers::from_bits(value.modifiers),
          };
        } else if constexpr (std::is_same_v<Type, msg::TerminalPasteEvent>) {
          if (!valid_ordinal(value.phase, PastePhase::CANCEL)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = PasteEvent{.phase = static_cast<PastePhase>(value.phase),
                              .data  = value.data};
        } else if constexpr (std::is_same_v<Type, msg::TerminalFocusEvent>) {
          output = FocusEvent{.focused = value.focused};
        } else if constexpr (std::is_same_v<Type,
                                            msg::TerminalClipboardEvent>) {
          if (!valid_ordinal(value.selection, ClipboardSelection::PRIMARY)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = ClipboardEvent{
              .selection = static_cast<ClipboardSelection>(value.selection),
              .data      = value.data};
        } else if constexpr (std::is_same_v<Type, msg::TerminalCommandEvent>) {
          if (!valid_ordinal(value.command, Command::MOVE_PAGE_DOWN)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = CommandEvent{.command = static_cast<Command>(value.command)};
        } else if constexpr (std::is_same_v<Type, msg::TerminalResponseEvent>) {
          if (!valid_ordinal(value.kind,
                             TerminalResponseKind::DEVICE_CONTROL_STRING)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = TerminalResponseEvent{
              .kind  = static_cast<TerminalResponseKind>(value.kind),
              .value = value.value,
              .bytes = value.bytes};
        } else {
          if (!valid_ordinal(value.reason,
                             UnknownInputReason::LIMIT_EXCEEDED)) {
            return msg::Status::MALFORMED_PAYLOAD;
          }
          output = UnknownEvent{
              .reason = static_cast<UnknownInputReason>(value.reason),
              .bytes  = value.bytes};
        }
        return msg::Status::OK;
      },
      message.data);
}

}  // namespace puc::terminal
