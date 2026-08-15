#pragma once

/**
 * @file channel_path.hpp
 * @brief Validated relative IPC channel paths and absolute-path resolution.
 */

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace puc::ipc {

/**
 * Canonical nonempty path relative to an absolute IPC channel namespace.
 *
 * Relative paths use the same segment grammar as channel names, but omit the
 * leading `//`. Keeping the two forms distinct prevents a caller from
 * accidentally replacing a namespace root with an absolute child path.
 */
class RelativeChannelPath final {
 public:
  /** Parse a canonical relative path such as `turns/committed`. */
  static std::optional<RelativeChannelPath> parse(std::string_view path);

  /** Return the canonical relative representation without a leading slash. */
  const std::string& string() const noexcept { return path_; }

  /**
   * Resolve this path beneath one canonical absolute channel namespace.
   *
   * The result is absent if `root` is invalid or the joined name exceeds the
   * IPC channel-name limit.
   */
  std::optional<std::string> resolve(std::string_view root) const;

  /** Compare canonical relative paths by exact bytes. */
  bool operator==(const RelativeChannelPath&) const noexcept = default;

 private:
  /** Construct from a path already accepted by parse(). */
  explicit RelativeChannelPath(std::string path) : path_(std::move(path)) {}

  std::string path_; /**< Canonical path without a leading slash. */
};

}  // namespace puc::ipc
