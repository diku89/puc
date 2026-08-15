#pragma once

/**
 * @file canvas_id.hpp
 * @brief RFC 4122 version-4 Canvas UUID generation.
 */

#include <array>
#include <cstdint>
#include <string>

namespace puc::canvas {

/** Raw 128-bit representation shared by every Canvas-owned UUID. */
using CanvasUuid = std::array<std::uint8_t, 16U>;

/** Generate one RFC 4122 version-4 UUID without a collision lookup. */
CanvasUuid random_canvas_uuid();

/** Render a UUID as 32 lowercase hexadecimal digits without separators. */
std::string canvas_uuid_hex(const CanvasUuid& uuid);

}  // namespace puc::canvas
