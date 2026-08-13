/**
 * @file sequences.cpp
 * @brief Implementation of deliberately supported terminal mode encodings.
 */

#include "puc-cli/tui/terminal/sequences.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <system_error>

#include "utils/logger/logger.hpp"

/** @cond TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("TerminalSequences");
/** @endcond */

namespace puc {
namespace terminal {

std::string_view mode_sequence(Mode mode, bool enabled) noexcept {
  switch (mode) {
    case Mode::ALTERNATE_SCREEN:
      return enabled ? "\x1b[?1049h" : "\x1b[?1049l";
    case Mode::CURSOR_VISIBLE:
      return enabled ? "\x1b[?25h" : "\x1b[?25l";
    case Mode::AUTO_WRAP:
      return enabled ? "\x1b[?7h" : "\x1b[?7l";
    case Mode::BRACKETED_PASTE:
      return enabled ? "\x1b[?2004h" : "\x1b[?2004l";
    case Mode::FOCUS_REPORTING:
      return enabled ? "\x1b[?1004h" : "\x1b[?1004l";
    case Mode::MOUSE_BUTTON_TRACKING:
      return enabled ? "\x1b[?1000h" : "\x1b[?1000l";
    case Mode::MOUSE_DRAG_TRACKING:
      return enabled ? "\x1b[?1002h" : "\x1b[?1002l";
    case Mode::MOUSE_MOTION_TRACKING:
      return enabled ? "\x1b[?1003h" : "\x1b[?1003l";
    case Mode::SGR_MOUSE:
      return enabled ? "\x1b[?1006h" : "\x1b[?1006l";
    case Mode::APPLICATION_CURSOR:
      return enabled ? "\x1b[?1h" : "\x1b[?1l";
    case Mode::APPLICATION_KEYPAD:
      return enabled ? "\x1b=" : "\x1b>";
    case Mode::SYNCHRONIZED_OUTPUT:
      return enabled ? "\x1b[?2026h" : "\x1b[?2026l";
  }
  return {};
}

Status build_kitty_keyboard_push(std::uint32_t flags,
                                 std::string& output) noexcept {
  constexpr std::uint32_t kKnownFlags = 0x1fU;
  if ((flags & ~kKnownFlags) != 0U) {
    Logger<ERROR> << "Cannot encode unknown Kitty keyboard flags: " << flags;
    return Status::INVALID_ARGUMENT;
  }

  std::array<char, 10> digits{};
  const auto [end, error] =
      std::to_chars(digits.data(), digits.data() + digits.size(), flags);
  if (error != std::errc{}) {
    Logger<ERROR> << "Could not encode Kitty keyboard flags";
    return Status::INVALID_ARGUMENT;
  }

  output.assign("\x1b[>");
  output.append(digits.data(), end);
  output.push_back('u');
  return Status::OK;
}

}  // namespace terminal
}  // namespace puc
