/** @file settings.cpp @brief IPC TOML loading and validation. */

#include "utils/ipc/settings.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <variant>

#include "properties/properties.hpp"

namespace puc::ipc {

bool load_settings(properties::Properties& properties, Settings& settings) {
  settings                           = {};
  constexpr std::string_view kSource = "ipc";
  constexpr std::string_view kPath   = "ipc.toml";
  const properties::Status loaded    = properties.load_mutable_defaults(
      std::string{kSource}, std::filesystem::path{kPath});
  if (!properties::is_ok(loaded)) {
    return false;
  }

  properties::Property property;
  if (!properties::is_ok(
          properties.get("ipc.channel.maximum_message_bytes", property))) {
    return false;
  }
  const auto* configured = std::get_if<std::int64_t>(&property.value);
  constexpr std::uint64_t kMaximumFramedPayload =
      std::numeric_limits<std::uint32_t>::max();
  if (configured == nullptr || *configured <= 0 ||
      static_cast<std::uint64_t>(*configured) > kMaximumFramedPayload ||
      static_cast<std::uint64_t>(*configured) >
          std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  settings.maximum_message_bytes = static_cast<std::size_t>(*configured);
  return true;
}

}  // namespace puc::ipc
