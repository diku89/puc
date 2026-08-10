#pragma once

/**
 * @file utf8.hpp
 * @brief Validation utilities for complete UTF-8 text.
 */

#include <string_view>

namespace puc::utf8 {

/**
 * Return whether `text` is a complete, well-formed UTF-8 byte sequence.
 *
 * Validation accepts every Unicode scalar value, including U+0000, and
 * rejects truncated sequences, malformed continuation bytes, overlong
 * encodings, UTF-16 surrogate code points, and values above U+10FFFF.
 */
bool is_valid(std::string_view text) noexcept;

}  // namespace puc::utf8
