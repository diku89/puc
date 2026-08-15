/**
 * @file decoder.cpp
 * @brief Streaming UTF-8, keyboard, mouse, paste, and response decoder.
 */

#include "puc-cli/tui/terminal/decoder.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "puc-cli/tui/terminal/clipboard.hpp"
#include "utils/logger/logger.hpp"

/** @cond TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("TerminalDecoder");
/** @endcond */

namespace puc {
namespace terminal {
namespace {

constexpr char kEscape = '\x1b';

/** Return a safe encoded-size ceiling for an OSC 52 clipboard response. */
std::size_t maximum_osc52_sequence_bytes(std::size_t decoded_bytes) noexcept {
  if (decoded_bytes > (std::numeric_limits<std::size_t>::max() - 2U) / 3U) {
    return std::numeric_limits<std::size_t>::max();
  }
  const std::size_t encoded                  = ((decoded_bytes + 2U) / 3U) * 4U;
  constexpr std::size_t kMaximumFramingBytes = 24U;
  if (encoded >
      std::numeric_limits<std::size_t>::max() - kMaximumFramingBytes) {
    return std::numeric_limits<std::size_t>::max();
  }
  return encoded + kMaximumFramingBytes;
}

/** Result of decoding exactly one UTF-8 scalar from a byte prefix. */
enum class Utf8Result {
  VALID,
  INCOMPLETE,
  INVALID,
};

/** Test whether a numeric code point is a Unicode scalar value. */
constexpr bool is_unicode_scalar(std::uint32_t codepoint) noexcept {
  return codepoint <= 0x10ffffU &&
         !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
}

/** Decode one strict UTF-8 scalar without reading past the supplied view. */
Utf8Result decode_utf8(std::string_view bytes, char32_t& codepoint,
                       std::size_t& length) noexcept {
  length = 0;
  if (bytes.empty()) {
    return Utf8Result::INCOMPLETE;
  }

  const auto byte = [&bytes](std::size_t index) {
    return static_cast<unsigned char>(bytes[index]);
  };
  const unsigned char first = byte(0);
  if (first <= 0x7fU) {
    codepoint = static_cast<char32_t>(first);
    length    = 1;
    return Utf8Result::VALID;
  }

  std::size_t expected = 0;
  std::uint32_t value  = 0;
  if (first >= 0xc2U && first <= 0xdfU) {
    expected = 2;
    value    = first & 0x1fU;
  } else if (first >= 0xe0U && first <= 0xefU) {
    expected = 3;
    value    = first & 0x0fU;
  } else if (first >= 0xf0U && first <= 0xf4U) {
    expected = 4;
    value    = first & 0x07U;
  } else {
    return Utf8Result::INVALID;
  }

  const std::size_t available = std::min(bytes.size(), expected);
  for (std::size_t index = 1; index < available; ++index) {
    if ((byte(index) & 0xc0U) != 0x80U) {
      return Utf8Result::INVALID;
    }
  }
  if (bytes.size() < expected) {
    return Utf8Result::INCOMPLETE;
  }

  if ((first == 0xe0U && byte(1) < 0xa0U) ||
      (first == 0xedU && byte(1) >= 0xa0U) ||
      (first == 0xf0U && byte(1) < 0x90U) ||
      (first == 0xf4U && byte(1) >= 0x90U)) {
    return Utf8Result::INVALID;
  }

  for (std::size_t index = 1; index < expected; ++index) {
    value = (value << 6U) | (byte(index) & 0x3fU);
  }
  if (!is_unicode_scalar(value)) {
    return Utf8Result::INVALID;
  }

  codepoint = static_cast<char32_t>(value);
  length    = expected;
  return Utf8Result::VALID;
}

/** Append one already validated Unicode scalar as UTF-8. */
void append_utf8(char32_t character, std::string& output) {
  const std::uint32_t codepoint = static_cast<std::uint32_t>(character);
  if (codepoint <= 0x7fU) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else if (codepoint <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
  }
}

/** Parse one non-empty decimal unsigned integer with overflow checking. */
bool parse_unsigned(std::string_view text, std::uint32_t& value) noexcept {
  if (text.empty()) {
    return false;
  }
  value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

/** Split a bounded protocol field while preserving empty sub-fields. */
template <std::size_t MaximumFields>
bool split_fields(std::string_view text, char delimiter,
                  std::array<std::string_view, MaximumFields>& fields,
                  std::size_t& count) noexcept {
  count = 0;
  if (text.empty()) {
    return true;
  }
  std::size_t begin = 0;
  while (true) {
    if (count == MaximumFields) {
      return false;
    }
    const std::size_t end = text.find(delimiter, begin);
    fields[count++] =
        text.substr(begin, end == std::string_view::npos ? end : end - begin);
    if (end == std::string_view::npos) {
      return true;
    }
    begin = end + 1U;
  }
}

/** Decode Kitty/xterm's one-plus-bit-field modifier representation. */
bool parse_modifiers_and_action(std::string_view field, Modifiers& modifiers,
                                KeyAction& action) noexcept {
  modifiers = {};
  action    = KeyAction::PRESS;
  if (field.empty()) {
    return true;
  }

  std::array<std::string_view, 2> parts{};
  std::size_t count = 0;
  if (!split_fields(field, ':', parts, count) || count == 0) {
    return false;
  }

  std::uint32_t encoded_modifiers = 1;
  if (!parts[0].empty() &&
      (!parse_unsigned(parts[0], encoded_modifiers) || encoded_modifiers < 1U ||
       encoded_modifiers > 256U)) {
    return false;
  }
  modifiers =
      Modifiers::from_bits(static_cast<std::uint16_t>(encoded_modifiers - 1U));

  if (count == 2U) {
    std::uint32_t encoded_action = 0;
    if (!parse_unsigned(parts[1], encoded_action) || encoded_action < 1U ||
        encoded_action > 3U) {
      return false;
    }
    action = encoded_action == 1U   ? KeyAction::PRESS
             : encoded_action == 2U ? KeyAction::REPEAT
                                    : KeyAction::RELEASE;
  }
  return true;
}

/** Map Kitty's private-use functional key codes into public named keys. */
std::optional<NamedKey> kitty_named_key(std::uint32_t code) noexcept {
  switch (code) {
    case 9:
      return NamedKey::TAB;
    case 13:
      return NamedKey::ENTER;
    case 27:
      return NamedKey::ESCAPE;
    case 127:
      return NamedKey::BACKSPACE;
    case 57348:
      return NamedKey::INSERT;
    case 57349:
      return NamedKey::DELETE_KEY;
    case 57350:
      return NamedKey::LEFT;
    case 57351:
      return NamedKey::RIGHT;
    case 57352:
      return NamedKey::UP;
    case 57353:
      return NamedKey::DOWN;
    case 57354:
      return NamedKey::PAGE_UP;
    case 57355:
      return NamedKey::PAGE_DOWN;
    case 57356:
      return NamedKey::HOME;
    case 57357:
      return NamedKey::END;
    case 57358:
      return NamedKey::CAPS_LOCK;
    case 57359:
      return NamedKey::SCROLL_LOCK;
    case 57360:
      return NamedKey::NUM_LOCK;
    case 57361:
      return NamedKey::PRINT_SCREEN;
    case 57362:
      return NamedKey::PAUSE;
    case 57363:
      return NamedKey::MENU;
    default:
      break;
  }

  if (code >= 57364U && code <= 57375U) {
    return static_cast<NamedKey>(static_cast<unsigned int>(NamedKey::F1) +
                                 (code - 57364U));
  }
  if (code >= 57376U && code <= 57398U) {
    return static_cast<NamedKey>(static_cast<unsigned int>(NamedKey::F13) +
                                 (code - 57376U));
  }
  if (code >= 57399U && code <= 57454U) {
    return static_cast<NamedKey>(static_cast<unsigned int>(NamedKey::KEYPAD_0) +
                                 (code - 57399U));
  }
  return std::nullopt;
}

/** Convert a protocol numeric key into a normalized logical key. */
bool protocol_key_code(std::uint32_t code, KeyCode& key) noexcept {
  if (const std::optional<NamedKey> named = kitty_named_key(code)) {
    key = *named;
    return true;
  }
  if (code == 0U || !is_unicode_scalar(code) ||
      (code >= 57344U && code <= 63743U)) {
    return false;
  }
  key = static_cast<char32_t>(code);
  return true;
}

/** Map the first parameter of a legacy CSI-tilde key sequence. */
std::optional<NamedKey> tilde_named_key(std::uint32_t code) noexcept {
  switch (code) {
    case 1:
    case 7:
      return NamedKey::HOME;
    case 2:
      return NamedKey::INSERT;
    case 3:
      return NamedKey::DELETE_KEY;
    case 4:
    case 8:
      return NamedKey::END;
    case 5:
      return NamedKey::PAGE_UP;
    case 6:
      return NamedKey::PAGE_DOWN;
    case 11:
      return NamedKey::F1;
    case 12:
      return NamedKey::F2;
    case 13:
      return NamedKey::F3;
    case 14:
      return NamedKey::F4;
    case 15:
      return NamedKey::F5;
    case 17:
      return NamedKey::F6;
    case 18:
      return NamedKey::F7;
    case 19:
      return NamedKey::F8;
    case 20:
      return NamedKey::F9;
    case 21:
      return NamedKey::F10;
    case 23:
      return NamedKey::F11;
    case 24:
      return NamedKey::F12;
    case 57427:
      return NamedKey::KEYPAD_BEGIN;
    default:
      return std::nullopt;
  }
}

/** Decode an Alt-prefixed traditional C0 key encoding. */
std::optional<KeyEvent> legacy_control_key(unsigned char byte) noexcept {
  switch (byte) {
    case 8:
    case 127:
      return KeyEvent{.key = NamedKey::BACKSPACE};
    case 9:
      return KeyEvent{.key = NamedKey::TAB};
    case 10:
    case 13:
      return KeyEvent{.key = NamedKey::ENTER};
    default:
      break;
  }

  char32_t character = U'\0';
  if (byte == 0U) {
    character = U' ';
  } else if (byte >= 1U && byte <= 26U) {
    character = static_cast<char32_t>(U'a' + byte - 1U);
  } else if (byte >= 28U && byte <= 31U) {
    constexpr char32_t kControlCharacters[] = {U'\\', U']', U'^', U'_'};
    character                               = kControlCharacters[byte - 28U];
  } else {
    return std::nullopt;
  }

  return KeyEvent{
      .key       = character,
      .modifiers = Modifier::CONTROL,
  };
}

/** Return an event containing unsupported bytes without interpreting them. */
UnknownEvent unknown(UnknownInputReason reason, std::string_view bytes) {
  return UnknownEvent{.reason = reason, .bytes = std::string{bytes}};
}

/** Map a CSI final character into a traditional named key. */
std::optional<NamedKey> csi_final_key(char final) noexcept {
  switch (final) {
    case 'A':
      return NamedKey::UP;
    case 'B':
      return NamedKey::DOWN;
    case 'C':
      return NamedKey::RIGHT;
    case 'D':
      return NamedKey::LEFT;
    case 'H':
      return NamedKey::HOME;
    case 'F':
      return NamedKey::END;
    case 'P':
      return NamedKey::F1;
    case 'Q':
      return NamedKey::F2;
    case 'S':
      return NamedKey::F4;
    default:
      return std::nullopt;
  }
}

/** Decode the modifier and optional event type on a CSI letter key. */
bool parse_letter_key_fields(std::string_view body, Modifiers& modifiers,
                             KeyAction& action) noexcept {
  std::array<std::string_view, 2> fields{};
  std::size_t count = 0;
  if (!split_fields(body, ';', fields, count)) {
    return false;
  }
  if (count == 0U) {
    modifiers = {};
    action    = KeyAction::PRESS;
    return true;
  }

  std::uint32_t first = 0;
  if (!parse_unsigned(fields[0], first) || first != 1U) {
    return false;
  }
  if (count == 1U) {
    modifiers = {};
    action    = KeyAction::PRESS;
    return true;
  }
  return parse_modifiers_and_action(fields[1], modifiers, action);
}

/** Map an xterm mouse button number outside the three primary buttons. */
MouseButton auxiliary_button(std::uint32_t index) noexcept {
  switch (index) {
    case 0:
      return MouseButton::AUXILIARY_1;
    case 1:
      return MouseButton::AUXILIARY_2;
    case 2:
      return MouseButton::AUXILIARY_3;
    default:
      return MouseButton::AUXILIARY_4;
  }
}

}  // namespace

Decoder::Decoder() : Decoder(DecoderLimits{}) {}

Decoder::Decoder(DecoderLimits limits) : limits_(limits) {
  if (limits_.maximum_pending_bytes == 0U) {
    limits_.maximum_pending_bytes = 1U;
  }
  if (limits_.maximum_sequence_bytes == 0U) {
    limits_.maximum_sequence_bytes = 1U;
  }
}

Decoder::Decoder(InputMap input_map, DecoderLimits limits) : Decoder(limits) {
  static_cast<void>(input_map.find_protocol_sequence(InputProtocol::PASTE_END,
                                                     paste_end_sequence_));
  input_trie_ = input_map.take_trie();
}

Status Decoder::setup(properties::Properties& properties,
                      std::string_view terminal_name, int output_fd) {
  InputMap input_map;
  const Status status =
      InputMap::setup(properties, input_map, terminal_name, output_fd);
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not set up terminal input decoder: "
                  << status_message(status);
    return status;
  }
  Decoder candidate{std::move(input_map), limits_};
  *this = std::move(candidate);
  return Status::OK;
}

Status Decoder::feed(std::string_view bytes, std::vector<Event>& events) {
  compact();
  const std::size_t retained =
      std::min(pending_bytes(), limits_.maximum_pending_bytes);
  if (bytes.size() > limits_.maximum_pending_bytes - retained) {
    events.emplace_back(
        UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
    Logger<WARN> << "Rejected terminal input block of " << bytes.size()
                 << " bytes because pending input would exceed "
                 << limits_.maximum_pending_bytes << " bytes";
    return Status::INPUT_LIMIT_EXCEEDED;
  }
  buffer_.append(bytes);
  const Status status = process(events);
  compact();
  update_pending_timeout(!bytes.empty());
  return status;
}

Status Decoder::resolve_timed_out_active_action(std::vector<Event>& events) {
  const EnterInputProtocol* protocol = active_protocol();
  if (protocol == nullptr) {
    reset_active_action();
    return Status::INVALID_ARGUMENT;
  }
  if (paste_in_progress_) {
    return Status::OK;
  }

  if (active_sequence_.empty() || active_sequence_.front() != kEscape) {
    std::string incomplete = std::move(active_sequence_);
    incomplete.append(pending());
    consume(pending().size());
    events.emplace_back(
        unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, incomplete));
    reset_active_action();
    reset_trie_cursor();
    return Status::OK;
  }

  std::string replay{pending()};
  if (protocol->protocol == InputProtocol::ESCAPE) {
    events.emplace_back(KeyEvent{.key = NamedKey::ESCAPE});
    reset_active_action();
  } else {
    replay.insert(0, active_sequence_.substr(1U));
    const InputTrie::NodeIndex escape =
        input_trie_.find_child(InputTrie::root(), kEscape);
    reset_active_action();
    if (escape == InputTrie::kInvalidNode) {
      events.emplace_back(KeyEvent{.key = NamedKey::ESCAPE});
    } else {
      active_action_node_ = escape;
      active_sequence_.assign(1U, kEscape);
    }
  }
  buffer_ = std::move(replay);
  offset_ = 0U;
  reset_trie_cursor();
  return process(events);
}

Status Decoder::handle_timeout(TimeoutInput input, std::vector<Event>& events) {
  if (!pending_timeout_.has_value() || input.generation == 0U ||
      input != *pending_timeout_) {
    return Status::OK;
  }
  pending_timeout_.reset();
  if (paste_in_progress_) {
    return Status::OK;
  }

  Status status = Status::OK;
  if (active_action_node_ != InputTrie::kInvalidNode) {
    status = resolve_timed_out_active_action(events);
  } else {
    const std::string_view bytes = pending();
    if (bytes.empty()) {
      return Status::OK;
    }
    if (trie_cursor_.last_match != InputTrie::kInvalidNode) {
      const bool matched_all = trie_cursor_.last_match_size == bytes.size();
      ParseResult result     = ParseResult::NEED_MORE;
      status                 = execute_action(trie_cursor_.last_match,
                                              trie_cursor_.last_match_size, events, result);
      if (is_ok(status) && matched_all &&
          active_action_node_ != InputTrie::kInvalidNode) {
        status = resolve_timed_out_active_action(events);
      }
      if (is_ok(status)) {
        status = process(events);
      }
      if (is_ok(status) && active_action_node_ != InputTrie::kInvalidNode) {
        status = resolve_timed_out_active_action(events);
      }
    } else {
      const std::string incomplete{bytes};
      consume(bytes.size());
      reset_trie_cursor();
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, incomplete));
    }
  }

