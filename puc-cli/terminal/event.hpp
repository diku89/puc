#pragma once

/**
 * @file event.hpp
 * @brief Terminal-independent input and terminal-response event types.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace puc {
namespace terminal {

/** Named keys that do not have an ordinary Unicode representation. */
enum class NamedKey {
  ESCAPE,
  ENTER,
  TAB,
  BACKSPACE,
  INSERT,
  DELETE_KEY,
  LEFT,
  RIGHT,
  UP,
  DOWN,
  PAGE_UP,
  PAGE_DOWN,
  HOME,
  END,
  CAPS_LOCK,
  SCROLL_LOCK,
  NUM_LOCK,
  PRINT_SCREEN,
  PAUSE,
  MENU,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  F13,
  F14,
  F15,
  F16,
  F17,
  F18,
  F19,
  F20,
  F21,
  F22,
  F23,
  F24,
  F25,
  F26,
  F27,
  F28,
  F29,
  F30,
  F31,
  F32,
  F33,
  F34,
  F35,
  KEYPAD_0,
  KEYPAD_1,
  KEYPAD_2,
  KEYPAD_3,
  KEYPAD_4,
  KEYPAD_5,
  KEYPAD_6,
  KEYPAD_7,
  KEYPAD_8,
  KEYPAD_9,
  KEYPAD_DECIMAL,
  KEYPAD_DIVIDE,
  KEYPAD_MULTIPLY,
  KEYPAD_SUBTRACT,
  KEYPAD_ADD,
  KEYPAD_ENTER,
  KEYPAD_EQUAL,
  KEYPAD_SEPARATOR,
  KEYPAD_LEFT,
  KEYPAD_RIGHT,
  KEYPAD_UP,
  KEYPAD_DOWN,
  KEYPAD_PAGE_UP,
  KEYPAD_PAGE_DOWN,
  KEYPAD_HOME,
  KEYPAD_END,
  KEYPAD_INSERT,
  KEYPAD_DELETE,
  KEYPAD_BEGIN,
  MEDIA_PLAY,
  MEDIA_PAUSE,
  MEDIA_PLAY_PAUSE,
  MEDIA_REVERSE,
  MEDIA_STOP,
  MEDIA_FAST_FORWARD,
  MEDIA_REWIND,
  MEDIA_TRACK_NEXT,
  MEDIA_TRACK_PREVIOUS,
  MEDIA_RECORD,
  VOLUME_DOWN,
  VOLUME_UP,
  VOLUME_MUTE,
  LEFT_SHIFT,
  LEFT_CONTROL,
  LEFT_ALT,
  LEFT_SUPER,
  LEFT_HYPER,
  LEFT_META,
  RIGHT_SHIFT,
  RIGHT_CONTROL,
  RIGHT_ALT,
  RIGHT_SUPER,
  RIGHT_HYPER,
  RIGHT_META,
  ISO_LEVEL3_SHIFT,
  ISO_LEVEL5_SHIFT,
};

/**
 * A logical key expressed either as a named functional key or Unicode scalar.
 *
 * This intentionally does not claim to be a physical scan code. Traditional
 * terminal protocols discard physical-key identity before PUC receives input.
 */
struct KeyCode {
  /** Named functional key or Unicode scalar carried by this value. */
  std::variant<NamedKey, char32_t> value = NamedKey::ESCAPE;

  /** Construct the Escape key used as the default value. */
  constexpr KeyCode() noexcept = default;

  /** Construct a key code from a named functional key. */
  constexpr KeyCode(NamedKey named_key) noexcept : value(named_key) {}

  /** Construct a key code from a Unicode scalar. */
  constexpr KeyCode(char32_t codepoint) noexcept : value(codepoint) {}

  /** Compare the contained key alternatives. */
  constexpr bool operator==(const KeyCode&) const noexcept = default;
};

/** Modifier bits reported by modern terminal keyboard protocols. */
enum class Modifier : std::uint16_t {
  SHIFT     = 1U << 0U,
  ALT       = 1U << 1U,
  CONTROL   = 1U << 2U,
  SUPER     = 1U << 3U,
  HYPER     = 1U << 4U,
  META      = 1U << 5U,
  CAPS_LOCK = 1U << 6U,
  NUM_LOCK  = 1U << 7U,
};

/** Small value type containing zero or more Modifier bits. */
class Modifiers {
 public:
  /** Construct an empty modifier set. */
  constexpr Modifiers() noexcept = default;

