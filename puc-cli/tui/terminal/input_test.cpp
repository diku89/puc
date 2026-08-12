/**
 * @file input_test.cpp
 * @brief Tests for ordered runtime construction of the terminal input trie.
 */

#include "puc-cli/tui/terminal/input.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "gtest/gtest.h"
#include "puc-cli/tui/frames/input_frame.hpp"
#include "puc-cli/tui/terminal/decoder.hpp"

namespace puc::terminal {
namespace {

using namespace std::chrono_literals;

/** Primary and user roots created below Bazel's per-test temporary directory.
 */
struct ProfileRoots {
  std::filesystem::path root;
  std::filesystem::path primary;
  std::filesystem::path user;
};

/** Write one profile file, including any missing parent directories. */
bool write_profile(const std::filesystem::path& path,
                   std::string_view contents);

/** Create clean configuration roots for one test. */
ProfileRoots profile_roots(std::string_view name) {
  const char* temporary_directory = std::getenv("TEST_TMPDIR");
  EXPECT_NE(temporary_directory, nullptr);
  if (temporary_directory == nullptr) {
    return {};
  }

  ProfileRoots roots{
      .root = std::filesystem::path{temporary_directory} / std::string{name},
  };
  roots.primary = roots.root / "primary";
  roots.user    = roots.root / "user";

  std::error_code error;
  static_cast<void>(std::filesystem::remove_all(roots.root, error));
  error.clear();
  EXPECT_TRUE(std::filesystem::create_directories(roots.primary, error));
  EXPECT_FALSE(error);
  error.clear();
  EXPECT_TRUE(std::filesystem::create_directories(roots.user, error));
  EXPECT_FALSE(error);
  const std::string_view defaults_path =
      operating_system_defaults_path(current_operating_system());
  EXPECT_FALSE(defaults_path.empty());
  if (!defaults_path.empty()) {
    EXPECT_TRUE(write_profile(roots.primary / defaults_path, "version = 1\n"));
  }
  return roots;
}

/** Write one profile file, including any missing parent directories. */
bool write_profile(const std::filesystem::path& path,
                   std::string_view contents) {
  std::error_code error;
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    static_cast<void>(std::filesystem::create_directories(parent, error));
    if (error) {
      return false;
    }
  }

  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool written = std::fwrite(contents.data(), 1U, contents.size(),
                                   file) == contents.size();
  return std::fclose(file) == 0 && written;
}

/** Return application properties over the puc_config runfiles root. */
properties::Properties runfiles_properties() {
  std::error_code error;
  const std::filesystem::path root = std::filesystem::current_path(error);
  EXPECT_FALSE(error);
  return properties::Properties{root, root / "missing_user_overrides"};
}

/** Load one packaged immutable source through the Properties boundary. */
properties::LoadResult load_runfile(std::string_view path) {
  properties::Properties properties = runfiles_properties();
  return properties.load_immutable("test.source", path);
}

/** Configure a decoder from packaged properties whose lifetime ends here. */
Status setup_runfiles(Decoder& decoder, std::string_view terminal_name) {
  properties::Properties properties = runfiles_properties();
  return decoder.setup(properties, terminal_name);
}

/** Configure a decoder from one test's primary and user roots. */
Status setup_from_roots(Decoder& decoder, const ProfileRoots& roots,
                        std::string_view terminal_name) {
  properties::Properties properties{roots.primary, roots.user};
  return decoder.setup(properties, terminal_name);
}

/** Feed bytes and return all immediately emitted events. */
std::vector<Event> decode(Decoder& decoder, std::string_view bytes) {
  std::vector<Event> events;
  EXPECT_EQ(decoder.feed(bytes, events), Status::OK);
  return events;
}

/** Return whether an event vector contains a selected event alternative. */
template <typename Type>
bool contains_event(const std::vector<Event>& events) {
  for (const Event& event : events) {
    if (std::holds_alternative<Type>(event)) {
      return true;
    }
  }
  return false;
}

TEST(TerminalInputActionTest, StoresEventsAndProtocolsWithValueSemantics) {
  const InputAction event_action{Event{KeyEvent{.key = NamedKey::PAGE_DOWN}}};
  const InputAction protocol_action{InputProtocol::PASTE_BEGIN};

  ASSERT_NE(event_action.event(), nullptr);
  EXPECT_EQ(*event_action.event(), Event{KeyEvent{.key = NamedKey::PAGE_DOWN}});
  EXPECT_EQ(event_action.protocol(), nullptr);
  EXPECT_EQ(protocol_action.event(), nullptr);
  ASSERT_NE(protocol_action.protocol(), nullptr);
  EXPECT_EQ(protocol_action.protocol()->protocol, InputProtocol::PASTE_BEGIN);
  EXPECT_TRUE(InputAction{}.empty());
  EXPECT_EQ(event_action, event_action);
}

TEST(TerminalInputSetupTest, PucConfigAppearsAtItsDeclaredRunfilesPath) {
  const properties::LoadResult loaded = load_runfile("input_keys.toml");
  ASSERT_EQ(loaded.status, properties::Status::OK);
  EXPECT_EQ(loaded.find("version").as_integer(), 1);
  EXPECT_EQ(loaded.find("mapping").type(), properties::ValueType::ARRAY);
  EXPECT_EQ(loaded.find("terminfo").type(), properties::ValueType::ARRAY);

  for (const std::string_view path :
       {"darwin-defaults.toml", "linux-defaults.toml", "bsd-defaults.toml"}) {
    const properties::LoadResult defaults = load_runfile(path);
    ASSERT_EQ(defaults.status, properties::Status::OK) << path;
    EXPECT_EQ(defaults.find("version").as_integer(), 1) << path;
    ASSERT_EQ(defaults.find("mapping").type(), properties::ValueType::ARRAY)
        << path;
    const std::size_t expected_mappings =
        path == "darwin-defaults.toml" ? 20U : 1U;
    EXPECT_EQ(defaults.find("mapping").size(), expected_mappings) << path;
    EXPECT_EQ(defaults.find("mapping").at(0U).find("kind").as_string(),
              "command")
        << path;
    EXPECT_EQ(defaults.find("mapping").at(0U).find("command").as_string(),
              "COPY")
        << path;
  }

  const properties::LoadResult ghostty =
      load_runfile("terminals/xterm-ghostty.toml");
  ASSERT_EQ(ghostty.status, properties::Status::OK);
  EXPECT_EQ(ghostty.find("version").as_integer(), 1);
  ASSERT_EQ(ghostty.find("mapping").type(), properties::ValueType::ARRAY);
  EXPECT_EQ(ghostty.find("mapping").size(), 4U);
}

TEST(TerminalInputSetupTest, SelectsAStableDefaultsPathForEverySupportedOs) {
  EXPECT_EQ(operating_system_defaults_path(OperatingSystem::DARWIN),
            "darwin-defaults.toml");
  EXPECT_EQ(operating_system_defaults_path(OperatingSystem::LINUX),
            "linux-defaults.toml");
  EXPECT_EQ(operating_system_defaults_path(OperatingSystem::BSD),
            "bsd-defaults.toml");
  EXPECT_TRUE(operating_system_defaults_path(OperatingSystem::OTHER).empty());
  EXPECT_FALSE(
      operating_system_defaults_path(current_operating_system()).empty());
}

TEST(TerminalInputSetupTest, PackagedOsDefaultsEmitCopyDirectlyFromTheTrie) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  std::string_view copy_sequence;
  switch (current_operating_system()) {
    case OperatingSystem::DARWIN:
      copy_sequence = "\x1b[99;9u";
      break;
    case OperatingSystem::LINUX:
    case OperatingSystem::BSD:
      copy_sequence = "\x1b[99;6u";
      break;
    case OperatingSystem::OTHER:
      FAIL() << "The test target has no packaged OS defaults";
      return;
  }

