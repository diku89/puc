#pragma once

/**
 * @file settings.hpp
 * @brief Property-backed IPC resource limits.
 */

#include <cstddef>

namespace puc::properties {
class Properties;
}

namespace puc::ipc {

/** Runtime IPC settings loaded once during application initialization. */
struct Settings {
  std::size_t maximum_message_bytes =
      0U; /**< Largest complete payload accepted by general IPC channels. */
};

/** Load and validate the system default and optional user override. */
bool load_settings(properties::Properties& properties, Settings& settings);

}  // namespace puc::ipc
