#pragma once

/**
 * @file zbuf.hpp
 * @brief Owning, ordered frame container used to define compositing order.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/rendering/status.hpp"

namespace puc {
namespace tui {

/**
 * A shared-ownership container that defines frame compositing order.
 *
 * Frames are stored in drawing order: the first frame is the backmost and the
 * last frame is the frontmost. Frame identifiers are unique within one
 * ZBuffer and are also used as keys for Layout constraints.
 */
class ZBuffer {
 public:
  /** One frame together with its stable layout identifier. */
  struct Entry {
    std::string frame_id; /**< Unique identifier within the Z-buffer. */
    std::shared_ptr<Frame>
        frame; /**< Shared ownership of the implementation. */
  };

  /** Construct an empty Z-buffer. */
  ZBuffer() = default;

  /** Release this container's ownership of every frame. */
  ~ZBuffer() = default;

  /**
   * Add a frame in front of all existing frames.
   *
   * @param[in] frame_id The unique id of the frame to add.
   * @param[in] frame Frame implementation for which ownership is shared.
   * @return Status::OK on success, Status::INVALID_ARGUMENT for an empty id or
   *         null frame, or Status::DUPLICATE_FRAME_ID when the id exists.
   */
  Status add(std::string frame_id, std::shared_ptr<Frame> frame);

  /**
   * Remove a frame.
   *
   * @param[in] frame_id The id of the frame to remove.
   * @return Status::OK on success, or Status::FRAME_NOT_FOUND.
   */
  Status remove(std::string_view frame_id);

  /**
   * Move a frame in front of all other frames.
   *
   * @param[in] frame_id The id of the frame to move.
   * @return Status::OK on success, including when it is already frontmost, or
   *         Status::FRAME_NOT_FOUND when the id is unknown.
   */
  Status bring_to_front(std::string_view frame_id);

  /**
   * Access entries in back-to-front drawing order.
   *
   * @return A read-only reference valid until this ZBuffer is mutated.
   */
  const std::vector<Entry>& frames() const noexcept;

  /** Return a generation incremented by every structural/order mutation. */
  std::size_t revision() const noexcept { return revision_; }

 private:
  /** Entries ordered from backmost to frontmost. */
  std::vector<Entry> frames_;
  /** Generation used to invalidate derived layout and rendering caches. */
  std::size_t revision_ = 0U;
};

}  // namespace tui
}  // namespace puc