  const std::vector<Event> events = decode(decoder, copy_sequence);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front(), Event{CommandEvent{.command = Command::COPY}});
}

TEST(TerminalInputSetupTest, EscapeColonEmitsCommandModeIntentFromTheTrie) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x1b:");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front(),
            Event{CommandEvent{.command = Command::ENTER_COMMAND_MODE}});
}

TEST(TerminalInputSetupTest,
     EscapeGreaterThanEmitsTerminalModeIntentFromTheTrie) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x1b>");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front(),
            Event{CommandEvent{.command = Command::ENTER_TERMINAL_MODE}});
}

TEST(TerminalInputIntegrationTest,
     LegacyAndEnhancedEscapeColonReachTheSameFrameTransition) {
  const auto exercise = [](std::string_view bytes) {
    Decoder decoder;
    ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);
    std::vector<Event> events = decode(decoder, bytes);
    ASSERT_FALSE(events.empty());

    tui::InputFrame frame;
    const auto start = tui::InputFrame::Clock::time_point{} + 1s;
    ASSERT_EQ(frame.handle_event(Event{TextEvent{.utf8 = "normal"}}, start),
              tui::Status::OK);
    for (std::size_t index = 0U; index < events.size(); ++index) {
      ASSERT_EQ(frame.handle_event(events[index], start + (index + 1U) * 1ms),
                tui::Status::OK);
    }
    EXPECT_EQ(frame.snapshot().mode, tui::InputMode::COMMAND);
    EXPECT_EQ(frame.snapshot().input_text, "normal");
    EXPECT_TRUE(frame.snapshot().command_text.empty());
  };

  exercise("\x1b:");
  // REPORT_ALL_KEYS emits the Shift press used to type colon as its own event.
  exercise("\x1b[27u\x1b[57441;2u\x1b[59:58;2;58u");
}

