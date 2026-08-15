/** @file sha256.cpp @brief Mbed TLS-backed SHA-256 implementation. */

#include "utils/hash/sha256.hpp"

#include <mbedtls/sha256.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace puc::hashing {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

}  // namespace

bool Hash256::empty() const noexcept {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t byte) { return byte == 0U; });
}

std::string Hash256::hex() const {
  std::string result(bytes.size() * 2U, '0');
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    result[index * 2U]      = kHexDigits[bytes[index] >> 4U];
    result[index * 2U + 1U] = kHexDigits[bytes[index] & 0x0fU];
  }
  return result;
}

bool Hash256::from_hex(std::string_view text, Hash256& output) noexcept {
  output = {};
  if (text.size() != output.bytes.size() * 2U) {
    return false;
  }
  for (std::size_t index = 0U; index < output.bytes.size(); ++index) {
    const int high = hex_value(text[index * 2U]);
    const int low  = hex_value(text[index * 2U + 1U]);
    if (high < 0 || low < 0) {
      output = {};
      return false;
    }
    output.bytes[index] = static_cast<std::uint8_t>((high << 4U) | low);
  }
  return true;
}

Hash256 sha256(std::span<const std::uint8_t> bytes) noexcept {
  Hash256 result;
  static constexpr std::array<std::uint8_t, 1U> kEmptyInput{};
  const std::uint8_t* input = bytes.empty() ? kEmptyInput.data() : bytes.data();
  if (mbedtls_sha256(input, bytes.size(), result.bytes.data(), 0) != 0) {
    result = {};
  }
  return result;
}

Hash256 sha256(std::string_view text) noexcept {
  return sha256(std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

}  // namespace puc::hashing
