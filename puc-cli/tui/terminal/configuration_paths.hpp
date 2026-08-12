#pragma once

/**
 * @file configuration_paths.hpp
 * @brief Installed and Bazel-runfiles configuration-root discovery.
 */

#include <filesystem>
#include <string>
#include <string_view>

namespace puc::terminal {

/** Primary packaged defaults and optional user-overlay roots. */
struct ConfigurationRoots {
  std::filesystem::path primary;
  std::filesystem::path user_overrides;
};

/** Copy one environment variable, or return an empty string when absent. */
std::string environment_value(const char* name);

/**
 * Locate packaged terminal/theme configuration and its user overlay.
 *
 * `PUC_CONFIG_ROOT` takes precedence. Otherwise the current directory and
 * common Bazel runfiles roots derived from `RUNFILES_DIR` and the executable
 * path are searched. `PUC_USER_CONFIG_ROOT` selects the overlay; without it a
 * deliberately absent path below the primary root disables overrides.
 */
ConfigurationRoots discover_configuration_roots(
    std::string_view executable = {});

}  // namespace puc::terminal
