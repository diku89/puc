#pragma once

/**
 * @file screen_msgs.hpp
 * @brief One-way commands and observed state shared by Screen and the terminal.
 */

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "msgs/codec.hpp"

namespace puc::msg {

/**
 * Channel carrying commands from TUI presentation to terminal mechanism.
 *
 * \channel{//screen/commands||Carries ordered, one-way terminal ownership and
 * presentation operations without a result path.||
 * \ref puc::tui::Screen "Screen".||
 * \ref puc::terminal::TerminalSession "TerminalSession".}
 */
inline constexpr std::string_view kScreenCommandChannel = "//screen/commands";

/**
 * Channel publishing terminal geometry whenever the observation changes.
 *
 * \channel{//screen/resize_events||Publishes the newest observed character and
 * optional pixel geometry as convergent state.||
 * \ref puc::terminal::TerminalSession "TerminalSession".||
 * \ref puc::tui::Screen "Screen" and subscribed geometry observers.}
 */
inline constexpr std::string_view kScreenResizeEventChannel =
    "//screen/resize_events";

/** Request terminal ownership under PUC's fixed interactive mode contract. */
struct ScreenTakeCommand {
  std::string
      initial_bytes; /**< Screen-owned bytes written after mode setup. */
  std::string
      final_bytes; /**< Screen-owned bytes guaranteed before teardown. */

  /** Compare complete take requests. */
  bool operator==(const ScreenTakeCommand&) const = default;
};

/** Request restoration of every mode owned by the terminal session. */
struct ScreenReleaseCommand {
  /** Compare empty release requests. */
  constexpr bool operator==(const ScreenReleaseCommand&) const noexcept =
      default;
};

/** Present one complete, already-rendered terminal frame. */
struct ScreenPresentCommand {
  std::string bytes; /**< Owned terminal output bytes for one frame. */

  /** Compare complete frame byte streams. */
  bool operator==(const ScreenPresentCommand&) const = default;
};

/** Host-terminal clipboard selected by one asynchronous Screen command. */
enum class ScreenClipboardSelection : std::uint8_t {
  PRIMARY,   /**< Selection clipboard where the terminal supports one. */
  CLIPBOARD, /**< Conventional explicit copy/paste clipboard. */
};

/** Request that TerminalSession publish UTF-8 bytes through OSC 52. */
struct ScreenSetClipboardCommand {
  ScreenClipboardSelection selection =
      ScreenClipboardSelection::CLIPBOARD; /**< Destination clipboard. */
  std::string text; /**< Selected logical UTF-8, excluding visual padding. */

  /** Compare destination and complete clipboard bytes. */
  bool operator==(const ScreenSetClipboardCommand&) const = default;
};

/**
 * One ordered, fire-and-forget operation sent from Screen to TerminalSession.
 *
 * Commands intentionally contain no request identifier: the protocol has no
 * completion or error reply. Recoverable terminal observations converge on a
 * later command; an unrecoverable session failure terminates the process.
 *
 * \msg{puc::msg::ScreenCommand||Carries one fire-and-forget terminal take,
 * presentation, clipboard-write, or release operation.||
 * \ref puc::tui::Screen "Screen".||
 * \ref puc::terminal::TerminalSession "TerminalSession".}
 */
struct ScreenCommand {
  using Data = std::variant<ScreenTakeCommand, ScreenReleaseCommand,
                            ScreenPresentCommand,
                            ScreenSetClipboardCommand>; /**< Supported command
                                                            alternatives. */

  Data data; /**< Concrete operation carried by this command. */

  /** Compare command alternatives and their complete contents. */
  bool operator==(const ScreenCommand&) const = default;
};

/**
 * Terminal geometry observed independently of any individual command.
 *
 * \msg{puc::msg::ScreenResizeEvent||Publishes changed terminal geometry as
 * latest-value state rather than as a command reply.||
 * \ref puc::terminal::TerminalSession "TerminalSession".||
 * \ref puc::tui::Screen "Screen" and subscribed geometry observers.}
 */
struct ScreenResizeEvent {
  std::uint32_t width        = 0U; /**< Character-cell columns. */
  std::uint32_t height       = 0U; /**< Character-cell rows. */
  std::uint32_t pixel_width  = 0U; /**< Total pixel width, or zero. */
  std::uint32_t pixel_height = 0U; /**< Total pixel height, or zero. */

  /** Compare cell and optional pixel dimensions. */
  constexpr bool operator==(const ScreenResizeEvent&) const noexcept = default;
};

}  // namespace puc::msg