TEST(TerminalInputIntegrationTest,
     LegacyAndEnhancedEscapeGreaterThanReachTheSameFrameTransition) {
  const auto exercise = [](std::string_view bytes) {
    Decoder decoder;
    ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);
    std::vector<Event> events = decode(decoder, bytes);
    ASSERT_FALSE(events.empty());

    tui::InputFrame frame;
    const auto start = tui::InputFrame::Clock::time_point{} + 1s;
    ASSERT_EQ(frame.handle_event(Event{TextEvent{.utf8 = "normal"}}, start),
              tui::Status::OK);
    for (std::size_t index = 0U; index < events.size(); ++index) {
      ASSERT_EQ(frame.handle_event(events[index], start + (index + 1U) * 1ms),
                tui::Status::OK);
    }
    EXPECT_EQ(frame.snapshot().mode, tui::InputMode::TERMINAL);
    EXPECT_EQ(frame.snapshot().input_text, "normal");
    EXPECT_TRUE(frame.snapshot().terminal_session_active);
  };

  exercise("\x1b>");
  // REPORT_ALL_KEYS emits the Shift press used to type greater-than separately.
  exercise("\x1b[27u\x1b[57441;2u\x1b[46:62;2;62u");
}

TEST(TerminalInputIntegrationTest,
     DecoderNormalizedDoubleEscapeClearsTheFrame) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);
  const std::vector<Event> events = decode(decoder, "\x1b\x1b");
  ASSERT_EQ(events.size(), 1U);

  tui::InputFrame frame;
  ASSERT_EQ(frame.handle_event(Event{TextEvent{.utf8 = "clear me"}}),
            tui::Status::OK);
  ASSERT_EQ(frame.handle_event(events.front()), tui::Status::OK);
  EXPECT_TRUE(frame.snapshot().input_text.empty());
  EXPECT_FALSE(frame.snapshot().escape_armed);
}

TEST(TerminalInputIntegrationTest,
     DecoderTimeoutArmsThenFrameTimeoutDismissesEscapePrompt) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);
  std::vector<Event> events;
  ASSERT_EQ(decoder.feed("\x1b", events), Status::OK);
  ASSERT_TRUE(events.empty());
  const std::optional<TimeoutInput> pending = decoder.pending_timeout();
  ASSERT_TRUE(pending.has_value());
  ASSERT_EQ(decoder.handle_timeout(*pending, events), Status::OK);
  ASSERT_EQ(events.size(), 1U);

  tui::InputFrame frame;
  const auto start = tui::InputFrame::Clock::time_point{} + 1s;
  ASSERT_EQ(frame.handle_event(Event{TextEvent{.utf8 = "keep"}}, start),
            tui::Status::OK);
  ASSERT_EQ(frame.handle_event(events.front(), start + 1ms), tui::Status::OK);
  EXPECT_TRUE(frame.snapshot().escape_armed);
  frame.advance_time(start + 501ms);
  EXPECT_FALSE(frame.snapshot().escape_armed);
  EXPECT_EQ(frame.snapshot().input_text, "keep");
}

