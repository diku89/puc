#pragma once

/**
 * @file event_messages.hpp
 * @brief Conversion between terminal events and portable channel messages.
 */

#include "msgs/status.hpp"
#include "msgs/terminal_msgs.hpp"
#include "puc-cli/tui/terminal/event.hpp"

namespace puc::terminal {

/** Convert one normalized in-process event to its portable message form. */
msg::Status to_message(const Event& event, msg::TerminalInputEvent& output);

/** Convert one validated portable message back to a normalized event. */
msg::Status from_message(const msg::TerminalInputEvent& message, Event& output);

}  // namespace puc::terminal