namespace std {

/** Format one ScreenCommand as complete, lossless JSON. */
template <>
struct formatter<puc::msg::ScreenCommand, char> {
  /** Accept only the formatter's empty format specification. */
  constexpr auto parse(format_parse_context& context) {
    return context.begin();
  }

  /** Write the concrete command and every field as lossless JSON. */
  template <typename FormatContext>
  auto format(const puc::msg::ScreenCommand& command,
              FormatContext& context) const {
    using namespace puc::msg;
    if (const auto* take = std::get_if<ScreenTakeCommand>(&command.data)) {
      auto output = std::format_to(
          context.out(), "{{\"type\":\"take\",\"initial_bytes_hex\":\"");
      constexpr char kHex[] = "0123456789abcdef";
      for (const unsigned char byte : take->initial_bytes) {
        *output++ = kHex[byte >> 4U];
        *output++ = kHex[byte & 0x0fU];
      }
      output = std::format_to(output, "\",\"final_bytes_hex\":\"");
      for (const unsigned char byte : take->final_bytes) {
        *output++ = kHex[byte >> 4U];
        *output++ = kHex[byte & 0x0fU];
      }
      return std::format_to(output, "\"}}");
    }
    if (std::holds_alternative<ScreenReleaseCommand>(command.data)) {
      return std::format_to(context.out(), "{{\"type\":\"release\"}}");
    }

    if (const auto* clipboard =
            std::get_if<ScreenSetClipboardCommand>(&command.data)) {
      auto output = std::format_to(
          context.out(),
          "{{\"type\":\"set_clipboard\",\"selection\":\"{}\","
          "\"text_hex\":\"",
          clipboard->selection == ScreenClipboardSelection::PRIMARY
              ? "primary"
              : "clipboard");
      constexpr char kHex[] = "0123456789abcdef";
      for (const unsigned char byte : clipboard->text) {
        *output++ = kHex[byte >> 4U];
        *output++ = kHex[byte & 0x0fU];
      }
      return std::format_to(output, "\"}}");
    }

    const auto& bytes     = std::get<ScreenPresentCommand>(command.data).bytes;
    auto output           = std::format_to(context.out(),
                                           "{{\"type\":\"present\",\"bytes_hex\":\"");
    constexpr char kHex[] = "0123456789abcdef";
    for (const unsigned char byte : bytes) {
      *output++ = kHex[byte >> 4U];
      *output++ = kHex[byte & 0x0fU];
    }
    return std::format_to(output, "\"}}");
  }
};

/** Format ScreenResizeEvent as complete JSON. */
template <>
struct formatter<puc::msg::ScreenResizeEvent, char> {
  /** Accept only the formatter's empty format specification. */
  constexpr auto parse(format_parse_context& context) {
    return context.begin();
  }

  /** Write all character-cell and optional pixel dimensions as JSON. */
  template <typename FormatContext>
  auto format(const puc::msg::ScreenResizeEvent& event,
              FormatContext& context) const {
    return std::format_to(context.out(),
                          "{{\"width\":{},\"height\":{},\"pixel_width\":{},"
                          "\"pixel_height\":{}}}",
                          event.width, event.height, event.pixel_width,
                          event.pixel_height);
  }
};

}  // namespace std

namespace puc::msg {

/** Portable payload codec for ScreenCommand. */
class ScreenCommandCodec final : public Codec<ScreenCommand> {
 public:
  /** Construct the codec under MessageId::SCREEN_COMMAND. */
  constexpr ScreenCommandCodec() noexcept : Codec(MessageId::SCREEN_COMMAND) {}

 private:
  Status encode_payload(const ScreenCommand& command,
                        std::vector<std::uint8_t>& output) const override;
  Status decode_payload(std::span<const std::uint8_t> payload,
                        ScreenCommand& output) const override;
};

/** Portable fixed-width payload codec for ScreenResizeEvent. */
class ScreenResizeEventCodec final : public Codec<ScreenResizeEvent> {
 public:
  /** Construct the codec under MessageId::SCREEN_RESIZE_EVENT. */
  constexpr ScreenResizeEventCodec() noexcept
      : Codec(MessageId::SCREEN_RESIZE_EVENT) {}

 private:
  Status encode_payload(const ScreenResizeEvent& event,
                        std::vector<std::uint8_t>& output) const override;
  Status decode_payload(std::span<const std::uint8_t> payload,
                        ScreenResizeEvent& output) const override;
};

/** Register every Screen/TerminalSession payload schema in a collection. */
Status register_screen_codecs(MessageCodecCollection& codecs);

}  // namespace puc::msg