  compact();
  update_pending_timeout(true);
  return status;
}

Status Decoder::finish(std::vector<Event>& events) {
  Status result = Status::OK;
  if (paste_in_progress_) {
    std::string remaining = std::move(paste_candidate_);
    remaining.append(pending());
    consume(pending().size());
    const Status data_status = append_paste_data(remaining, events);
    if (!is_ok(data_status)) {
      result = data_status;
    }
    if (!paste_discarding_) {
      events.emplace_back(PasteEvent{.phase = PastePhase::CANCEL});
    }
    paste_in_progress_ = false;
    paste_discarding_  = false;
    paste_bytes_       = 0;
    reset_active_action();
  } else if (active_action_node_ != InputTrie::kInvalidNode) {
    std::string incomplete = std::move(active_sequence_);
    incomplete.append(pending());
    consume(pending().size());
    events.emplace_back(
        unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, incomplete));
    reset_active_action();
  } else if (!pending().empty()) {
    const std::string_view bytes = pending();
    if (trie_cursor_.node != InputTrie::kRootNode &&
        trie_cursor_.scanned == bytes.size()) {
      const InputTrie::Node& current = input_trie_.node(trie_cursor_.node);
      if (current.sequence_end && current.value.event() != nullptr) {
        ParseResult parse_result = ParseResult::NEED_MORE;
        result = execute_action(trie_cursor_.node, bytes.size(), events,
                                parse_result);
      }
    }
    if (!pending().empty() && pending().size() == 1U &&
        pending().front() == kEscape) {
      events.emplace_back(KeyEvent{.key = NamedKey::ESCAPE});
      consume(1U);
    }
    if (!pending().empty()) {
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, pending()));
      consume(pending().size());
    }
  }
  reset_trie_cursor();
  compact();
  pending_timeout_.reset();
  return result;
}

