#pragma once

/**
 * @file address.hpp
 * @brief Canonical hierarchical human Turn addresses.
 */

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace puc::canvas {

/** One numeric or lowercase alphabetic address component. */
struct AddressComponent {
  /** Select numeric sibling numbering or alphabetic response-part numbering. */
  enum class Kind : std::uint8_t {
    NUMERIC,    /**< Positive decimal component such as `2`. */
    ALPHABETIC, /**< Positive base-26 component such as `b`. */
  };

  Kind kind = Kind::NUMERIC;  /**< Component namespace used for rendering. */
  std::uint64_t ordinal = 0U; /**< One-based semantic component value. */

  /** Compare two components for exact semantic equality. */
  constexpr bool operator==(const AddressComponent&) const noexcept = default;
  /** Order numeric before alphabetic components, then compare ordinals. */
  constexpr std::strong_ordering operator<=>(
      const AddressComponent& other) const noexcept {
    if (kind != other.kind) {
      return kind == Kind::NUMERIC ? std::strong_ordering::less
                                   : std::strong_ordering::greater;
    }
    return ordinal <=> other.ordinal;
  }
};

/** Parsed address whose semantic ordering is presentation ordering. */
class TurnAddress final {
 public:
  /** Construct the empty address used only before parsing or assignment. */
  TurnAddress() = default;

  /** Parse one canonical address; zero and leading-zero numerics are invalid.
   */
  static std::optional<TurnAddress> parse(std::string_view text);

  /** Construct one numeric root address. */
  static TurnAddress root(std::uint64_t ordinal);

  /** Return this address with one numeric child appended. */
  TurnAddress numeric_child(std::uint64_t ordinal) const;

  /** Return this address with one alphabetic child appended. */
  TurnAddress alphabetic_child(std::uint64_t ordinal) const;

  /** Render the unique canonical human representation. */
  std::string string() const;

  /** Return immutable parsed components from root to leaf. */
  const std::vector<AddressComponent>& components() const noexcept {
    return components_;
  }

  /** Return whether this address has no components. */
  bool empty() const noexcept { return components_.empty(); }

  /** Return whether this address is an ancestor of `other`, including itself.
   */
  bool is_prefix_of(const TurnAddress& other) const noexcept;

  /** Compare exact semantic address identity. */
  bool operator==(const TurnAddress&) const noexcept = default;
  /** Compare component-wise in stable presentation order. */
  std::strong_ordering operator<=>(const TurnAddress& other) const noexcept;

 private:
  /** Construct an address from already validated root-to-leaf components. */
  explicit TurnAddress(std::vector<AddressComponent> components)
      : components_(std::move(components)) {}

  std::vector<AddressComponent> components_; /**< Root-to-leaf components. */
};

}  // namespace puc::canvas
