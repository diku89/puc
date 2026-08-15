#pragma once

/**
 * @file sha256.hpp
 * @brief Project-wide SHA-256 value and digest API.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace puc::hashing {

/** Byte width of one SHA-256 digest. */
inline constexpr std::size_t kSha256Bytes = 32U;

/** Value-semantic SHA-256 digest with canonical hexadecimal conversion. */
struct Hash256 {
  std::array<std::uint8_t, kSha256Bytes> bytes{}; /**< Big-endian digest. */

  /** Compare two digests for exact byte equality. */
  constexpr bool operator==(const Hash256&) const noexcept = default;
  /** Order two digests lexicographically by bytes. */
  constexpr auto operator<=>(const Hash256&) const noexcept = default;

  /** Return whether every digest byte is zero. */
  bool empty() const noexcept;

  /** Render 64 lowercase hexadecimal digits. */
  std::string hex() const;

  /** Parse exactly 64 hexadecimal digits into `output`. */
  static bool from_hex(std::string_view text, Hash256& output) noexcept;
};

/** Hash an arbitrary byte span with the repository's standard provider. */
Hash256 sha256(std::span<const std::uint8_t> bytes) noexcept;

/** Hash the exact bytes of a string view with the standard provider. */
Hash256 sha256(std::string_view text) noexcept;

}  // namespace puc::hashing