void Decoder::reset() noexcept {
  buffer_.clear();
  offset_            = 0;
  paste_in_progress_ = false;
  paste_discarding_  = false;
  paste_bytes_       = 0;
  paste_candidate_.clear();
  reset_trie_cursor();
  reset_active_action();
  pending_timeout_.reset();
}

std::size_t Decoder::pending_bytes() const noexcept {
  return pending().size() + active_sequence_.size() + paste_candidate_.size();
}

void Decoder::update_pending_timeout(bool rearm) noexcept {
  const bool needs_timeout =
      !paste_in_progress_ &&
      (active_action_node_ != InputTrie::kInvalidNode || !pending().empty());
  if (!needs_timeout) {
    pending_timeout_.reset();
    return;
  }
  if (!rearm && pending_timeout_.has_value()) {
    return;
  }
  ++next_timeout_generation_;
  if (next_timeout_generation_ == 0U) {
    ++next_timeout_generation_;
  }
  pending_timeout_ = TimeoutInput{.generation = next_timeout_generation_};
}

Status Decoder::process(std::vector<Event>& events) {
  Status first_error = Status::OK;
  while (!pending().empty()) {
    ParseResult result  = ParseResult::NEED_MORE;
    const Status status = active_action_node_ == InputTrie::kInvalidNode
                              ? advance_trie(events, result)
                              : advance_active_action(events, result);
    if (!is_ok(status) && is_ok(first_error)) {
      first_error = status;
    }
    if (result == ParseResult::NEED_MORE) {
      break;
    }
  }
  return first_error;
}