TEST(TerminalInputSetupTest, DarwinDefaultsDeclareMacNavigationCommands) {
  const properties::LoadResult defaults = load_runfile("darwin-defaults.toml");
  ASSERT_EQ(defaults.status, properties::Status::OK);
  const properties::Value mappings = defaults.find("mapping");
  ASSERT_EQ(mappings.size(), 20U);
  EXPECT_EQ(mappings.at(1U).find("command").as_string(), "MOVE_WORD_LEFT");
  EXPECT_EQ(mappings.at(2U).find("command").as_string(), "MOVE_WORD_RIGHT");
  EXPECT_EQ(mappings.at(3U).find("sequence").as_string(), "<ESC>b");
  EXPECT_EQ(mappings.at(4U).find("sequence").as_string(), "<ESC>f");
  EXPECT_EQ(mappings.at(10U).find("sequence").as_string(), "<CSI>57353;3u");
  EXPECT_EQ(mappings.at(11U).find("command").as_string(), "MOVE_ROW_START");
  EXPECT_EQ(mappings.at(14U).find("command").as_string(), "MOVE_BUFFER_END");
  EXPECT_EQ(mappings.at(15U).find("sequence").as_string(), "<CSI>57350;9u");
  EXPECT_EQ(mappings.at(18U).find("command").as_string(), "MOVE_BUFFER_END");
  EXPECT_EQ(mappings.at(19U).find("command").as_string(), "SELECT_ALL");
}

TEST(TerminalInputSetupTest, GhosttyProfileDeclaresReachableMacFallbacks) {
  const properties::LoadResult profile =
      load_runfile("terminals/xterm-ghostty.toml");
  ASSERT_EQ(profile.status, properties::Status::OK);
  const properties::Value mappings = profile.find("mapping");
  ASSERT_EQ(mappings.size(), 4U);
  EXPECT_EQ(mappings.at(0U).find("sequence").as_string(), "^A");
  EXPECT_EQ(mappings.at(0U).find("command").as_string(), "MOVE_ROW_START");
  EXPECT_EQ(mappings.at(1U).find("sequence").as_string(), "<CSI>97;5u");
  EXPECT_EQ(mappings.at(1U).find("command").as_string(), "SELECT_ALL");
  EXPECT_EQ(mappings.at(2U).find("sequence").as_string(), "^E");
  EXPECT_EQ(mappings.at(2U).find("command").as_string(), "MOVE_ROW_END");
  EXPECT_EQ(mappings.at(3U).find("command").as_string(), "SELECT_ALL");
}

TEST(TerminalInputSetupTest, PackagedGhosttyMacFallbacksDecodeFromTrie) {
  Decoder decoder;
  const Status setup = setup_runfiles(decoder, "xterm-ghostty");
  if (setup == Status::TERMINFO_LOAD_FAILED) {
    GTEST_SKIP() << "xterm-ghostty terminfo is not installed on this host";
  }
  ASSERT_EQ(setup, Status::OK);

  const struct {
    std::string_view sequence;
    Command command;
  } cases[] = {
      {"\x01", Command::MOVE_ROW_START},
      {"\x1b[97;5u", Command::SELECT_ALL},
      {"\x05", Command::MOVE_ROW_END},
      {"\x1b[97;6u", Command::SELECT_ALL},
  };
  for (const auto& test_case : cases) {
    const std::vector<Event> events = decode(decoder, test_case.sequence);
    ASSERT_EQ(events.size(), 1U) << test_case.sequence;
    EXPECT_EQ(events.front(), Event{CommandEvent{.command = test_case.command}})
        << test_case.sequence;
  }
}

