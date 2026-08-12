/**
 * @file configuration_paths.cpp
 * @brief Installed and Bazel-runfiles configuration-root discovery.
 */

#include "puc-cli/tui/terminal/configuration_paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "puc-cli/tui/terminal/input.hpp"
#include "puc-cli/tui/terminal/timeouts.hpp"

namespace puc::terminal {
namespace {

bool regular_file(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

bool contains_packaged_configuration(const std::filesystem::path& root) {
  const std::string_view operating_system_defaults =
      operating_system_defaults_path(current_operating_system());
  return regular_file(root / "input_keys.toml") &&
         !operating_system_defaults.empty() &&
         regular_file(root / operating_system_defaults) &&
         regular_file(root / kTimeoutConfigurationPath) &&
         regular_file(root / "themes/default-dark.toml");
}

}  // namespace

std::string environment_value(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

ConfigurationRoots discover_configuration_roots(std::string_view executable) {
  std::filesystem::path primary;
  const std::string configured = environment_value("PUC_CONFIG_ROOT");
  if (!configured.empty()) {
    primary = configured;
  } else {
    std::vector<std::filesystem::path> candidates;
    std::error_code error;
    candidates.push_back(std::filesystem::current_path(error));
    const std::string runfiles = environment_value("RUNFILES_DIR");
    if (!runfiles.empty()) {
      candidates.emplace_back(runfiles);
      candidates.emplace_back(std::filesystem::path{runfiles} / "_main");
      candidates.emplace_back(std::filesystem::path{runfiles} / "puc");
    }
    if (!executable.empty()) {
      const std::filesystem::path executable_path{executable};
      candidates.emplace_back(executable_path.string() + ".runfiles/_main");
      candidates.emplace_back(executable_path.string() + ".runfiles/puc");
    }
    for (const std::filesystem::path& candidate : candidates) {
      if (contains_packaged_configuration(candidate)) {
        primary = candidate;
        break;
      }
    }
    if (primary.empty() && !candidates.empty()) {
      primary = candidates.front();
    }
  }

  const std::string user = environment_value("PUC_USER_CONFIG_ROOT");
  return ConfigurationRoots{
      .primary        = primary,
      .user_overrides = user.empty() ? primary / ".puc-no-user-overrides"
                                     : std::filesystem::path{user},
  };
}

}  // namespace puc::terminal