Status Decoder::advance_trie(std::vector<Event>& events, ParseResult& result) {
  const std::string_view bytes = pending();
  while (trie_cursor_.scanned < bytes.size()) {
    const char byte = bytes[trie_cursor_.scanned];
    const InputTrie::NodeIndex child =
        input_trie_.find_child(trie_cursor_.node, byte);
    if (child == InputTrie::kInvalidNode) {
      if (trie_cursor_.last_match != InputTrie::kInvalidNode) {
        return execute_action(trie_cursor_.last_match,
                              trie_cursor_.last_match_size, events, result);
      }
      if (trie_cursor_.scanned != 0U) {
        events.emplace_back(unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE,
                                    bytes.substr(0, trie_cursor_.scanned)));
        consume(trie_cursor_.scanned);
        reset_trie_cursor();
        result = ParseResult::CONSUMED;
        return Status::OK;
      }
      const InputTrie::Node& root = input_trie_.node(InputTrie::root());
      if (root.sequence_end) {
        return execute_action(InputTrie::root(), 0U, events, result);
      }
      events.emplace_back(unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE,
                                  bytes.substr(0, 1U)));
      consume(1U);
      reset_trie_cursor();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }

    trie_cursor_.node = child;
    ++trie_cursor_.scanned;
    const InputTrie::Node& node = input_trie_.node(child);
    if (node.sequence_end) {
      trie_cursor_.last_match      = child;
      trie_cursor_.last_match_size = trie_cursor_.scanned;
    }
    if (node.sequence_end && node.children.empty()) {
      return execute_action(child, trie_cursor_.scanned, events, result);
    }
    if (!node.sequence_end && node.children.empty()) {
      events.emplace_back(unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE,
                                  bytes.substr(0, trie_cursor_.scanned)));
      consume(trie_cursor_.scanned);
      reset_trie_cursor();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
  }
  result = ParseResult::NEED_MORE;
  return Status::OK;
}

Status Decoder::execute_action(InputTrie::NodeIndex node,
                               std::size_t byte_count,
                               std::vector<Event>& events,
                               ParseResult& result) {
  const InputAction& action = input_trie_.node(node).value;
  if (const Event* event = action.event()) {
    events.push_back(*event);
    consume(byte_count);
    reset_trie_cursor();
    result = ParseResult::CONSUMED;
    return Status::OK;
  }

  const EnterInputProtocol* protocol = action.protocol();
  if (protocol == nullptr) {
    const std::string bytes{pending().substr(0, byte_count)};
    events.emplace_back(
        unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, bytes));
    consume(byte_count == 0U ? 1U : byte_count);
    reset_trie_cursor();
    result = ParseResult::CONSUMED;
    return Status::OK;
  }

  if (protocol->protocol == InputProtocol::TEXT) {
    reset_trie_cursor();
    result = parse_text(events);
    return Status::OK;
  }

  const std::string matched{pending().substr(0, byte_count)};
  consume(byte_count);
  reset_trie_cursor();
  if (protocol->protocol == InputProtocol::PASTE_BEGIN) {
    events.emplace_back(PasteEvent{.phase = PastePhase::BEGIN});
    active_action_node_ = node;
    active_sequence_.clear();
    paste_candidate_.clear();
    paste_in_progress_ = true;
    paste_discarding_  = false;
    paste_bytes_       = 0;
  } else if (protocol->protocol == InputProtocol::PASTE_END) {
    events.emplace_back(
        unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, matched));
  } else {
    active_action_node_ = node;
    active_sequence_    = matched;
  }
  result = ParseResult::CONSUMED;
  return Status::OK;
}

