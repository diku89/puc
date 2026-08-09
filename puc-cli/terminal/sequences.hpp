#pragma once

/**
 * @file sequences.hpp
 * @brief Encoding of the terminal control sequences PUC deliberately owns.
 */

#include <cstdint>
#include <string>
#include <string_view>

#include "puc-cli/terminal/status.hpp"

namespace puc {
namespace terminal {

/** Whitelisted terminal modes that PUC may enable and later restore. */
enum class Mode {
  ALTERNATE_SCREEN,
  CURSOR_VISIBLE,
  AUTO_WRAP,
  BRACKETED_PASTE,
  FOCUS_REPORTING,
  MOUSE_BUTTON_TRACKING,
  MOUSE_DRAG_TRACKING,
  MOUSE_MOTION_TRACKING,
  SGR_MOUSE,
  APPLICATION_CURSOR,
  APPLICATION_KEYPAD,
  SYNCHRONIZED_OUTPUT,
};

/** Kitty progressive-keyboard enhancement flags. */
enum class KeyboardEnhancement : std::uint32_t {
  DISAMBIGUATE_ESCAPE_CODES = 1U << 0U,
  REPORT_EVENT_TYPES        = 1U << 1U,
  REPORT_ALTERNATE_KEYS     = 1U << 2U,
  REPORT_ALL_KEYS           = 1U << 3U,
  REPORT_ASSOCIATED_TEXT    = 1U << 4U,
};

/** Combine two keyboard enhancement flags into a wire-ready bit field. */
constexpr std::uint32_t operator|(KeyboardEnhancement left,
                                  KeyboardEnhancement right) noexcept {
  return static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right);
}

/**
 * Return the sequence that enables or disables one named mode.
 *
 * @param[in] mode Whitelisted mode.
 * @param[in] enabled Desired state of the named feature.
 * @return Static escape sequence.
 */
std::string_view mode_sequence(Mode mode, bool enabled) noexcept;

/** Return the Kitty keyboard-protocol capability query. */
constexpr std::string_view kitty_keyboard_query() noexcept { return "\x1b[?u"; }

/** Return the sequence that pops one Kitty keyboard mode stack entry. */
constexpr std::string_view kitty_keyboard_pop() noexcept { return "\x1b[<u"; }

/**
 * Build a sequence that pushes Kitty keyboard enhancement flags.
 *
 * @param[in] flags Bit field formed from KeyboardEnhancement values.
 * @param[out] output Receives `CSI > flags u` on success.
 * @return Status::OK, or Status::INVALID_ARGUMENT for unsupported flag bits.
 */
Status build_kitty_keyboard_push(std::uint32_t flags,
                                 std::string& output) noexcept;

}  // namespace terminal
}  // namespace puc
