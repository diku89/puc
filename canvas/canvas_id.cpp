/** @file canvas_id.cpp @brief Unix random Canvas UUID generation. */

#include "canvas/canvas_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>

namespace puc::canvas {

CanvasUuid random_canvas_uuid() {
  CanvasUuid uuid{};
  std::ifstream random{"/dev/urandom", std::ios::binary};
  random.read(reinterpret_cast<char*>(uuid.data()),
              static_cast<std::streamsize>(uuid.size()));
  if (!random) {
    std::random_device fallback;
    for (std::uint8_t& byte : uuid) {
      byte = static_cast<std::uint8_t>(fallback());
    }
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fU) | 0x40U);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fU) | 0x80U);
  return uuid;
}

std::string canvas_uuid_hex(const CanvasUuid& uuid) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result(uuid.size() * 2U, '0');
  for (std::size_t index = 0U; index < uuid.size(); ++index) {
    result[index * 2U]      = digits[uuid[index] >> 4U];
    result[index * 2U + 1U] = digits[uuid[index] & 0x0fU];
  }
  return result;
}

}  // namespace puc::canvas
