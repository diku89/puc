/**
 * @file utf8.cpp
 * @brief Complete UTF-8 validation.
 */

#include "utils/utf8/utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace puc::utf8 {

bool is_valid(std::string_view text) noexcept {
  std::size_t offset = 0U;
  while (offset < text.size()) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first <= 0x7fU) {
      ++offset;
      continue;
    }

    std::size_t length      = 0U;
    std::uint32_t codepoint = 0U;
    std::uint32_t minimum   = 0U;
    if (first >= 0xc2U && first <= 0xdfU) {
      length    = 2U;
      codepoint = first & 0x1fU;
      minimum   = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
      length    = 3U;
      codepoint = first & 0x0fU;
      minimum   = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      length    = 4U;
      codepoint = first & 0x07U;
      minimum   = 0x10000U;
    } else {
      return false;
    }

    if (text.size() - offset < length) {
      return false;
    }
    for (std::size_t index = 1U; index < length; ++index) {
      const auto continuation =
          static_cast<unsigned char>(text[offset + index]);
      if ((continuation & 0xc0U) != 0x80U) {
        return false;
      }
      codepoint = (codepoint << 6U) | (continuation & 0x3fU);
    }
    if (codepoint < minimum || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
      return false;
    }
    offset += length;
  }
  return true;
}

}  // namespace puc::utf8