TEST(TerminalInputSetupTest, DarwinDefaultsDecodeCommonMacNavigationForms) {
  if (current_operating_system() != OperatingSystem::DARWIN) {
    GTEST_SKIP() << "Darwin defaults are active only on macOS";
  }

  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);
  const struct {
    std::string_view sequence;
    Command command;
  } cases[] = {
      {"\x1b"
       "b",
       Command::MOVE_WORD_LEFT},
      {"\x1b"
       "f",
       Command::MOVE_WORD_RIGHT},
      {"\x1b[57350;3u", Command::MOVE_WORD_LEFT},
      {"\x1b[57351;3u", Command::MOVE_WORD_RIGHT},
      {"\x1b[57352;3u", Command::MOVE_PAGE_UP},
      {"\x1b[57353;3u", Command::MOVE_PAGE_DOWN},
      {"\x1b[57350;9u", Command::MOVE_ROW_START},
      {"\x1b[57351;9u", Command::MOVE_ROW_END},
      {"\x1b[57352;9u", Command::MOVE_BUFFER_START},
      {"\x1b[57353;9u", Command::MOVE_BUFFER_END},
      {"\x1b[97;9u", Command::SELECT_ALL},
  };
  for (const auto& test_case : cases) {
    const std::vector<Event> events = decode(decoder, test_case.sequence);
    ASSERT_EQ(events.size(), 1U) << test_case.sequence;
    EXPECT_EQ(events.front(), Event{CommandEvent{.command = test_case.command}})
        << test_case.sequence;
  }
}

TEST(TerminalInputSetupTest, PlainControlCRemainsAKeyAndNotACopyCommand) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x03");
  ASSERT_EQ(events.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<KeyEvent>(events.front()));
  const KeyEvent& key = std::get<KeyEvent>(events.front());
  EXPECT_EQ(key.key, KeyCode{U'c'});
  EXPECT_TRUE(key.modifiers.contains(Modifier::CONTROL));
}

TEST(TerminalInputSetupTest, LoadsThePackagedProfileAtRuntime) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  const std::vector<Event> focus = decode(decoder, "\x1b[I");
  ASSERT_EQ(focus.size(), 1U);
  EXPECT_EQ(focus.front(), Event{FocusEvent{.focused = true}});

  const std::vector<Event> mouse = decode(decoder, "\x1b[<0;2;3M");
  ASSERT_EQ(mouse.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<MouseEvent>(mouse.front()));
  EXPECT_EQ(std::get<MouseEvent>(mouse.front()).position,
            (CellPosition{.x = 1, .y = 2}));
}

TEST(TerminalInputSetupTest,
     AppliesTerminfoThenUniversalAndOsThenFinalTerminalProfile) {
  const ProfileRoots roots = profile_roots("source_hierarchy");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[terminfo]]
