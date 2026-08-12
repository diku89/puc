/**
 * @file zbuf.cpp
 * @brief ZBuffer ownership, lookup, removal, and reordering implementation.
 */

#include "puc-cli/tui/rendering/zbuf.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI ZBuffer");
/** @endcond */

namespace puc {
namespace tui {

Status ZBuffer::add(std::string frame_id, std::shared_ptr<Frame> frame) {
  if (frame_id.empty() || !frame) {
    Logger<ERROR> << "Cannot add frame to Z-buffer: "
                  << status_message(Status::INVALID_ARGUMENT);
    return Status::INVALID_ARGUMENT;
  }

  const auto existing = std::find_if(
      frames_.begin(), frames_.end(),
      [&](const Entry& entry) { return entry.frame_id == frame_id; });
  if (existing != frames_.end()) {
    Logger<ERROR> << "Cannot add duplicate frame '" << frame_id
                  << "': " << status_message(Status::DUPLICATE_FRAME_ID);
    return Status::DUPLICATE_FRAME_ID;
  }

  Logger<DEBUG> << "Added frame '" << frame_id << "' at Z-index "
                << frames_.size();
  frames_.push_back(Entry{
      .frame_id = std::move(frame_id),
      .frame    = std::move(frame),
  });
  ++revision_;
  return Status::OK;
}

Status ZBuffer::remove(std::string_view frame_id) {
  const auto found = std::find_if(
      frames_.begin(), frames_.end(),
      [&](const Entry& entry) { return entry.frame_id == frame_id; });
  if (found == frames_.end()) {
    Logger<WARN> << "Cannot remove unknown frame '" << frame_id << "'";
    return Status::FRAME_NOT_FOUND;
  }

  Logger<DEBUG> << "Removed frame '" << frame_id << "' from Z-buffer";
  frames_.erase(found);
  ++revision_;
  return Status::OK;
}

Status ZBuffer::bring_to_front(std::string_view frame_id) {
  const auto found = std::find_if(
      frames_.begin(), frames_.end(),
      [&](const Entry& entry) { return entry.frame_id == frame_id; });
  if (found == frames_.end()) {
    Logger<WARN> << "Cannot move unknown frame '" << frame_id << "'";
    return Status::FRAME_NOT_FOUND;
  }
  if (std::next(found) == frames_.end()) {
    return Status::OK;
  }

  const std::string moved_frame_id{frame_id};
  Entry entry = std::move(*found);
  frames_.erase(found);
  frames_.push_back(std::move(entry));
  ++revision_;
  Logger<DEBUG> << "Moved frame '" << moved_frame_id << "' to front";
  return Status::OK;
}

const std::vector<ZBuffer::Entry>& ZBuffer::frames() const noexcept {
  return frames_;
}

}  // namespace tui
}  // namespace puc
