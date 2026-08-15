#pragma once

/**
 * @file settings.hpp
 * @brief Property-backed Canvas settings.
 */

#include <filesystem>

namespace puc::properties {
class Properties;
}

namespace puc::canvas {

/** Fully resolved configuration required by Canvas initialization. */
struct Settings {
  std::filesystem::path database_path; /**< SQLite database file location. */
};

/** Load defaults and user overrides, then expand the configured home path. */
bool load_settings(properties::Properties& properties, Settings& settings);

}  // namespace puc::canvas