capability = "kcuu1"
key = "UP"

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "<CSI>A"
kind = "key"
key = "HOME"
)toml"));
  ASSERT_TRUE(
      write_profile(roots.primary / "terminals" / "xterm-256color.toml", R"toml(
version = 1

[[mapping]]
sequence = "<CSI>A"
kind = "key"
key = "PAGE_UP"

[[mapping]]
sequence = "y"
kind = "key"
key = "PAGE_DOWN"
)toml"));
  ASSERT_TRUE(write_profile(roots.user / "input_keys.toml", R"toml(
[[mapping]]
sequence = "<CSI>A"
kind = "key"
key = "END"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x1b[Ay");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], Event{KeyEvent{.key = NamedKey::PAGE_UP}});
  EXPECT_EQ(events[1], Event{KeyEvent{.key = NamedKey::PAGE_DOWN}});
}

TEST(TerminalInputSetupTest,
     SpecificProfileMappingsSurviveGenericProtocolFallbacks) {
  const ProfileRoots roots = profile_roots("specific_profile_paths");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "<ESC>"
kind = "protocol"
protocol = "ESCAPE"

[[mapping]]
sequence = "<CSI>"
kind = "protocol"
protocol = "CSI"

[[mapping]]
sequence = "<OSC>"
kind = "protocol"
protocol = "OSC"
)toml"));
  ASSERT_TRUE(write_profile(roots.primary / "terminals" / "xterm-256color.toml",
                            R"toml(
version = 1

[[mapping]]
sequence = "<CSI>A"
kind = "key"
key = "PAGE_UP"

[[mapping]]
sequence = "<OSC>52;"
kind = "protocol"
protocol = "OSC52"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);

  const std::vector<Event> events =
      decode(decoder, "\x1b[A\x1b]52;c;YQ==\x1b\\");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], Event{KeyEvent{.key = NamedKey::PAGE_UP}});
  ASSERT_TRUE(std::holds_alternative<ClipboardEvent>(events[1]));
  EXPECT_EQ(std::get<ClipboardEvent>(events[1]).data, "a");
}

TEST(TerminalInputSetupTest,
     SpecificTerminfoMappingsSurviveGenericProtocolFallbacks) {
  const ProfileRoots roots = profile_roots("specific_terminfo_paths");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[terminfo]]
capability = "kf13"
key = "F13"

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "<ESC>"
kind = "protocol"
protocol = "ESCAPE"

[[mapping]]
sequence = "<CSI>"
kind = "protocol"
protocol = "CSI"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x1b[1;2P");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0], Event{KeyEvent{.key = NamedKey::F13}});
}

TEST(TerminalInputSetupTest, MissingRuntimeConfigurationIsAnError) {
  const ProfileRoots roots = profile_roots("missing_profile");
  properties::Properties configurations{roots.primary, roots.user};
  Decoder decoder;

  EXPECT_EQ(decoder.setup(configurations, "xterm-256color"),
            Status::CONFIGURATION_LOAD_FAILED);
  const std::vector<Event> events = decode(decoder, "x");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(std::get<UnknownEvent>(events.front()).bytes, "x");
}

TEST(TerminalInputSetupTest, MissingOperatingSystemDefaultsIsAnError) {
  const ProfileRoots roots = profile_roots("missing_os_defaults");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"
)toml"));
  std::error_code error;
  ASSERT_TRUE(std::filesystem::remove(
      roots.primary /
          operating_system_defaults_path(current_operating_system()),
      error));
  ASSERT_FALSE(error);

  Decoder decoder;
  EXPECT_EQ(setup_from_roots(decoder, roots, "xterm-256color"),
            Status::CONFIGURATION_LOAD_FAILED);
}

