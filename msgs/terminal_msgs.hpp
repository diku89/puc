#pragma once

/**
 * @file terminal_msgs.hpp
 * @brief Portable normalized terminal-input event messages.
 */

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "msgs/codec.hpp"

namespace puc::msg {

/**
 * Channel publishing normalized terminal input and protocol responses.
 *
 * \channel{//terminal/input_events||Publishes ordered, terminal-independent
 * keyboard, text, pointer, paste, focus, clipboard, command, protocol-response,
 * and diagnostic events after the terminal Decoder has applied its input
 * Trie.||The lifecycle-owned terminal input producer.||The TUI Screen, which
 * retains decoded events for application-specific handling.}
 */
inline constexpr std::string_view kTerminalInputEventChannel =
    "//terminal/input_events";

/** Portable logical-key message. Numeric enum values follow terminal::NamedKey.
 */
struct TerminalKeyEvent {
  bool named              = true; /**< Whether key stores a named-key value. */
  std::uint32_t key       = 0U;   /**< Named-key ordinal or Unicode scalar. */
  std::uint16_t modifiers = 0U;   /**< Normalized modifier bit field. */
  std::uint8_t action     = 0U;   /**< Press, repeat, or release ordinal. */
  std::optional<std::uint32_t> shifted_key;     /**< Shifted Unicode scalar. */
  std::optional<std::uint32_t> base_layout_key; /**< Base-layout scalar. */
  std::string text; /**< Associated UTF-8 from an extended key protocol. */

  bool operator==(const TerminalKeyEvent&) const = default;
};

/** Portable committed UTF-8 text message. */
struct TerminalTextEvent {
  std::string utf8;
  bool operator==(const TerminalTextEvent&) const = default;
};

/** Portable mouse button or motion message. */
struct TerminalMouseEvent {
  std::uint32_t x                                                     = 0U;
  std::uint32_t y                                                     = 0U;
  std::uint8_t button                                                 = 0U;
  std::uint8_t action                                                 = 0U;
  std::uint16_t modifiers                                             = 0U;
  constexpr bool operator==(const TerminalMouseEvent&) const noexcept = default;
};

/** Portable wheel-motion message. */
struct TerminalScrollEvent {
  std::uint32_t x         = 0U;
  std::uint32_t y         = 0U;
  std::int32_t delta_x    = 0;
  std::int32_t delta_y    = 0;
  std::uint16_t modifiers = 0U;
  constexpr bool operator==(const TerminalScrollEvent&) const noexcept =
      default;
};

/** Portable bracketed-paste stage or data chunk. */
struct TerminalPasteEvent {
  std::uint8_t phase = 0U;
  std::string data;
  bool operator==(const TerminalPasteEvent&) const = default;
};

/** Portable terminal-focus transition. */
struct TerminalFocusEvent {
  bool focused                                                        = false;
  constexpr bool operator==(const TerminalFocusEvent&) const noexcept = default;
};

/** Portable decoded clipboard-query response. */
struct TerminalClipboardEvent {
  std::uint8_t selection = 0U;
  std::string data;
  bool operator==(const TerminalClipboardEvent&) const = default;
};

/** Portable high-level command selected by the configured input Trie. */
struct TerminalCommandEvent {
  std::uint16_t command = 0U;
  constexpr bool operator==(const TerminalCommandEvent&) const noexcept =
      default;
};

/** Portable terminal-protocol response routed away from user input. */
struct TerminalResponseEvent {
  std::uint8_t kind   = 0U;
  std::uint32_t value = 0U;
  std::string bytes;
  bool operator==(const TerminalResponseEvent&) const = default;
};

/** Portable recoverable unknown-input diagnostic. */
struct TerminalUnknownEvent {
  std::uint8_t reason = 0U;
  std::string bytes;
  bool operator==(const TerminalUnknownEvent&) const = default;
};

/**
 * One normalized event published after terminal decoding and Trie lookup.
 *
 * \msg{puc::msg::TerminalInputEvent||Carries one portable normalized terminal
 * input event or protocol response.||The lifecycle-owned terminal input
 * producer.||The TUI Screen transport consumer.}
 */
struct TerminalInputEvent {
  using Data =
      std::variant<TerminalKeyEvent, TerminalTextEvent, TerminalMouseEvent,
                   TerminalScrollEvent, TerminalPasteEvent, TerminalFocusEvent,
                   TerminalClipboardEvent, TerminalCommandEvent,
                   TerminalResponseEvent, TerminalUnknownEvent>;
  Data data;
  bool operator==(const TerminalInputEvent&) const = default;
};

/** Return a stable name for the concrete terminal input alternative. */
std::string_view terminal_input_event_name(
    const TerminalInputEvent& event) noexcept;

}  // namespace puc::msg

namespace std {

/** Format one terminal input message as diagnostic JSON. */
template <>
struct formatter<puc::msg::TerminalInputEvent, char> {
  constexpr auto parse(format_parse_context& context) {
    return context.begin();
  }

  template <typename FormatContext>
  auto format(const puc::msg::TerminalInputEvent& event,
              FormatContext& context) const {
    return std::format_to(context.out(), "{{\"type\":\"{}\"}}",
                          puc::msg::terminal_input_event_name(event));
  }
};

}  // namespace std

namespace puc::msg {

/** Portable codec for TerminalInputEvent. */
class TerminalInputEventCodec final : public Codec<TerminalInputEvent> {
 public:
  /** Construct the codec under MessageId::TERMINAL_INPUT_EVENT. */
  constexpr TerminalInputEventCodec() noexcept
      : Codec(MessageId::TERMINAL_INPUT_EVENT) {}

 private:
  Status encode_payload(const TerminalInputEvent& event,
                        std::vector<std::uint8_t>& output) const override;
  Status decode_payload(std::span<const std::uint8_t> payload,
                        TerminalInputEvent& output) const override;
};

/** Register the normalized terminal-input codec in a collection. */
Status register_terminal_codecs(MessageCodecCollection& codecs);

}  // namespace puc::msg
