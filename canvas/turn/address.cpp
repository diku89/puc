/** @file address.cpp @brief Hierarchical Turn-address parsing and ordering. */

#include "canvas/turn/address.hpp"

#include <algorithm>
#include <charconv>
#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace puc::canvas {
namespace {

std::optional<AddressComponent> parse_component(std::string_view text) {
  if (text.empty()) return std::nullopt;
  if (text.front() >= '0' && text.front() <= '9') {
    if (text.front() == '0') return std::nullopt;
    std::uint64_t value = 0U;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value == 0U) {
      return std::nullopt;
    }
    return AddressComponent{.kind    = AddressComponent::Kind::NUMERIC,
                            .ordinal = value};
  }

  std::uint64_t value = 0U;
  for (const char character : text) {
    if (character < 'a' || character > 'z') return std::nullopt;
    const std::uint64_t digit =
        static_cast<std::uint64_t>(character - 'a') + 1U;
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 26U) {
      return std::nullopt;
    }
    value = value * 26U + digit;
  }
  return AddressComponent{.kind    = AddressComponent::Kind::ALPHABETIC,
                          .ordinal = value};
}

std::string alphabetic(std::uint64_t ordinal) {
  std::string result;
  while (ordinal > 0U) {
    --ordinal;
    result.push_back(static_cast<char>('a' + ordinal % 26U));
    ordinal /= 26U;
  }
  std::reverse(result.begin(), result.end());
  return result;
}

}  // namespace

std::optional<TurnAddress> TurnAddress::parse(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::vector<AddressComponent> components;
  std::size_t begin = 0U;
  while (begin < text.size()) {
    const std::size_t separator = text.find('.', begin);
    const std::size_t end =
        separator == std::string_view::npos ? text.size() : separator;
    const auto component = parse_component(text.substr(begin, end - begin));
    if (!component.has_value()) return std::nullopt;
    components.push_back(*component);
    if (separator == std::string_view::npos) break;
    begin = separator + 1U;
  }
  return TurnAddress{std::move(components)};
}

TurnAddress TurnAddress::root(std::uint64_t ordinal) {
  return TurnAddress{{AddressComponent{.kind = AddressComponent::Kind::NUMERIC,
                                       .ordinal = ordinal}}};
}

TurnAddress TurnAddress::numeric_child(std::uint64_t ordinal) const {
  std::vector<AddressComponent> result = components_;
  result.push_back(AddressComponent{.kind    = AddressComponent::Kind::NUMERIC,
                                    .ordinal = ordinal});
  return TurnAddress{std::move(result)};
}

TurnAddress TurnAddress::alphabetic_child(std::uint64_t ordinal) const {
  std::vector<AddressComponent> result = components_;
  result.push_back(AddressComponent{.kind = AddressComponent::Kind::ALPHABETIC,
                                    .ordinal = ordinal});
  return TurnAddress{std::move(result)};
}

std::string TurnAddress::string() const {
  std::string result;
  for (const AddressComponent& component : components_) {
    if (!result.empty()) result.push_back('.');
    result.append(component.kind == AddressComponent::Kind::NUMERIC
                      ? std::to_string(component.ordinal)
                      : alphabetic(component.ordinal));
  }
  return result;
}

bool TurnAddress::is_prefix_of(const TurnAddress& other) const noexcept {
  return components_.size() < other.components_.size() &&
         std::equal(components_.begin(), components_.end(),
                    other.components_.begin());
}

std::strong_ordering TurnAddress::operator<=>(
    const TurnAddress& other) const noexcept {
  return std::lexicographical_compare_three_way(
      components_.begin(), components_.end(), other.components_.begin(),
      other.components_.end());
}

}  // namespace puc::canvas
