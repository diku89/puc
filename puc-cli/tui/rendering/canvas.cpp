/**
 * @file canvas.cpp
 * @brief Canvas transaction, bounds-validation, and buffer-swap implementation.
 */

#include "puc-cli/tui/rendering/canvas.hpp"

#include <algorithm>
#include <limits>

#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Canvas");
/** @endcond */

namespace puc {
namespace tui {
namespace {

/**
 * Multiply canvas dimensions without wrapping `size_t`.
 *
 * @param[in] width Number of columns.
 * @param[in] height Number of rows.
 * @param[out] area Receives the product, or zero on overflow.
 * @return Status::OK or Status::DIMENSION_OVERFLOW.
 */
Status checked_area(size_t width, size_t height, size_t& area) noexcept {
  if (height != 0 && width > std::numeric_limits<size_t>::max() / height) {
    area = 0;
    return Status::DIMENSION_OVERFLOW;
  }

  area = width * height;
  return Status::OK;
}

}  // namespace

Canvas::Canvas(size_t width, size_t height) : width_(width), height_(height) {
  size_t area = 0;
  status_     = checked_area(width, height, area);
  if (!is_ok(status_)) {
    Logger<ERROR> << "Could not create " << width << 'x' << height
                  << " canvas: " << status_message(status_);
    width_  = 0;
    height_ = 0;
    return;
  }

  screen_buffer_a_.resize(area);
  screen_buffer_b_.resize(area);
  Logger<DEBUG> << "Created " << width_ << 'x' << height_ << " canvas";
}

std::pair<size_t, size_t> Canvas::get_dimensions() const noexcept {
  return {width_, height_};
}

Status Canvas::get_status() const noexcept { return status_; }

Status Canvas::begin_frame() noexcept {
  if (!is_ok(status_)) {
    Logger<ERROR> << "Cannot begin canvas frame: " << status_message(status_);
    return status_;
  }
  if (frame_in_progress_) {
    Logger<ERROR> << status_message(Status::FRAME_ALREADY_IN_PROGRESS);
    return Status::FRAME_ALREADY_IN_PROGRESS;
  }

  copy_drawable_to_writable();
  frame_in_progress_ = true;
  return Status::OK;
}

Status Canvas::clear(const Cell& cell) noexcept {
  if (!frame_in_progress_) {
    Logger<ERROR> << "Cannot clear canvas: "
                  << status_message(Status::NO_FRAME_IN_PROGRESS);
    return Status::NO_FRAME_IN_PROGRESS;
  }

  std::vector<Cell>& writable = writable_buffer();
  std::fill(writable.begin(), writable.end(), cell);
  return Status::OK;
}

Status Canvas::write_cells(const Rect& rect,
                           const std::span<std::span<Cell>>& cells) noexcept {
  if (!frame_in_progress_) {
    Logger<ERROR> << "Cannot write cells: "
                  << status_message(Status::NO_FRAME_IN_PROGRESS);
    return Status::NO_FRAME_IN_PROGRESS;
  }
  if (rect.x > width_ || rect.y > height_ || rect.width > width_ - rect.x ||
      rect.height > height_ - rect.y) {
    Logger<ERROR> << "Cannot write rectangle " << rect.x << ',' << rect.y << ' '
                  << rect.width << 'x' << rect.height << " to " << width_ << 'x'
                  << height_
                  << " canvas: " << status_message(Status::RECT_OUT_OF_BOUNDS);
    return Status::RECT_OUT_OF_BOUNDS;
  }
  if (cells.size() != rect.height) {
    Logger<ERROR> << "Expected " << rect.height << " cell rows, received "
                  << cells.size();
    return Status::CELL_SHAPE_MISMATCH;
  }

  for (const auto row : cells) {
    if (row.size() != rect.width) {
      Logger<ERROR> << "Expected cell rows of width " << rect.width
                    << ", received " << row.size();
      return Status::CELL_SHAPE_MISMATCH;
    }
  }

  std::vector<Cell>& writable = writable_buffer();
  for (size_t row = 0; row < rect.height; ++row) {
    const size_t destination = (rect.y + row) * width_ + rect.x;
    std::copy(cells[row].begin(), cells[row].end(),
              writable.begin() + static_cast<std::ptrdiff_t>(destination));
  }
  return Status::OK;
}

Status Canvas::write_cells(const Rect& rect,
                           std::vector<std::vector<Cell>>& cells) {
  std::vector<std::span<Cell>> rows;
  rows.reserve(cells.size());
  for (std::vector<Cell>& row : cells) {
    rows.emplace_back(row);
  }
  return write_cells(rect, std::span<std::span<Cell>>{rows});
}

Status Canvas::end_frame() noexcept {
  if (!frame_in_progress_) {
    Logger<ERROR> << status_message(Status::NO_FRAME_IN_PROGRESS);
    return Status::NO_FRAME_IN_PROGRESS;
  }

  buffer_state_      = buffer_state_ == BufferState::DRAW_A_WRITETO_B
                           ? BufferState::DRAW_B_WRITETO_A
                           : BufferState::DRAW_A_WRITETO_B;
  frame_in_progress_ = false;
  return Status::OK;
}

Status Canvas::cancel_frame() noexcept {
  if (!frame_in_progress_) {
    Logger<ERROR> << status_message(Status::NO_FRAME_IN_PROGRESS);
    return Status::NO_FRAME_IN_PROGRESS;
  }
  frame_in_progress_ = false;
  return Status::OK;
}

std::span<const Canvas::Cell> Canvas::get_drawable_buffer() const noexcept {
  return buffer_state_ == BufferState::DRAW_A_WRITETO_B
             ? std::span<const Cell>{screen_buffer_a_}
             : std::span<const Cell>{screen_buffer_b_};
}

void Canvas::copy_drawable_to_writable() noexcept {
  if (buffer_state_ == BufferState::DRAW_A_WRITETO_B) {
    std::copy(screen_buffer_a_.begin(), screen_buffer_a_.end(),
              screen_buffer_b_.begin());
    return;
  }

  std::copy(screen_buffer_b_.begin(), screen_buffer_b_.end(),
            screen_buffer_a_.begin());
}

std::vector<Canvas::Cell>& Canvas::writable_buffer() noexcept {
  return buffer_state_ == BufferState::DRAW_A_WRITETO_B ? screen_buffer_b_
                                                        : screen_buffer_a_;
}

}  // namespace tui
}  // namespace puc