Status Decoder::advance_active_action(std::vector<Event>& events,
                                      ParseResult& result) {
  const EnterInputProtocol* action = active_protocol();
  if (action == nullptr) {
    reset_active_action();
    result = ParseResult::CONSUMED;
    return Status::INVALID_ARGUMENT;
  }
  switch (action->protocol) {
    case InputProtocol::TEXT:
      result = parse_text(events);
      return Status::OK;
    case InputProtocol::ESCAPE:
      return parse_alt_key(events, result);
    case InputProtocol::SS3:
      return capture_ss3(events, result);
    case InputProtocol::CSI:
    case InputProtocol::SGR_MOUSE:
      return capture_csi(events, result);
    case InputProtocol::OSC:
    case InputProtocol::OSC52:
      return capture_osc(events, result);
    case InputProtocol::DEVICE_CONTROL_STRING:
    case InputProtocol::APPLICATION_PROGRAM_COMMAND:
    case InputProtocol::PRIVACY_MESSAGE:
      return capture_st_string(events, result);
    case InputProtocol::PASTE_BEGIN:
      return capture_paste(events, result);
    case InputProtocol::PASTE_END:
      break;
  }
  reset_active_action();
  result = ParseResult::CONSUMED;
  return Status::INVALID_ARGUMENT;
}

Status Decoder::parse_alt_key(std::vector<Event>& events, ParseResult& result) {
  const std::string_view bytes = pending();
  if (bytes.empty()) {
    result = ParseResult::NEED_MORE;
    return Status::OK;
  }

  const unsigned char first = static_cast<unsigned char>(bytes.front());
  if (first == static_cast<unsigned char>(kEscape)) {
    events.emplace_back(KeyEvent{
        .key       = NamedKey::ESCAPE,
        .modifiers = Modifier::ALT,
    });
    consume(1U);
    reset_active_action();
    result = ParseResult::CONSUMED;
    return Status::OK;
  }
  if (const std::optional<KeyEvent> control = legacy_control_key(first)) {
    KeyEvent event = *control;
    event.modifiers.add(Modifier::ALT);
    events.emplace_back(std::move(event));
    consume(1U);
    reset_active_action();
    result = ParseResult::CONSUMED;
    return Status::OK;
  }

  char32_t codepoint    = U'\0';
  std::size_t length    = 0;
  const Utf8Result utf8 = decode_utf8(bytes, codepoint, length);
  if (utf8 == Utf8Result::INCOMPLETE) {
    result = ParseResult::NEED_MORE;
    return Status::OK;
  }
  if (utf8 == Utf8Result::INVALID) {
    std::string malformed = active_sequence_;
    malformed.push_back(bytes.front());
    events.emplace_back(unknown(UnknownInputReason::INVALID_UTF8, malformed));
    consume(1U);
  } else {
    events.emplace_back(KeyEvent{
        .key       = codepoint,
        .modifiers = Modifier::ALT,
    });
    consume(length);
  }
  reset_active_action();
  result = ParseResult::CONSUMED;
  return Status::OK;
}

