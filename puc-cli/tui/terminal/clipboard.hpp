#pragma once

/**
 * @file clipboard.hpp
 * @brief Text clipboard encoding and decoding through terminal OSC 52.
 */

#include <cstddef>
#include <string>
#include <string_view>

#include "puc-cli/tui/terminal/event.hpp"
#include "puc-cli/tui/terminal/status.hpp"

namespace puc {
namespace terminal {

/** Default maximum decoded clipboard payload accepted or emitted by PUC. */
inline constexpr std::size_t kDefaultMaximumClipboardBytes = 1024U * 1024U;

/** Return the OSC 52 selector character for a normalized selection. */
constexpr char clipboard_selector(ClipboardSelection selection) noexcept {
  return selection == ClipboardSelection::PRIMARY ? 'p' : 'c';
}

/**
 * Encode UTF-8 bytes into an OSC 52 clipboard-write request.
 *
 * @param[in] selection Clipboard selection to replace.
 * @param[in] data Bytes to Base64 encode. They are never logged.
 * @param[out] output Receives the complete ST-terminated control sequence.
 * @param[in] maximum_bytes Maximum accepted unencoded payload size.
 * @return Status::OK or Status::OUTPUT_LIMIT_EXCEEDED.
 */
Status build_clipboard_write(
    ClipboardSelection selection, std::string_view data, std::string& output,
    std::size_t maximum_bytes = kDefaultMaximumClipboardBytes);

/** Build an OSC 52 query for one clipboard selection. */
void build_clipboard_query(ClipboardSelection selection, std::string& output);

/**
 * Decode the Base64 payload from an OSC 52 response.
 *
 * @param[in] selection Clipboard selection named by the response.
 * @param[in] encoded Base64 payload without OSC framing.
 * @param[out] event Receives decoded clipboard data only on success.
 * @param[in] maximum_bytes Maximum accepted decoded size.
 * @return Status::OK, Status::INVALID_ARGUMENT, or
 *         Status::INPUT_LIMIT_EXCEEDED.
 */
Status decode_clipboard_payload(
    ClipboardSelection selection, std::string_view encoded,
    ClipboardEvent& event,
    std::size_t maximum_bytes = kDefaultMaximumClipboardBytes);

}  // namespace terminal
}  // namespace puc
