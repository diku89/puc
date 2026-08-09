/**
 * @file clipboard.cpp
 * @brief Bounded OSC 52 and Base64 clipboard protocol implementation.
 */

#include "puc-cli/terminal/clipboard.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "utils/logger/logger.hpp"

/** @cond TERMINAL_LOGGER_MODULE */
LOGGER_MODULE("TerminalClipboard");
/** @endcond */

namespace puc {
namespace terminal {
namespace {

constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** Return the six-bit value of a Base64 character or -1 when invalid. */
constexpr int base64_value(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 26;
  }
  if (character >= '0' && character <= '9') {
    return character - '0' + 52;
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return -1;
}

/** Append Base64 without inspecting or logging the underlying data. */
void append_base64(std::string_view data, std::string& output) {
  std::size_t offset = 0;
  while (offset + 3U <= data.size()) {
    const std::uint32_t value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset]))
         << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(data[offset + 1U]))
         << 8U) |
        static_cast<std::uint32_t>(
            static_cast<unsigned char>(data[offset + 2U]));
    output.push_back(kBase64Alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(kBase64Alphabet[(value >> 12U) & 0x3fU]);
    output.push_back(kBase64Alphabet[(value >> 6U) & 0x3fU]);
    output.push_back(kBase64Alphabet[value & 0x3fU]);
    offset += 3U;
  }

  const std::size_t remaining = data.size() - offset;
  if (remaining == 1U) {
    const std::uint32_t value =
        static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset]))
        << 16U;
    output.push_back(kBase64Alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(kBase64Alphabet[(value >> 12U) & 0x3fU]);
    output.append("==");
  } else if (remaining == 2U) {
    const std::uint32_t value =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset]))
         << 16U) |
        (static_cast<std::uint32_t>(
             static_cast<unsigned char>(data[offset + 1U]))
         << 8U);
    output.push_back(kBase64Alphabet[(value >> 18U) & 0x3fU]);
    output.push_back(kBase64Alphabet[(value >> 12U) & 0x3fU]);
    output.push_back(kBase64Alphabet[(value >> 6U) & 0x3fU]);
    output.push_back('=');
  }
}

}  // namespace

Status build_clipboard_write(ClipboardSelection selection,
                             std::string_view data, std::string& output,
                             std::size_t maximum_bytes) {
  if (data.size() > maximum_bytes) {
    Logger<WARN> << "Refused OSC 52 clipboard write of " << data.size()
                 << " bytes because the configured limit is " << maximum_bytes;
    return Status::OUTPUT_LIMIT_EXCEEDED;
  }
  if (data.size() > (std::numeric_limits<std::size_t>::max() - 2U) / 3U) {
    Logger<ERROR> << "Clipboard payload size overflows Base64 arithmetic";
    return Status::OUTPUT_LIMIT_EXCEEDED;
  }

  const std::size_t encoded_size      = ((data.size() + 2U) / 3U) * 4U;
  constexpr std::size_t kFramingBytes = 9U;
  if (encoded_size > std::numeric_limits<std::size_t>::max() - kFramingBytes) {
    Logger<ERROR> << "Clipboard output size overflows addressable storage";
    return Status::OUTPUT_LIMIT_EXCEEDED;
  }

  std::string encoded;
  encoded.reserve(encoded_size + kFramingBytes);
  encoded.append("\x1b]52;");
  encoded.push_back(clipboard_selector(selection));
  encoded.push_back(';');
  append_base64(data, encoded);
  encoded.append("\x1b\\");
  output.swap(encoded);
  Logger<DEBUG> << "Encoded bounded OSC 52 clipboard write of " << data.size()
                << " bytes";
  return Status::OK;
}

void build_clipboard_query(ClipboardSelection selection, std::string& output) {
  output.assign("\x1b]52;");
  output.push_back(clipboard_selector(selection));
  output.append(";?\x1b\\");
}

Status decode_clipboard_payload(ClipboardSelection selection,
                                std::string_view encoded, ClipboardEvent& event,
                                std::size_t maximum_bytes) {
  std::size_t padding = 0;
  while (padding < encoded.size() &&
         encoded[encoded.size() - 1U - padding] == '=') {
    ++padding;
  }
  if (padding > 2U) {
    return Status::INVALID_ARGUMENT;
  }

  const std::size_t content_size = encoded.size() - padding;
  const std::size_t remainder    = content_size % 4U;
  if (remainder == 1U || (padding != 0U && encoded.size() % 4U != 0U) ||
      (padding == 1U && remainder != 3U) ||
      (padding == 2U && remainder != 2U)) {
    return Status::INVALID_ARGUMENT;
  }

  const std::size_t decoded_size = (content_size / 4U) * 3U +
                                   (remainder == 2U ? 1U : 0U) +
                                   (remainder == 3U ? 2U : 0U);
  if (decoded_size > maximum_bytes) {
    Logger<WARN> << "Refused OSC 52 clipboard response whose decoded size is "
                 << decoded_size << " bytes";
    return Status::INPUT_LIMIT_EXCEEDED;
  }

  std::string decoded;
  decoded.reserve(decoded_size);
  std::uint32_t accumulator = 0;
  unsigned int bit_count    = 0;
  for (std::size_t index = 0; index < content_size; ++index) {
    const int value = base64_value(encoded[index]);
    if (value < 0) {
      return Status::INVALID_ARGUMENT;
    }
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
    bit_count += 6U;
    if (bit_count >= 8U) {
      bit_count -= 8U;
      decoded.push_back(static_cast<char>((accumulator >> bit_count) & 0xffU));
    }
  }

  if (decoded.size() != decoded_size ||
      (bit_count != 0U && (accumulator & ((1U << bit_count) - 1U)) != 0U)) {
    return Status::INVALID_ARGUMENT;
  }

  event.selection = selection;
  event.data.swap(decoded);
  Logger<DEBUG> << "Decoded bounded OSC 52 clipboard response of "
                << event.data.size() << " bytes";
  return Status::OK;
}

}  // namespace terminal
}  // namespace puc