Status Decoder::capture_ss3(std::vector<Event>& events, ParseResult& result) {
  while (!pending().empty()) {
    const unsigned char byte = static_cast<unsigned char>(pending().front());
    if (byte == static_cast<unsigned char>(kEscape)) {
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    active_sequence_.push_back(pending().front());
    consume(1U);

    if (byte >= 0x40U && byte <= 0x7eU) {
      events.emplace_back(
          unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    if (byte < 0x20U || byte > 0x3fU) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    if (active_sequence_.size() >= limits_.maximum_sequence_bytes) {
      events.emplace_back(
          UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::INPUT_LIMIT_EXCEEDED;
    }
  }
  result = ParseResult::NEED_MORE;
  return Status::OK;
}

Status Decoder::capture_csi(std::vector<Event>& events, ParseResult& result) {
  const EnterInputProtocol* action = active_protocol();
  const bool sgr_mouse =
      action != nullptr && action->protocol == InputProtocol::SGR_MOUSE;
  while (!pending().empty()) {
    const unsigned char byte = static_cast<unsigned char>(pending().front());
    if (byte == static_cast<unsigned char>(kEscape)) {
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    active_sequence_.push_back(pending().front());
    consume(1U);

    if (byte >= 0x40U && byte <= 0x7eU) {
      const bool sgr_mouse_report =
          sgr_mouse && (byte == static_cast<unsigned char>('M') ||
                        byte == static_cast<unsigned char>('m'));
      const Status status = sgr_mouse_report
                                ? parse_sgr_mouse(active_sequence_, events)
                                : parse_csi(active_sequence_, events);
      reset_active_action();
      result = ParseResult::CONSUMED;
      return status;
    }
    if (byte < 0x20U || byte > 0x3fU) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    if (active_sequence_.size() >= limits_.maximum_sequence_bytes) {
      events.emplace_back(
          UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::INPUT_LIMIT_EXCEEDED;
    }
  }
  result = ParseResult::NEED_MORE;
  return Status::OK;
}

Status Decoder::capture_osc(std::vector<Event>& events, ParseResult& result) {
  const EnterInputProtocol* action = active_protocol();
  const std::size_t sequence_limit =
      action != nullptr && action->protocol == InputProtocol::OSC52
          ? maximum_osc52_sequence_bytes(limits_.maximum_clipboard_bytes)
          : limits_.maximum_sequence_bytes;
  while (!pending().empty()) {
    const char byte = pending().front();
    if (byte == kEscape) {
      if (pending().size() == 1U) {
        result = ParseResult::NEED_MORE;
        return Status::OK;
      }
      if (pending()[1] == '\\') {
        if (sequence_limit < 2U ||
            active_sequence_.size() > sequence_limit - 2U) {
          events.emplace_back(
              UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
          reset_active_action();
          result = ParseResult::CONSUMED;
          return Status::INPUT_LIMIT_EXCEEDED;
        }
        active_sequence_.append(pending().substr(0, 2U));
        consume(2U);
        const Status status =
            action != nullptr && action->protocol == InputProtocol::OSC52
                ? parse_osc52(active_sequence_, events)
                : parse_osc(active_sequence_, events);
        reset_active_action();
        result = ParseResult::CONSUMED;
        return status;
      }
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    active_sequence_.push_back(byte);
    consume(1U);

    const bool terminated_by_bel = byte == '\a';
    if (terminated_by_bel) {
      const Status status =
          action != nullptr && action->protocol == InputProtocol::OSC52
              ? parse_osc52(active_sequence_, events)
              : parse_osc(active_sequence_, events);
      reset_active_action();
      result = ParseResult::CONSUMED;
      return status;
    }
    if (active_sequence_.size() >= sequence_limit) {
      events.emplace_back(
          UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::INPUT_LIMIT_EXCEEDED;
    }
  }
  result = ParseResult::NEED_MORE;
  return Status::OK;
}

Status Decoder::capture_st_string(std::vector<Event>& events,
                                  ParseResult& result) {
  const EnterInputProtocol* action = active_protocol();
  while (!pending().empty()) {
    const char byte = pending().front();
    if (byte == kEscape) {
      if (pending().size() == 1U) {
        result = ParseResult::NEED_MORE;
        return Status::OK;
      }
      if (pending()[1] == '\\') {
        if (limits_.maximum_sequence_bytes < 2U ||
            active_sequence_.size() > limits_.maximum_sequence_bytes - 2U) {
          events.emplace_back(
              UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
          reset_active_action();
          result = ParseResult::CONSUMED;
          return Status::INPUT_LIMIT_EXCEEDED;
        }
        active_sequence_.append(pending().substr(0, 2U));
        consume(2U);
        events.emplace_back(TerminalResponseEvent{
            .kind =
                action != nullptr &&
                        action->protocol == InputProtocol::DEVICE_CONTROL_STRING
                    ? TerminalResponseKind::DEVICE_CONTROL_STRING
                    : TerminalResponseKind::OPERATING_SYSTEM_COMMAND,
            .bytes = active_sequence_,
        });
        reset_active_action();
        result = ParseResult::CONSUMED;
        return Status::OK;
      }
      events.emplace_back(
          unknown(UnknownInputReason::INCOMPLETE_SEQUENCE, active_sequence_));
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::OK;
    }
    active_sequence_.push_back(byte);
    consume(1U);
    if (active_sequence_.size() >= limits_.maximum_sequence_bytes) {
      events.emplace_back(
          UnknownEvent{.reason = UnknownInputReason::LIMIT_EXCEEDED});
      reset_active_action();
      result = ParseResult::CONSUMED;
      return Status::INPUT_LIMIT_EXCEEDED;
    }
  }
  result = ParseResult::NEED_MORE;
  return Status::OK;
}

Status Decoder::capture_paste(std::vector<Event>& events, ParseResult& result) {
  if (paste_end_sequence_.empty()) {
    events.emplace_back(
        UnknownEvent{.reason = UnknownInputReason::UNSUPPORTED_SEQUENCE});
    reset_active_action();
    paste_in_progress_ = false;
    result             = ParseResult::CONSUMED;
    return Status::UNSUPPORTED;
  }

  std::string data;
  while (!pending().empty()) {
    paste_candidate_.push_back(pending().front());
    consume(1U);

    while (!paste_candidate_.empty() &&
           !paste_end_sequence_.starts_with(paste_candidate_)) {
      data.push_back(paste_candidate_.front());
      paste_candidate_.erase(0, 1U);
    }
    if (paste_candidate_ == paste_end_sequence_) {
      const Status data_status = append_paste_data(data, events);
      paste_candidate_.clear();
      if (!paste_discarding_) {
        events.emplace_back(PasteEvent{.phase = PastePhase::END});
      }
      paste_in_progress_ = false;
      paste_discarding_  = false;
      paste_bytes_       = 0;
      reset_active_action();
      result = ParseResult::CONSUMED;
      return data_status;
    }
  }

  const Status status = append_paste_data(data, events);
  result = data.empty() ? ParseResult::NEED_MORE : ParseResult::CONSUMED;
  return status;
}

Status Decoder::append_paste_data(std::string_view data,
                                  std::vector<Event>& events) {
  if (data.empty() || paste_discarding_) {
    return Status::OK;
  }
  if (data.size() > limits_.maximum_paste_bytes -
                        std::min(paste_bytes_, limits_.maximum_paste_bytes)) {
    events.emplace_back(PasteEvent{.phase = PastePhase::CANCEL});
    paste_discarding_ = true;
    Logger<WARN> << "Cancelled bracketed paste after exceeding "
                 << limits_.maximum_paste_bytes << " bytes";
    return Status::INPUT_LIMIT_EXCEEDED;
  }
  events.emplace_back(PasteEvent{
      .phase = PastePhase::DATA,
      .data  = std::string{data},
  });
  paste_bytes_ += data.size();
  return Status::OK;
}

void Decoder::reset_trie_cursor() noexcept { trie_cursor_.reset(); }

void Decoder::reset_active_action() noexcept {
  active_action_node_ = InputTrie::kInvalidNode;
  active_sequence_.clear();
}

const EnterInputProtocol* Decoder::active_protocol() const noexcept {
  if (active_action_node_ == InputTrie::kInvalidNode) {
    return nullptr;
  }
  return input_trie_.node(active_action_node_).value.protocol();
}

Status Decoder::parse_sgr_mouse(std::string_view sequence,
                                std::vector<Event>& events) {
  const char final            = sequence.back();
  const std::string_view body = sequence.substr(2, sequence.size() - 3U);
  if (body.empty() || body.front() != '<' || (final != 'M' && final != 'm')) {
    events.emplace_back(
        unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    return Status::OK;
  }

  std::array<std::string_view, 3> fields{};
  std::size_t count = 0;
  if (!split_fields(body.substr(1), ';', fields, count) || count != 3U) {
    events.emplace_back(
        unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    return Status::OK;
  }
  std::uint32_t code = 0;
  std::uint32_t x    = 0;
  std::uint32_t y    = 0;
  if (!parse_unsigned(fields[0], code) || !parse_unsigned(fields[1], x) ||
      !parse_unsigned(fields[2], y) || x == 0U || y == 0U) {
    events.emplace_back(
        unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    return Status::OK;
  }

  Modifiers modifiers;
  if ((code & 4U) != 0U) {
    modifiers.add(Modifier::SHIFT);
  }
  if ((code & 8U) != 0U) {
    modifiers.add(Modifier::ALT);
  }
  if ((code & 16U) != 0U) {
    modifiers.add(Modifier::CONTROL);
  }
  const CellPosition position{
      .x = static_cast<std::size_t>(x - 1U),
      .y = static_cast<std::size_t>(y - 1U),
  };
  const std::uint32_t button_code = code & ~(4U | 8U | 16U | 32U);
  if ((button_code & ~(3U | 64U | 128U)) != 0U ||
      (button_code & (64U | 128U)) == (64U | 128U)) {
    events.emplace_back(
        unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, sequence));
    return Status::OK;
  }
  if ((button_code & 64U) != 0U && (button_code & 128U) == 0U) {
    const std::uint32_t wheel = button_code & 3U;
    events.emplace_back(ScrollEvent{
        .position  = position,
        .delta_x   = wheel == 2U   ? 1
                     : wheel == 3U ? -1
                                   : 0,
        .delta_y   = wheel == 0U   ? 1
                     : wheel == 1U ? -1
                                   : 0,
        .modifiers = modifiers,
    });
    return Status::OK;
  }

  MouseButton button = MouseButton::NONE;
  if ((button_code & 128U) != 0U) {
    button = auxiliary_button(button_code & 3U);
  } else {
    switch (button_code & 3U) {
      case 0:
        button = MouseButton::LEFT;
        break;
      case 1:
        button = MouseButton::MIDDLE;
        break;
      case 2:
        button = MouseButton::RIGHT;
        break;
      default:
        break;
    }
  }
  const bool motion = (code & 32U) != 0U;
  events.emplace_back(MouseEvent{
      .position  = position,
      .button    = button,
      .action    = final == 'm'                  ? MouseAction::RELEASE
                   : !motion                     ? MouseAction::PRESS
                   : button == MouseButton::NONE ? MouseAction::MOVE
                                                 : MouseAction::DRAG,
      .modifiers = modifiers,
  });
  return Status::OK;
}

Status Decoder::parse_csi(std::string_view sequence,
                          std::vector<Event>& events) {
  const char final            = sequence.back();
  const std::string_view body = sequence.substr(2, sequence.size() - 3U);

  if (final == 'u') {
    if (!body.empty() && body.front() == '?') {
      std::uint32_t flags = 0;
      if (!parse_unsigned(body.substr(1), flags)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      } else {
        events.emplace_back(TerminalResponseEvent{
            .kind  = TerminalResponseKind::KITTY_KEYBOARD_FLAGS,
            .value = flags,
        });
      }
      return Status::OK;
    }
    if (!body.empty() && (body.front() == '>' || body.front() == '<')) {
      events.emplace_back(TerminalResponseEvent{
          .kind  = TerminalResponseKind::KITTY_KEYBOARD_FLAGS,
          .bytes = std::string{sequence},
      });
      return Status::OK;
    }

    std::array<std::string_view, 3> fields{};
    std::size_t count = 0;
    if (!split_fields(body, ';', fields, count) || count == 0U) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      return Status::OK;
    }

    std::array<std::string_view, 3> key_fields{};
    std::size_t key_count    = 0;
    std::uint32_t key_number = 0;
    if (!split_fields(fields[0], ':', key_fields, key_count) ||
        key_count == 0U || !parse_unsigned(key_fields[0], key_number)) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      return Status::OK;
    }

    KeyEvent event;
    if (key_count >= 2U && !key_fields[1].empty()) {
      std::uint32_t shifted = 0;
      if (!parse_unsigned(key_fields[1], shifted) ||
          !is_unicode_scalar(shifted)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
        return Status::OK;
      }
      event.shifted_key = static_cast<char32_t>(shifted);
    }
    if (key_count >= 3U && !key_fields[2].empty()) {
      std::uint32_t base = 0;
      if (!parse_unsigned(key_fields[2], base) || !is_unicode_scalar(base)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
        return Status::OK;
      }
      event.base_layout_key = static_cast<char32_t>(base);
    }

    if (count >= 2U &&
        !parse_modifiers_and_action(fields[1], event.modifiers, event.action)) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      return Status::OK;
    }

    if (count >= 3U && !fields[2].empty()) {
      std::array<std::string_view, 64> text_fields{};
      std::size_t text_count = 0;
      if (!split_fields(fields[2], ':', text_fields, text_count)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
        return Status::OK;
      }
      for (std::size_t index = 0; index < text_count; ++index) {
        std::uint32_t text_codepoint = 0;
        if (!parse_unsigned(text_fields[index], text_codepoint) ||
            !is_unicode_scalar(text_codepoint) || text_codepoint < 0x20U ||
            (text_codepoint >= 0x7fU && text_codepoint <= 0x9fU)) {
          events.emplace_back(
              unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
          return Status::OK;
        }
        append_utf8(static_cast<char32_t>(text_codepoint), event.text);
      }
    }

    if (key_number == 0U) {
      if (event.text.empty()) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      } else {
        events.emplace_back(TextEvent{.utf8 = std::move(event.text)});
      }
      return Status::OK;
    }
    if (!protocol_key_code(key_number, event.key)) {
      events.emplace_back(
          unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, sequence));
      return Status::OK;
    }
    events.emplace_back(std::move(event));
    return Status::OK;
  }

  if (final == '~') {
    std::array<std::string_view, 3> fields{};
    std::size_t count        = 0;
    std::uint32_t key_number = 0;
    if (!split_fields(body, ';', fields, count) || count == 0U ||
        !parse_unsigned(fields[0], key_number)) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      return Status::OK;
    }

    if (key_number == 27U && count == 3U) {
      KeyEvent event;
      std::uint32_t codepoint = 0;
      if (!parse_modifiers_and_action(fields[1], event.modifiers,
                                      event.action) ||
          !parse_unsigned(fields[2], codepoint) ||
          !protocol_key_code(codepoint, event.key)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      } else {
        events.emplace_back(std::move(event));
      }
      return Status::OK;
    }

    const std::optional<NamedKey> named = tilde_named_key(key_number);
    if (!named || count != 2U) {
      events.emplace_back(
          unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, sequence));
      return Status::OK;
    }
    KeyEvent event{.key = *named};
    if (!parse_modifiers_and_action(fields[1], event.modifiers, event.action)) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      return Status::OK;
    }
    events.emplace_back(std::move(event));
    return Status::OK;
  }

  if (!body.empty()) {
    const std::optional<NamedKey> named = csi_final_key(final);
    if (named) {
      KeyEvent event{.key = *named};
      if (!parse_letter_key_fields(body, event.modifiers, event.action)) {
        events.emplace_back(
            unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
      } else {
        events.emplace_back(std::move(event));
      }
      return Status::OK;
    }
  }

  if (final == 'E' && !body.empty()) {
    KeyEvent event{.key = NamedKey::KEYPAD_BEGIN};
    if (!parse_letter_key_fields(body, event.modifiers, event.action)) {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    } else {
      events.emplace_back(std::move(event));
    }
    return Status::OK;
  }

  if (final == 'R') {
    std::array<std::string_view, 2> fields{};
    std::size_t count    = 0;
    std::uint32_t row    = 0;
    std::uint32_t column = 0;
    if (split_fields(body, ';', fields, count) && count == 2U &&
        parse_unsigned(fields[0], row) && parse_unsigned(fields[1], column)) {
      events.emplace_back(TerminalResponseEvent{
          .kind  = TerminalResponseKind::CURSOR_POSITION,
          .value = row,
          .bytes = std::string{fields[1]},
      });
    } else {
      events.emplace_back(
          unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    }
    return Status::OK;
  }

  if (final == 'c') {
    events.emplace_back(TerminalResponseEvent{
        .kind  = TerminalResponseKind::DEVICE_ATTRIBUTES,
        .bytes = std::string{sequence},
    });
    return Status::OK;
  }

  if (final == 'y' && body.find('$') != std::string_view::npos) {
    events.emplace_back(TerminalResponseEvent{
        .kind  = TerminalResponseKind::MODE_REPORT,
        .bytes = std::string{sequence},
    });
    return Status::OK;
  }

  events.emplace_back(
      unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, sequence));
  return Status::OK;
}

Status Decoder::parse_osc(std::string_view sequence,
                          std::vector<Event>& events) {
  events.emplace_back(TerminalResponseEvent{
      .kind  = TerminalResponseKind::OPERATING_SYSTEM_COMMAND,
      .bytes = std::string{sequence},
  });
  return Status::OK;
}

Status Decoder::parse_osc52(std::string_view sequence,
                            std::vector<Event>& events) {
  const std::size_t terminator_size = sequence.back() == '\a' ? 1U : 2U;
  const std::string_view content =
      sequence.substr(2, sequence.size() - 2U - terminator_size);
  if (!content.starts_with("52;")) {
    events.emplace_back(
        unknown(UnknownInputReason::MALFORMED_SEQUENCE, sequence));
    return Status::OK;
  }

  const std::size_t separator = content.find(';', 3U);
  if (separator == std::string_view::npos) {
    events.emplace_back(UnknownEvent{
        .reason = UnknownInputReason::MALFORMED_SEQUENCE,
    });
    return Status::OK;
  }
  const std::string_view selectors = content.substr(3, separator - 3U);
  const std::string_view payload   = content.substr(separator + 1U);
  if (payload == "?") {
    events.emplace_back(TerminalResponseEvent{
        .kind = TerminalResponseKind::OPERATING_SYSTEM_COMMAND,
    });
    return Status::OK;
  }

  ClipboardSelection selection = ClipboardSelection::CLIPBOARD;
  if (selectors.find('c') != std::string_view::npos) {
    selection = ClipboardSelection::CLIPBOARD;
  } else if (selectors.find('p') != std::string_view::npos) {
    selection = ClipboardSelection::PRIMARY;
  } else {
    events.emplace_back(UnknownEvent{
        .reason = UnknownInputReason::UNSUPPORTED_SEQUENCE,
    });
    return Status::OK;
  }

  ClipboardEvent event;
  const Status status = decode_clipboard_payload(
      selection, payload, event, limits_.maximum_clipboard_bytes);
  if (is_ok(status)) {
    events.emplace_back(std::move(event));
    return Status::OK;
  }
  events.emplace_back(UnknownEvent{
      .reason = status == Status::INPUT_LIMIT_EXCEEDED
                    ? UnknownInputReason::LIMIT_EXCEEDED
                    : UnknownInputReason::MALFORMED_SEQUENCE,
  });
  return status == Status::INPUT_LIMIT_EXCEEDED ? status : Status::OK;
}

Decoder::ParseResult Decoder::parse_text(std::vector<Event>& events) {
  const std::string_view bytes = pending();
  const unsigned char first    = static_cast<unsigned char>(bytes.front());
  if (first < 0x20U || first == 0x7fU) {
    events.emplace_back(
        unknown(UnknownInputReason::UNSUPPORTED_SEQUENCE, bytes.substr(0, 1)));
    consume(1U);
    return ParseResult::CONSUMED;
  }

  std::size_t text_size = 0;
  while (text_size < bytes.size()) {
    const unsigned char byte = static_cast<unsigned char>(bytes[text_size]);
    if (byte == static_cast<unsigned char>(kEscape) || byte < 0x20U ||
        byte == 0x7fU) {
      break;
    }
    if (byte < 0x80U) {
      ++text_size;
      continue;
    }

    char32_t codepoint = U'\0';
    std::size_t length = 0;
    const Utf8Result result =
        decode_utf8(bytes.substr(text_size), codepoint, length);
    if (result == Utf8Result::INCOMPLETE) {
      if (text_size == 0U) {
        return ParseResult::NEED_MORE;
      }
      break;
    }
    if (result == Utf8Result::INVALID) {
      if (text_size == 0U) {
        events.emplace_back(
            unknown(UnknownInputReason::INVALID_UTF8, bytes.substr(0, 1)));
        consume(1U);
        return ParseResult::CONSUMED;
      }
      break;
    }
    text_size += length;
  }

  events.emplace_back(
      TextEvent{.utf8 = std::string{bytes.substr(0, text_size)}});
  consume(text_size);
  return ParseResult::CONSUMED;
}

void Decoder::consume(std::size_t byte_count) noexcept {
  offset_ += byte_count;
}

void Decoder::compact() {
  if (offset_ == 0U) {
    return;
  }
  if (offset_ == buffer_.size()) {
    buffer_.clear();
  } else {
    buffer_.erase(0, offset_);
  }
  offset_ = 0;
}

std::string_view Decoder::pending() const noexcept {
  return std::string_view{buffer_}.substr(offset_);
}

}  // namespace terminal
}  // namespace puc