  /** Construct a set containing one modifier. */
  constexpr Modifiers(Modifier modifier) noexcept
      : bits_(static_cast<std::uint16_t>(modifier)) {}

  /** Construct a set from protocol-normalized modifier bits. */
  static constexpr Modifiers from_bits(std::uint16_t bits) noexcept {
    return Modifiers(bits);
  }

  /** Return the contained modifier bit field. */
  constexpr std::uint16_t bits() const noexcept { return bits_; }

  /** Test whether a modifier is present. */
  constexpr bool contains(Modifier modifier) const noexcept {
    return (bits_ & static_cast<std::uint16_t>(modifier)) != 0;
  }

  /** Add one modifier to this set. */
  constexpr Modifiers& add(Modifier modifier) noexcept {
    bits_ |= static_cast<std::uint16_t>(modifier);
    return *this;
  }

  /** Compare normalized modifier bit fields. */
  constexpr bool operator==(const Modifiers&) const noexcept = default;

 private:
  explicit constexpr Modifiers(std::uint16_t bits) noexcept : bits_(bits) {}

  std::uint16_t bits_ = 0; /**< Protocol-independent modifier mask. */
};

/** Combine two individual modifiers. */
constexpr Modifiers operator|(Modifier left, Modifier right) noexcept {
  return Modifiers(left).add(right);
}

/** Add an individual modifier to a modifier set. */
constexpr Modifiers operator|(Modifiers left, Modifier right) noexcept {
  return left.add(right);
}

/** Action associated with a key event. */
enum class KeyAction {
  PRESS,
  REPEAT,
  RELEASE,
};

/** A logical key event decoded from a legacy or extended keyboard protocol. */
struct KeyEvent {
  KeyCode key;                         /**< Logical key whose state changed. */
  Modifiers modifiers;                 /**< Modifiers active for this event. */
  KeyAction action = KeyAction::PRESS; /**< Press, repeat, or release action. */
  std::optional<char32_t>
      shifted_key; /**< Shifted alternate key, when known. */
  std::optional<char32_t>
      base_layout_key; /**< Base-layout alternate key, when known. */
  std::string text;    /**< Associated UTF-8 text from an extended protocol. */

  /** Compare every normalized key field. */
  bool operator==(const KeyEvent&) const = default;
};

/** UTF-8 text committed by a keyboard layout, IME, or legacy terminal. */
struct TextEvent {
  std::string utf8; /**< One or more complete UTF-8 scalars. */

  /** Compare committed UTF-8 bytes. */
  bool operator==(const TextEvent&) const = default;
};

/** Zero-based position in terminal character cells. */
struct CellPosition {
  std::size_t x = 0; /**< Zero-based column. */
  std::size_t y = 0; /**< Zero-based row. */

  /** Compare both cell coordinates. */
  constexpr bool operator==(const CellPosition&) const noexcept = default;
};

/** Mouse buttons represented by xterm SGR mouse reports. */
enum class MouseButton {
  NONE,
  LEFT,
  MIDDLE,
  RIGHT,
  AUXILIARY_1,
  AUXILIARY_2,
  AUXILIARY_3,
  AUXILIARY_4,
};

/** Mouse actions normalized independently of the wire encoding. */
enum class MouseAction {
  PRESS,
  RELEASE,
  MOVE,
  DRAG,
};

/** Mouse button or motion event at a cell position. */
struct MouseEvent {
  CellPosition position;                  /**< Cell containing the pointer. */
  MouseButton button = MouseButton::NONE; /**< Button associated with motion. */
  MouseAction action =
      MouseAction::MOVE; /**< Normalized button or motion action. */
  Modifiers modifiers;   /**< Active keyboard modifiers. */

  /** Compare every normalized mouse field. */
  constexpr bool operator==(const MouseEvent&) const noexcept = default;
};

/**
 * Wheel motion normalized from xterm's synthetic mouse buttons.
 *
 * Positive vertical deltas move up and positive horizontal deltas move right.
 */
struct ScrollEvent {
  CellPosition position; /**< Cell containing the pointer. */
  int delta_x = 0;       /**< Horizontal wheel steps; right is positive. */
  int delta_y = 0;       /**< Vertical wheel steps; up is positive. */
  Modifiers modifiers;   /**< Active keyboard modifiers. */

  /** Compare position, deltas, and modifiers. */
  constexpr bool operator==(const ScrollEvent&) const noexcept = default;
};

/** Stage of a streaming bracketed-paste operation. */
enum class PastePhase {
  BEGIN,
  DATA,
  END,
  CANCEL,
};