TEST(TerminalInputSetupTest, UserOsDefaultsReplaceAnExactSystemCommand) {
  const ProfileRoots roots = profile_roots("os_user_override");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"
)toml"));
  const std::string_view defaults_path =
      operating_system_defaults_path(current_operating_system());
  ASSERT_TRUE(write_profile(roots.primary / defaults_path, R"toml(
version = 1
[[mapping]]
sequence = "z"
kind = "command"
command = "COPY"
)toml"));
  ASSERT_TRUE(write_profile(roots.user / defaults_path, R"toml(
[[mapping]]
sequence = "z"
kind = "key"
key = "END"
)toml"));

  Decoder decoder;
  ASSERT_EQ(setup_from_roots(decoder, roots, "xterm-256color"), Status::OK);
  EXPECT_EQ(decode(decoder, "z"),
            (std::vector<Event>{Event{KeyEvent{.key = NamedKey::END}}}));
}

TEST(TerminalInputSetupTest, RejectsEveryPrefixRelatedCommandChord) {
  const ProfileRoots roots = profile_roots("command_prefix_conflicts");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"
)toml"));
  ASSERT_TRUE(write_profile(roots.primary / operating_system_defaults_path(
                                                current_operating_system()),
                            R"toml(
version = 1
[[mapping]]
sequence = "x"
kind = "command"
command = "COPY"

[[mapping]]
sequence = "xy"
kind = "command"
command = "COPY"

[[mapping]]
sequence = "xyz"
kind = "command"
command = "COPY"
)toml"));

  Decoder decoder;
  EXPECT_EQ(setup_from_roots(decoder, roots, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);
}

TEST(TerminalInputSetupTest, RejectsDuplicateCommandsInOneSourceFile) {
  const ProfileRoots roots = profile_roots("exact_command_conflict");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"
)toml"));
  ASSERT_TRUE(write_profile(roots.primary / operating_system_defaults_path(
                                                current_operating_system()),
                            R"toml(
version = 1
[[mapping]]
sequence = "x"
kind = "command"
command = "COPY"

[[mapping]]
sequence = "x"
kind = "command"
command = "COPY"
)toml"));

  Decoder decoder;
  EXPECT_EQ(setup_from_roots(decoder, roots, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);
}

TEST(TerminalInputSetupTest, RejectsUnknownCommandNames) {
  const ProfileRoots roots = profile_roots("unknown_command");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"
)toml"));
  ASSERT_TRUE(write_profile(roots.primary / operating_system_defaults_path(
                                                current_operating_system()),
                            R"toml(
version = 1
[[mapping]]
sequence = "x"
kind = "command"
command = "NOT_A_COMMAND"
)toml"));

  Decoder decoder;
  EXPECT_EQ(setup_from_roots(decoder, roots, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);
}

TEST(TerminalInputSetupTest, FailedSetupPreservesThePreviousDecoder) {
  Decoder decoder;
  ASSERT_EQ(setup_runfiles(decoder, "xterm-256color"), Status::OK);

  const ProfileRoots roots = profile_roots("transactional_setup");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1
[[mapping]]
sequence = "broken"
kind = "key"
key = "NOT_A_KEY"
)toml"));
  properties::Properties invalid{roots.primary, roots.user};
  EXPECT_EQ(decoder.setup(invalid, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);

  const std::vector<Event> events = decode(decoder, "\x1b[I");
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events.front(), Event{FocusEvent{.focused = true}});
}

TEST(TerminalInputSetupTest, RejectsUnsafeTerminalProfileNames) {
  Decoder decoder;
  EXPECT_EQ(setup_runfiles(decoder, "../xterm"), Status::INVALID_ARGUMENT);
}

TEST(TerminalInputSetupTest,
     TerminalProfilesCannotMoveTerminfoAfterTheTerminfoLayer) {
  const ProfileRoots roots = profile_roots("late_terminfo");
  ASSERT_TRUE(
      write_profile(roots.primary / "input_keys.toml", "version = 1\n"));
  ASSERT_TRUE(
      write_profile(roots.primary / "terminals" / "xterm-256color.toml", R"toml(
version = 1
[[terminfo]]
capability = "kcuu1"
key = "UP"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  EXPECT_EQ(decoder.setup(configurations, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);
}

TEST(TerminalInputSetupTest,
     UnknownTriePathPreservesAndRetriesTheNewestByteFromRoot) {
  const ProfileRoots roots = profile_roots("unknown_path_retry");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "ab"
kind = "key"
key = "UP"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);
  const std::vector<Event> events = decode(decoder, "ax");

  ASSERT_EQ(events.size(), 2U);
  ASSERT_TRUE(std::holds_alternative<UnknownEvent>(events[0]));
  EXPECT_EQ(std::get<UnknownEvent>(events[0]).bytes, "a");
  ASSERT_TRUE(std::holds_alternative<TextEvent>(events[1]));
  EXPECT_EQ(std::get<TextEvent>(events[1]).utf8, "x");
}

TEST(TerminalInputSetupTest,
     DisabledProtocolMappingsDoNotFallThroughToGenericParsers) {
  const ProfileRoots roots = profile_roots("disabled_protocols");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "<ESC>"
kind = "protocol"
protocol = "ESCAPE"

[[mapping]]
sequence = "<CSI>"
kind = "protocol"
protocol = "CSI"

[[mapping]]
sequence = "<OSC>"
kind = "protocol"
protocol = "OSC"

[[mapping]]
sequence = "<CSI><LT>"
kind = "protocol"
protocol = "SGR_MOUSE"

[[mapping]]
sequence = "<OSC>52;"
kind = "protocol"
protocol = "OSC52"

[[mapping]]
sequence = "<CSI>200~"
kind = "protocol"
protocol = "PASTE_BEGIN"

[[mapping]]
sequence = "<CSI>201~"
kind = "protocol"
protocol = "PASTE_END"

[[mapping]]
sequence = "<CSI>I"
kind = "focus"
focused = true

[[mapping]]
sequence = "<CSI>A"
kind = "key"
key = "UP"

[[mapping]]
sequence = "^A"
kind = "key"
key = "a"
modifiers = ["CONTROL"]
)toml"));
  ASSERT_TRUE(write_profile(roots.user / "input_keys.toml", R"toml(
[[mapping]]
sequence = "<CSI><LT>"
disabled = true

[[mapping]]
sequence = "<OSC>52;"
disabled = true

[[mapping]]
sequence = "<CSI>200~"
disabled = true

[[mapping]]
sequence = "<CSI>I"
disabled = true

[[mapping]]
sequence = "<CSI>A"
disabled = true

[[mapping]]
sequence = "^A"
disabled = true
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);

  const std::vector<Event> focus = decode(decoder, "\x1b[I");
  EXPECT_TRUE(contains_event<UnknownEvent>(focus));
  EXPECT_FALSE(contains_event<FocusEvent>(focus));

  const std::vector<Event> mouse = decode(decoder, "\x1b[<0;2;3M");
  EXPECT_TRUE(contains_event<UnknownEvent>(mouse));
  EXPECT_FALSE(contains_event<MouseEvent>(mouse));
  EXPECT_FALSE(contains_event<ScrollEvent>(mouse));

  const std::vector<Event> clipboard = decode(decoder, "\x1b]52;c;YQ==\x1b\\");
  EXPECT_TRUE(contains_event<UnknownEvent>(clipboard));
  EXPECT_FALSE(contains_event<ClipboardEvent>(clipboard));

  const std::vector<Event> paste = decode(decoder, "\x1b[200~payload\x1b[201~");
  EXPECT_TRUE(contains_event<UnknownEvent>(paste));
  EXPECT_FALSE(contains_event<PasteEvent>(paste));

  const std::vector<Event> fixed_key = decode(decoder, "\x1b[A");
  ASSERT_EQ(fixed_key.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<UnknownEvent>(fixed_key.front()));

  const std::vector<Event> control_key = decode(decoder, "\x01");
  ASSERT_EQ(control_key.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<UnknownEvent>(control_key.front()));
}

TEST(TerminalInputSetupTest, ExactKeysAreNotHiddenInsideProtocolParsers) {
  const ProfileRoots roots = profile_roots("no_hidden_exact_keys");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[mapping]]
sequence = ""
kind = "protocol"
protocol = "TEXT"

[[mapping]]
sequence = "<ESC>"
kind = "protocol"
protocol = "ESCAPE"

[[mapping]]
sequence = "<CSI>"
kind = "protocol"
protocol = "CSI"
)toml"));

  Decoder decoder;
  properties::Properties configurations{roots.primary, roots.user};
  ASSERT_EQ(decoder.setup(configurations, "xterm-256color"), Status::OK);

  const std::vector<Event> events = decode(decoder, "\x01\x1b[A");
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0], (Event{UnknownEvent{
                           .reason = UnknownInputReason::UNSUPPORTED_SEQUENCE,
                           .bytes  = "\x01",
                       }}));
  EXPECT_EQ(events[1], (Event{UnknownEvent{
                           .reason = UnknownInputReason::UNSUPPORTED_SEQUENCE,
                           .bytes  = "\x1b[A",
                       }}));
}

TEST(TerminalInputSetupTest, RejectsInvalidRootAndTerminfoActions) {
  const ProfileRoots roots = profile_roots("invalid_internal_actions");
  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[mapping]]
sequence = ""
kind = "key"
key = "UP"
)toml"));

  Decoder decoder;
  properties::Properties invalid_root_action{roots.primary, roots.user};
  EXPECT_EQ(decoder.setup(invalid_root_action, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);

  ASSERT_TRUE(write_profile(roots.primary / "input_keys.toml", R"toml(
version = 1

[[terminfo]]
capability = "bad-name"
key = "UP"
)toml"));
  EXPECT_EQ(decoder.setup(invalid_root_action, "xterm-256color"),
            Status::CONFIGURATION_PARSE_FAILED);
}

}  // namespace
}  // namespace puc::terminal
