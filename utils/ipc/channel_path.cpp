/** @file channel_path.cpp @brief Relative IPC channel-path implementation. */

#include "utils/ipc/channel_path.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "utils/ipc/channel.hpp"

namespace puc::ipc {

std::optional<RelativeChannelPath> RelativeChannelPath::parse(
    std::string_view path) {
  if (path.empty() || path.starts_with('/') || path.ends_with('/')) {
    return std::nullopt;
  }
  std::string absolute{"//"};
  absolute.append(path);
  if (!valid_channel_name(absolute)) {
    return std::nullopt;
  }
  return RelativeChannelPath{std::string{path}};
}

std::optional<std::string> RelativeChannelPath::resolve(
    std::string_view root) const {
  if (!valid_channel_name(root)) {
    return std::nullopt;
  }
  std::string resolved{root};
  resolved.push_back('/');
  resolved.append(path_);
  if (!valid_channel_name(resolved)) {
    return std::nullopt;
  }
  return resolved;
}

}  // namespace puc::ipc