/** One stage or data chunk from a bracketed paste. */
struct PasteEvent {
  PastePhase phase = PastePhase::BEGIN; /**< Stage represented by this event. */
  std::string data; /**< Uninterpreted bytes carried by a DATA event. */

  /** Compare the paste phase and payload bytes. */
  bool operator==(const PasteEvent&) const = default;
};

/** Terminal focus transition. */
struct FocusEvent {
  bool focused = false; /**< Whether the terminal gained focus. */

  /** Compare focus states. */
  constexpr bool operator==(const FocusEvent&) const noexcept = default;
};

/** Clipboard selections supported by the initial OSC 52 implementation. */
enum class ClipboardSelection {
  CLIPBOARD,
  PRIMARY,
};

/** Decoded response to an asynchronous clipboard query. */
struct ClipboardEvent {
  ClipboardSelection selection =
      ClipboardSelection::CLIPBOARD; /**< Selection that supplied the data. */
  std::string data;                  /**< Decoded clipboard bytes. */

  /** Compare selection and decoded bytes. */
  bool operator==(const ClipboardEvent&) const = default;
};

/** High-level application commands selected by configured input sequences. */
enum class Command {
  COPY,       /**< Copy the application's completed logical selection. */
  SELECT_ALL, /**< Select all logical text in the focused application field. */
  ENTER_COMMAND_MODE,  /**< Replace the active editor with a command buffer. */
  ENTER_TERMINAL_MODE, /**< Show the libtmt-backed terminal surface. */
  MOVE_WORD_LEFT,      /**< Move an editor caret to the preceding word. */
  MOVE_WORD_RIGHT,     /**< Move an editor caret to the following word. */
  MOVE_ROW_START,      /**< Move an editor caret to the start of its row. */
  MOVE_ROW_END,        /**< Move an editor caret to the end of its row. */
  MOVE_BUFFER_START,   /**< Move an editor caret to the beginning of input. */
  MOVE_BUFFER_END,     /**< Move an editor caret to the end of input. */
  MOVE_PAGE_UP,        /**< Move an editor caret up one visible page. */
  MOVE_PAGE_DOWN,      /**< Move an editor caret down one visible page. */
};

/**
 * A configurable semantic command emitted directly by the input Trie.
 *
 * Unlike KeyEvent, this event no longer describes the physical key chord.
 * The active operating-system defaults, terminal profile, and user overrides
 * have already resolved that chord to application intent.
 */
struct CommandEvent {
  Command command = Command::COPY; /**< Requested application operation. */

  /** Compare normalized command identities. */
  constexpr bool operator==(const CommandEvent&) const noexcept = default;
};

/** Kinds of terminal protocol replies routed away from user key input. */
enum class TerminalResponseKind {
  KITTY_KEYBOARD_FLAGS,
  CURSOR_POSITION,
  DEVICE_ATTRIBUTES,
  MODE_REPORT,
  OPERATING_SYSTEM_COMMAND,
  DEVICE_CONTROL_STRING,
};

/** Terminal response that should be consumed by protocol orchestration. */
struct TerminalResponseEvent {
  TerminalResponseKind kind =
      TerminalResponseKind::OPERATING_SYSTEM_COMMAND; /**< Reply category. */
  std::uint32_t value = 0; /**< Parsed numeric value, when applicable. */
  std::string bytes;       /**< Original or remaining protocol bytes. */

  /** Compare the category and retained response data. */
  bool operator==(const TerminalResponseEvent&) const = default;
};

/** Reason bytes could not be normalized into a supported event. */
enum class UnknownInputReason {
  UNSUPPORTED_SEQUENCE,
  MALFORMED_SEQUENCE,
  INVALID_UTF8,
  INCOMPLETE_SEQUENCE,
  LIMIT_EXCEEDED,
};

/** Recoverable unrecognized or malformed input retained for diagnostics. */
struct UnknownEvent {
  UnknownInputReason reason =
      UnknownInputReason::UNSUPPORTED_SEQUENCE; /**< Why decoding stopped. */
  std::string bytes; /**< Safe bytes retained when diagnostics benefit. */

  /** Compare the reason and retained bytes. */
  bool operator==(const UnknownEvent&) const = default;
};

/** Every semantic input or protocol-reply event emitted by Decoder. */
using Event = std::variant<KeyEvent, TextEvent, MouseEvent, ScrollEvent,
                           PasteEvent, FocusEvent, ClipboardEvent, CommandEvent,
                           TerminalResponseEvent, UnknownEvent>;

}  // namespace terminal
}  // namespace puc
