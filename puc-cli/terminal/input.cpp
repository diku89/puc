/**
 * @file input.cpp
 * @brief TOML and terminfo construction of the terminal input trie.
 */

#include "puc-cli/terminal/input.hpp"

#include <curses.h>
#include <term.h>

#ifdef OK
#undef OK
#endif
#ifdef ERR
#undef ERR
#endif
#ifdef tab
#undef tab
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "utils/logger/logger.hpp"

/** @cond TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("TerminalInputMap");
/** @endcond */

namespace puc {
namespace terminal {
namespace {

constexpr std::size_t kMaximumConfiguredSequenceBytes = 4096U;
constexpr std::string_view kInputConfigurationPath    = "input_keys.toml";
constexpr std::string_view kTerminalProfileDirectory  = "terminals";
using InputTrie = containers::Trie<char, InputAction>;

/** Return a table field or a missing value when it is absent. */
config::Value field(const config::Value& table,
                    std::string_view name) noexcept {
  return table.find(name);
}

/** Return a non-owning configuration string value. */
std::optional<std::string_view> string_value(
    const config::Value& value) noexcept {
  return value.as_string();
}

/** Log one configuration diagnostic with source coordinates when available. */
void log_configuration_error(const config::Value& value,
                             std::string_view message) {
  const config::SourceLocation location = value.location();
  Logger<ERROR> << (location.source.empty() ? "<configuration>"
                                            : location.source)
                << ':' << location.line << ':' << location.column << ": "
                << message;
}

/** Expand PUC's readable control-sequence notation into exact bytes. */
bool decode_sequence(std::string_view encoded, std::string& decoded) {
  decoded.clear();
  decoded.reserve(encoded.size());

  for (std::size_t index = 0; index < encoded.size();) {
    if (encoded[index] == '<') {
      const std::size_t end = encoded.find('>', index + 1U);
      if (end == std::string_view::npos) {
        return false;
      }
      const std::string_view token =
          encoded.substr(index + 1U, end - index - 1U);
      if (token == "ESC") {
        decoded.push_back('\x1b');
      } else if (token == "CSI") {
        decoded.append("\x1b[");
      } else if (token == "OSC") {
        decoded.append("\x1b]");
      } else if (token == "DCS") {
        decoded.append("\x1bP");
      } else if (token == "APC") {
        decoded.append("\x1b_");
      } else if (token == "PM") {
        decoded.append("\x1b^");
      } else if (token == "ST") {
        decoded.append("\x1b\\");
      } else if (token == "BEL") {
        decoded.push_back('\a');
      } else if (token == "DEL") {
        decoded.push_back('\x7f');
      } else if (token == "NUL") {
        decoded.push_back('\0');
      } else if (token == "TAB") {
        decoded.push_back('\t');
      } else if (token == "CR") {
        decoded.push_back('\r');
      } else if (token == "LF") {
        decoded.push_back('\n');
      } else if (token == "LT") {
        decoded.push_back('<');
      } else {
        return false;
      }
      index = end + 1U;
      continue;
    }

    if (encoded[index] == '^' && index + 1U < encoded.size()) {
      unsigned char control = static_cast<unsigned char>(encoded[index + 1U]);
      if (control == '?') {
        decoded.push_back('\x7f');
        index += 2U;
        continue;
      }
      if (control >= 'a' && control <= 'z') {
        control = static_cast<unsigned char>(control - 'a' + 'A');
      }
      if (control >= '@' && control <= '_') {
        decoded.push_back(static_cast<char>(control & 0x1fU));
        index += 2U;
        continue;
      }
      return false;
    }

    decoded.push_back(encoded[index]);
    ++index;
  }
  return decoded.size() <= kMaximumConfiguredSequenceBytes;
}

/** Render arbitrary sequence bytes without emitting terminal control codes. */
std::string diagnostic_sequence(std::string_view sequence) {
  constexpr char kHexDigits[] = "0123456789ABCDEF";
  std::string output;
  output.reserve(sequence.size());
  for (const unsigned char byte : sequence) {
    if (byte == 0x1bU) {
      output.append("<ESC>");
    } else if (byte == 0x7fU) {
      output.append("<DEL>");
    } else if (byte < 0x20U) {
      output.push_back('^');
      output.push_back(static_cast<char>(byte + '@'));
    } else if (byte < 0x7fU) {
      output.push_back(static_cast<char>(byte));
    } else {
      output.append("\\x");
      output.push_back(kHexDigits[byte >> 4U]);
      output.push_back(kHexDigits[byte & 0x0fU]);
    }
  }
  return output;
}

/** Parse a decimal suffix with an inclusive upper bound. */
std::optional<unsigned int> decimal_suffix(std::string_view text,
                                           std::string_view prefix,
                                           unsigned int maximum) noexcept {
  if (!text.starts_with(prefix) || text.size() == prefix.size()) {
    return std::nullopt;
  }
  unsigned int result = 0;
  for (const char byte : text.substr(prefix.size())) {
    if (byte < '0' || byte > '9') {
      return std::nullopt;
    }
    result = result * 10U + static_cast<unsigned int>(byte - '0');
    if (result > maximum) {
      return std::nullopt;
    }
  }
  return result;
}

/** Convert a configuration key name into PUC's public named-key enum. */
std::optional<NamedKey> named_key(std::string_view name) noexcept {
  struct Entry {
    std::string_view name;
    NamedKey key;
  };
  constexpr Entry kEntries[] = {
      {"ESCAPE", NamedKey::ESCAPE},
      {"ENTER", NamedKey::ENTER},
      {"TAB", NamedKey::TAB},
      {"BACKSPACE", NamedKey::BACKSPACE},
      {"INSERT", NamedKey::INSERT},
      {"DELETE", NamedKey::DELETE_KEY},
      {"LEFT", NamedKey::LEFT},
      {"RIGHT", NamedKey::RIGHT},
      {"UP", NamedKey::UP},
      {"DOWN", NamedKey::DOWN},
      {"PAGE_UP", NamedKey::PAGE_UP},
      {"PAGE_DOWN", NamedKey::PAGE_DOWN},
      {"HOME", NamedKey::HOME},
      {"END", NamedKey::END},
      {"CAPS_LOCK", NamedKey::CAPS_LOCK},
      {"SCROLL_LOCK", NamedKey::SCROLL_LOCK},
      {"NUM_LOCK", NamedKey::NUM_LOCK},
      {"PRINT_SCREEN", NamedKey::PRINT_SCREEN},
      {"PAUSE", NamedKey::PAUSE},
      {"MENU", NamedKey::MENU},
      {"KEYPAD_DECIMAL", NamedKey::KEYPAD_DECIMAL},
      {"KEYPAD_DIVIDE", NamedKey::KEYPAD_DIVIDE},
      {"KEYPAD_MULTIPLY", NamedKey::KEYPAD_MULTIPLY},
      {"KEYPAD_SUBTRACT", NamedKey::KEYPAD_SUBTRACT},
      {"KEYPAD_ADD", NamedKey::KEYPAD_ADD},
      {"KEYPAD_ENTER", NamedKey::KEYPAD_ENTER},
      {"KEYPAD_EQUAL", NamedKey::KEYPAD_EQUAL},
      {"KEYPAD_SEPARATOR", NamedKey::KEYPAD_SEPARATOR},
      {"KEYPAD_LEFT", NamedKey::KEYPAD_LEFT},
      {"KEYPAD_RIGHT", NamedKey::KEYPAD_RIGHT},
      {"KEYPAD_UP", NamedKey::KEYPAD_UP},
      {"KEYPAD_DOWN", NamedKey::KEYPAD_DOWN},
      {"KEYPAD_PAGE_UP", NamedKey::KEYPAD_PAGE_UP},
      {"KEYPAD_PAGE_DOWN", NamedKey::KEYPAD_PAGE_DOWN},
      {"KEYPAD_HOME", NamedKey::KEYPAD_HOME},
      {"KEYPAD_END", NamedKey::KEYPAD_END},
      {"KEYPAD_INSERT", NamedKey::KEYPAD_INSERT},
      {"KEYPAD_DELETE", NamedKey::KEYPAD_DELETE},
      {"KEYPAD_BEGIN", NamedKey::KEYPAD_BEGIN},
      {"MEDIA_PLAY", NamedKey::MEDIA_PLAY},
      {"MEDIA_PAUSE", NamedKey::MEDIA_PAUSE},
      {"MEDIA_PLAY_PAUSE", NamedKey::MEDIA_PLAY_PAUSE},
      {"MEDIA_REVERSE", NamedKey::MEDIA_REVERSE},
      {"MEDIA_STOP", NamedKey::MEDIA_STOP},
      {"MEDIA_FAST_FORWARD", NamedKey::MEDIA_FAST_FORWARD},
      {"MEDIA_REWIND", NamedKey::MEDIA_REWIND},
      {"MEDIA_TRACK_NEXT", NamedKey::MEDIA_TRACK_NEXT},
      {"MEDIA_TRACK_PREVIOUS", NamedKey::MEDIA_TRACK_PREVIOUS},
      {"MEDIA_RECORD", NamedKey::MEDIA_RECORD},
      {"VOLUME_DOWN", NamedKey::VOLUME_DOWN},
      {"VOLUME_UP", NamedKey::VOLUME_UP},
      {"VOLUME_MUTE", NamedKey::VOLUME_MUTE},
      {"LEFT_SHIFT", NamedKey::LEFT_SHIFT},
      {"LEFT_CONTROL", NamedKey::LEFT_CONTROL},
      {"LEFT_ALT", NamedKey::LEFT_ALT},
      {"LEFT_SUPER", NamedKey::LEFT_SUPER},
      {"LEFT_HYPER", NamedKey::LEFT_HYPER},
      {"LEFT_META", NamedKey::LEFT_META},
      {"RIGHT_SHIFT", NamedKey::RIGHT_SHIFT},
      {"RIGHT_CONTROL", NamedKey::RIGHT_CONTROL},
      {"RIGHT_ALT", NamedKey::RIGHT_ALT},
      {"RIGHT_SUPER", NamedKey::RIGHT_SUPER},
      {"RIGHT_HYPER", NamedKey::RIGHT_HYPER},
      {"RIGHT_META", NamedKey::RIGHT_META},
      {"ISO_LEVEL3_SHIFT", NamedKey::ISO_LEVEL3_SHIFT},
      {"ISO_LEVEL5_SHIFT", NamedKey::ISO_LEVEL5_SHIFT},
  };
  for (const Entry& entry : kEntries) {
    if (entry.name == name) {
      return entry.key;
    }
  }

  if (const std::optional<unsigned int> number = decimal_suffix(name, "F", 35U);
      number && *number >= 1U) {
    return static_cast<NamedKey>(static_cast<unsigned int>(NamedKey::F1) +
                                 *number - 1U);
  }
  if (const std::optional<unsigned int> number =
          decimal_suffix(name, "KEYPAD_", 9U)) {
    return static_cast<NamedKey>(static_cast<unsigned int>(NamedKey::KEYPAD_0) +
                                 *number);
  }
  return std::nullopt;
}

/** Parse one modifier name into its bit value. */
std::optional<Modifier> modifier(std::string_view name) noexcept {
  if (name == "SHIFT") {
    return Modifier::SHIFT;
  }
  if (name == "ALT") {
    return Modifier::ALT;
  }
  if (name == "CONTROL") {
    return Modifier::CONTROL;
  }
  if (name == "SUPER") {
    return Modifier::SUPER;
  }
  if (name == "HYPER") {
    return Modifier::HYPER;
  }
  if (name == "META") {
    return Modifier::META;
  }
  if (name == "CAPS_LOCK") {
    return Modifier::CAPS_LOCK;
  }
  if (name == "NUM_LOCK") {
    return Modifier::NUM_LOCK;
  }
  return std::nullopt;
}

/** Parse optional modifiers from a mapping table. */
bool parse_modifiers(const config::Value& table, Modifiers& modifiers) {
  modifiers                      = {};
  const config::Value configured = field(table, "modifiers");
  if (!configured) {
    return true;
  }
  if (configured.type() != config::ValueType::ARRAY) {
    return false;
  }
  for (std::size_t index = 0; index < configured.size(); ++index) {
    const std::optional<std::string_view> name =
        string_value(configured.at(index));
    if (!name) {
      return false;
    }
    const std::optional<Modifier> parsed = modifier(*name);
    if (!parsed) {
      return false;
    }
    modifiers.add(*parsed);
  }
  return true;
}

/** Parse an optional key action from a mapping table. */
bool parse_key_action(const config::Value& table, KeyAction& action) {
  action                         = KeyAction::PRESS;
  const config::Value configured = field(table, "key_action");
  if (!configured) {
    return true;
  }
  const std::optional<std::string_view> name = string_value(configured);
  if (!name) {
    return false;
  }
  if (*name == "PRESS") {
    action = KeyAction::PRESS;
  } else if (*name == "REPEAT") {
    action = KeyAction::REPEAT;
  } else if (*name == "RELEASE") {
    action = KeyAction::RELEASE;
  } else {
    return false;
  }
  return true;
}

/** Parse a key event shared by direct and terminfo mappings. */
bool parse_key_event(const config::Value& table, KeyEvent& event) {
  event                   = {};
  const config::Value key = field(table, "key");
  if (const std::optional<std::string_view> name = string_value(key)) {
    if (const std::optional<NamedKey> parsed = named_key(*name)) {
      event.key = *parsed;
    } else if (name->size() == 1U) {
      event.key =
          static_cast<char32_t>(static_cast<unsigned char>(name->front()));
    } else {
      return false;
    }
  } else {
    const std::optional<std::int64_t> codepoint =
        field(table, "codepoint").as_integer();
    if (!codepoint || *codepoint <= 0 || *codepoint > 0x10ffff ||
        (*codepoint >= 0xd800 && *codepoint <= 0xdfff)) {
      return false;
    }
    event.key = static_cast<char32_t>(*codepoint);
  }
  return parse_modifiers(table, event.modifiers) &&
         parse_key_action(table, event.action);
}

/** Parse a symbolic protocol name. */
std::optional<InputProtocol> input_protocol(std::string_view name) noexcept {
  struct Entry {
    std::string_view name;
    InputProtocol protocol;
  };
  constexpr Entry kEntries[] = {
      {"TEXT", InputProtocol::TEXT},
      {"ESCAPE", InputProtocol::ESCAPE},
      {"SS3", InputProtocol::SS3},
      {"CSI", InputProtocol::CSI},
      {"OSC", InputProtocol::OSC},
      {"DEVICE_CONTROL_STRING", InputProtocol::DEVICE_CONTROL_STRING},
      {"APPLICATION_PROGRAM_COMMAND",
       InputProtocol::APPLICATION_PROGRAM_COMMAND},
      {"PRIVACY_MESSAGE", InputProtocol::PRIVACY_MESSAGE},
      {"SGR_MOUSE", InputProtocol::SGR_MOUSE},
      {"OSC52", InputProtocol::OSC52},
      {"PASTE_BEGIN", InputProtocol::PASTE_BEGIN},
      {"PASTE_END", InputProtocol::PASTE_END},
  };
  for (const Entry& entry : kEntries) {
    if (entry.name == name) {
      return entry.protocol;
    }
  }
  return std::nullopt;
}

/** Parse one high-level command name stored directly in the input Trie. */
std::optional<Command> command(std::string_view name) noexcept {
  if (name == "COPY") {
    return Command::COPY;
  }
  return std::nullopt;
}

/** Parse a mapping table's directly stored trie action. */
bool parse_action(const config::Value& table, InputAction& action) {
  const std::optional<std::string_view> kind =
      string_value(field(table, "kind"));
  if (!kind) {
    return false;
  }
  if (*kind == "key") {
    KeyEvent event;
    if (!parse_key_event(table, event)) {
      return false;
    }
    action = InputAction{Event{std::move(event)}};
    return true;
  }
  if (*kind == "protocol") {
    const std::optional<std::string_view> name =
        string_value(field(table, "protocol"));
    const std::optional<InputProtocol> parsed =
        name ? input_protocol(*name) : std::nullopt;
    if (!parsed) {
      return false;
    }
    action = InputAction{*parsed};
    return true;
  }
  if (*kind == "focus") {
    const std::optional<bool> focused = field(table, "focused").as_boolean();
    if (!focused) {
      return false;
    }
    action = InputAction{Event{FocusEvent{.focused = *focused}}};
    return true;
  }
  if (*kind == "command") {
    const std::optional<std::string_view> name =
        string_value(field(table, "command"));
    const std::optional<Command> parsed = name ? command(*name) : std::nullopt;
    if (!parsed) {
      return false;
    }
    action = InputAction{Event{CommandEvent{.command = *parsed}}};
    return true;
  }
  return false;
}

/** Validate the optional format version. */
bool valid_version(const config::Value& root) noexcept {
  const config::Value version = field(root, "version");
  return !version || version.as_integer() == 1;
}

/** Return whether a table explicitly disables its sequence. */
bool mapping_disabled(const config::Value& table, bool& disabled) noexcept {
  const config::Value value = field(table, "disabled");
  if (!value) {
    disabled = false;
    return true;
  }
  const std::optional<bool> configured = value.as_boolean();
  if (!configured) {
    return false;
  }
  disabled = *configured;
  return true;
}

/** Parse an array-of-tables field or accept its absence. */
bool table_array(const config::Value& root, std::string_view name,
                 config::Value& output) noexcept {
  output = field(root, name);
  if (!output) {
    return true;
  }
  if (output.type() != config::ValueType::ARRAY) {
    return false;
  }
  for (std::size_t index = 0; index < output.size(); ++index) {
    if (output.at(index).type() != config::ValueType::TABLE) {
      return false;
    }
  }
  return true;
}

/** Depth-first lookup of one protocol action while retaining its byte path. */
bool find_protocol_path(const InputTrie& trie, InputTrie::NodeIndex node,
                        InputProtocol protocol, std::string& path) {
  const InputTrie::Node& current = trie.node(node);
  if (current.sequence_end) {
    const EnterInputProtocol* action = current.value.protocol();
    if (action != nullptr && action->protocol == protocol) {
      return true;
    }
  }
  for (const InputTrie::NodeIndex child : current.children) {
    path.push_back(trie.node(child).key);
    if (find_protocol_path(trie, child, protocol, path)) {
      return true;
    }
    path.pop_back();
  }
  return false;
}

/** Convert shared configuration failures into the terminal status domain. */
Status configuration_status(config::Status status) noexcept {
  return status == config::Status::PARSE_ERROR
             ? Status::CONFIGURATION_PARSE_FAILED
             : Status::CONFIGURATION_LOAD_FAILED;
}

/** Resolve and validate the terminal name used by terminfo and profile lookup.
 */
Status terminal_name(std::string_view configured, std::string& output) {
  output.assign(configured);
  if (output.empty()) {
    const char* environment_terminal = std::getenv("TERM");
    if (environment_terminal == nullptr || *environment_terminal == '\0') {
      Logger<ERROR> << "Cannot set up terminal input because TERM is empty";
      return Status::TERMINFO_LOAD_FAILED;
    }
    output.assign(environment_terminal);
  }
  for (const unsigned char byte : output) {
    const bool valid = (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '-' ||
                       byte == '_' || byte == '.' || byte == '+';
    if (!valid) {
      Logger<ERROR> << "Rejected unsafe terminal profile name '" << output
                    << "'";
      return Status::INVALID_ARGUMENT;
    }
  }
  return Status::OK;
}

}  // namespace

const Event* InputAction::event() const noexcept {
  const auto* emit = std::get_if<EmitInputEvent>(&storage_);
  return emit == nullptr ? nullptr : &emit->event;
}

const EnterInputProtocol* InputAction::protocol() const noexcept {
  return std::get_if<EnterInputProtocol>(&storage_);
}

bool InputAction::empty() const noexcept {
  return std::holds_alternative<std::monostate>(storage_);
}

Status InputMap::setup(const config::Config& configurations, InputMap& output,
                       std::string_view configured_terminal_name,
                       int output_fd) {
  std::string selected_terminal;
  Status status = terminal_name(configured_terminal_name, selected_terminal);
  if (!is_ok(status)) {
    return status;
  }
  const config::LoadResult configured =
      configurations.load(kInputConfigurationPath);
  if (configured.status != config::Status::OK) {
    Logger<ERROR> << "Could not load terminal input configuration: "
                  << config::status_message(configured.status);
    return configuration_status(configured.status);
  }

  InputMap candidate;
  status = candidate.validate_config(configured.document.root());
  if (is_ok(status)) {
    status = candidate.apply_terminfo_bindings(configured.document.root());
  }
  if (is_ok(status)) {
    status = candidate.load_terminfo(selected_terminal, output_fd);
  }
  const std::filesystem::path terminal_profile_path =
      std::filesystem::path{kTerminalProfileDirectory} /
      (selected_terminal + ".toml");
  const config::LoadResult terminal_profile =
      configurations.load(terminal_profile_path);
  if (is_ok(status) && terminal_profile.status != config::Status::OK &&
      terminal_profile.status != config::Status::NOT_FOUND) {
    Logger<ERROR> << "Could not load terminal-specific input profile '"
                  << terminal_profile_path.string()
                  << "': " << config::status_message(terminal_profile.status);
    status = configuration_status(terminal_profile.status);
  }
  if (is_ok(status) && terminal_profile.status == config::Status::OK) {
    status = candidate.validate_config(terminal_profile.document.root());
    if (is_ok(status) && field(terminal_profile.document.root(), "terminfo")) {
      log_configuration_error(
          field(terminal_profile.document.root(), "terminfo"),
          "terminal-specific profiles may not redefine terminfo declarations");
      status = Status::CONFIGURATION_PARSE_FAILED;
    }
    if (is_ok(status)) {
      status = candidate.apply_mappings(terminal_profile.document.root());
    }
  }
  if (is_ok(status)) {
    status = candidate.apply_mappings(configured.document.root());
  }
  const std::string_view operating_system_path =
      operating_system_defaults_path(current_operating_system());
  if (is_ok(status) && operating_system_path.empty()) {
    Logger<ERROR> << "No terminal input defaults exist for this operating "
                     "system";
    status = Status::UNSUPPORTED;
  }
  config::LoadResult operating_system_defaults;
  if (is_ok(status)) {
    operating_system_defaults = configurations.load(operating_system_path);
    if (operating_system_defaults.status != config::Status::OK) {
      Logger<ERROR> << "Could not load operating-system input defaults '"
                    << operating_system_path << "': "
                    << config::status_message(operating_system_defaults.status);
      status = configuration_status(operating_system_defaults.status);
    }
  }
  if (is_ok(status)) {
    const config::Value root = operating_system_defaults.document.root();
    status                   = candidate.validate_config(root);
    if (is_ok(status) && field(root, "terminfo")) {
      log_configuration_error(
          field(root, "terminfo"),
          "operating-system defaults may not redefine terminfo declarations");
      status = Status::CONFIGURATION_PARSE_FAILED;
    }
    if (is_ok(status)) {
      status = candidate.apply_mappings(root);
    }
  }
  if (is_ok(status)) {
    status = candidate.validate_command_sequences();
  }
  if (!is_ok(status)) {
    return status;
  }
  output = std::move(candidate);
  return Status::OK;
}

Status InputMap::validate_command_sequences() const {
  std::size_t conflict_count = exact_command_conflicts_.size();
  for (const ExactCommandConflict& conflict : exact_command_conflicts_) {
    Logger<ERROR> << "Duplicate terminal command sequence '"
                  << diagnostic_sequence(conflict.first.sequence) << "' at "
                  << conflict.first.source << ':' << conflict.first.line
                  << " and " << conflict.second.source << ':'
                  << conflict.second.line;
  }
  for (std::size_t left = 0U; left < command_sequences_.size(); ++left) {
    for (std::size_t right = left + 1U; right < command_sequences_.size();
         ++right) {
      const std::string_view first  = command_sequences_[left].sequence;
      const std::string_view second = command_sequences_[right].sequence;
      if (!first.starts_with(second) && !second.starts_with(first)) {
        continue;
      }
      ++conflict_count;
      Logger<ERROR> << "Ambiguous terminal command sequences conflict: '"
                    << diagnostic_sequence(first) << "' and '"
                    << diagnostic_sequence(second) << "'";
    }
  }
  if (conflict_count != 0U) {
    Logger<ERROR> << "Rejected terminal input configuration with "
                  << conflict_count << " command-sequence conflict(s)";
    return Status::CONFIGURATION_PARSE_FAILED;
  }
  return Status::OK;
}

void InputMap::track_command_sequence(std::string_view sequence,
                                      const InputAction* action,
                                      config::SourceLocation location) {
  const auto existing = std::ranges::find(command_sequences_, sequence,
                                          &CommandSequence::sequence);
  const bool is_command =
      action != nullptr && action->event() != nullptr &&
      std::holds_alternative<CommandEvent>(*action->event());
  if (!is_command) {
    if (existing != command_sequences_.end()) {
      command_sequences_.erase(existing);
    }
    return;
  }

  CommandSequence declaration{
      .sequence = std::string{sequence},
      .source   = std::string{location.source},
      .line     = location.line,
      .column   = location.column,
  };
  if (existing == command_sequences_.end()) {
    command_sequences_.push_back(std::move(declaration));
    return;
  }
  if (existing->source == declaration.source) {
    exact_command_conflicts_.push_back(ExactCommandConflict{
        .first  = *existing,
        .second = declaration,
    });
  }
  *existing = std::move(declaration);
}

Status InputMap::validate_config(const config::Value& root) const {
  if (root.type() != config::ValueType::TABLE || !valid_version(root)) {
    log_configuration_error(root, "expected terminal input format version 1");
    return Status::CONFIGURATION_PARSE_FAILED;
  }

  config::Value mappings;
  config::Value terminfo;
  if (!table_array(root, "mapping", mappings) ||
      !table_array(root, "terminfo", terminfo)) {
    log_configuration_error(
        root, "'mapping' and 'terminfo' must be arrays of tables");
    return Status::CONFIGURATION_PARSE_FAILED;
  }
  return Status::OK;
}

Status InputMap::apply_mappings(const config::Value& root) {
  const config::Value mappings = field(root, "mapping");
  if (mappings) {
    for (std::size_t index = 0; index < mappings.size(); ++index) {
      const config::Value mapping = mappings.at(index);
      const std::optional<std::string_view> encoded =
          string_value(field(mapping, "sequence"));
      std::string sequence;
      bool disabled = false;
      if (!encoded || !decode_sequence(*encoded, sequence) ||
          !mapping_disabled(mapping, disabled)) {
        log_configuration_error(mapping, "invalid input mapping");
        return Status::CONFIGURATION_PARSE_FAILED;
      }
      if (disabled) {
        track_command_sequence(sequence, nullptr, mapping.location());
        disable_sequence(sequence);
        continue;
      }

      InputAction action;
      if (!parse_action(mapping, action)) {
        log_configuration_error(mapping, "invalid input mapping action");
        return Status::CONFIGURATION_PARSE_FAILED;
      }
      track_command_sequence(sequence, &action, mapping.location());
      if (!is_ok(register_sequence(sequence, std::move(action)))) {
        log_configuration_error(mapping, "invalid input mapping action");
        return Status::CONFIGURATION_PARSE_FAILED;
      }
    }
  }
  return Status::OK;
}

Status InputMap::apply_terminfo_bindings(const config::Value& root) {
  const config::Value terminfo = field(root, "terminfo");
  if (terminfo) {
    for (std::size_t index = 0; index < terminfo.size(); ++index) {
      const config::Value binding = terminfo.at(index);
      const std::optional<std::string_view> capability =
          string_value(field(binding, "capability"));
      KeyEvent event;
      if (!capability || !parse_key_event(binding, event) ||
          !is_ok(register_terminfo_key(*capability, std::move(event)))) {
        log_configuration_error(binding, "invalid terminfo key binding");
        return Status::CONFIGURATION_PARSE_FAILED;
      }
    }
  }

  return Status::OK;
}

Status InputMap::register_sequence(std::string_view sequence,
                                   InputAction action) {
  const EnterInputProtocol* protocol = action.protocol();
  if (sequence.size() > kMaximumConfiguredSequenceBytes || action.empty() ||
      (sequence.empty() &&
       (protocol == nullptr || protocol->protocol != InputProtocol::TEXT))) {
    return Status::INVALID_ARGUMENT;
  }
  const std::vector<char> path{sequence.begin(), sequence.end()};
  trie_.insert(path, std::move(action));
  return Status::OK;
}

Status InputMap::register_key_sequence(std::string_view sequence,
                                       KeyEvent event) {
  return register_sequence(sequence, InputAction{Event{std::move(event)}});
}

bool InputMap::disable_sequence(std::string_view sequence) {
  return trie_.erase(std::vector<char>{sequence.begin(), sequence.end()});
}

Status InputMap::register_terminfo_key(std::string_view capability,
                                       KeyEvent event) {
  if (capability.empty() || capability.size() > 64U) {
    return Status::INVALID_ARGUMENT;
  }
  for (const char byte : capability) {
    const bool valid = (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '_';
    if (!valid) {
      return Status::INVALID_ARGUMENT;
    }
  }
  for (TerminfoKeyBinding& binding : terminfo_keys_) {
    if (binding.capability == capability) {
      binding.event = std::move(event);
      return Status::OK;
    }
  }
  terminfo_keys_.push_back(TerminfoKeyBinding{
      .capability = std::string{capability},
      .event      = std::move(event),
  });
  return Status::OK;
}

Status InputMap::load_terminfo(std::string_view terminal_name, int output_fd) {
  if (output_fd < 0) {
    return Status::INVALID_ARGUMENT;
  }

  std::string selected_terminal{terminal_name};
  if (selected_terminal.empty()) {
    const char* environment_terminal = std::getenv("TERM");
    if (environment_terminal == nullptr || *environment_terminal == '\0') {
      Logger<WARN> << "Cannot load terminfo because TERM is empty";
      return Status::TERMINFO_LOAD_FAILED;
    }
    selected_terminal = environment_terminal;
  }

  int error = 0;
  if (::setupterm(selected_terminal.data(), output_fd, &error) != 0 ||
      error <= 0) {
    Logger<WARN> << "Could not load terminfo entry '" << selected_terminal
                 << "'";
    return Status::TERMINFO_LOAD_FAILED;
  }

  TERMINAL* loaded_terminal = cur_term;
  std::size_t loaded_count  = 0;
  for (const TerminfoKeyBinding& binding : terminfo_keys_) {
    char* sequence = ::tigetstr(const_cast<char*>(binding.capability.c_str()));
    const char* const invalid =
        reinterpret_cast<const char*>(static_cast<std::intptr_t>(-1));
    if (sequence == nullptr || sequence == invalid || *sequence == '\0') {
      continue;
    }
    const Status status = register_key_sequence(sequence, binding.event);
    if (!is_ok(status)) {
      static_cast<void>(::del_curterm(loaded_terminal));
      return status;
    }
    ++loaded_count;
  }
  static_cast<void>(::del_curterm(loaded_terminal));

  Logger<DEBUG> << "Loaded " << loaded_count << " key sequences from terminfo '"
                << selected_terminal << "'";
  return Status::OK;
}

bool InputMap::find_protocol_sequence(InputProtocol protocol,
                                      std::string& sequence) const {
  sequence.clear();
  return find_protocol_path(trie_, Trie::root(), protocol, sequence);
}

}  // namespace terminal
}  // namespace puc
