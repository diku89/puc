#pragma once

/**
 * @file canvas.hpp
 * @brief Transactional, double-buffered storage for terminal character cells.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "puc-cli/tui/status.hpp"

namespace puc {
namespace tui {

/**
 * A rectangular, double-buffered grid of terminal cells.
 *
 * The drawable buffer remains stable while the next frame is assembled in a
 * separate writable buffer. `begin_frame()` initializes the writable buffer
 * from the currently published image, and `end_frame()` atomically selects it
 * as the next drawable image from the caller's perspective. This preserves
 * cells that a partial redraw does not touch.
 *
 * Transaction lifecycle operations and `clear()` belong to one coordinating
 * thread. During an active transaction, separate workers may call
 * `write_cells()` concurrently only for non-intersecting rectangles. The frame
 * execution graph establishes that invariant and completes every write before
 * cancellation, publication, or drawable-buffer access.
 */
class Canvas {
 public:
  /**
   * One terminal character cell and its complete rendering attributes.
   *
   * Colors use packed `0xRRGGBB` true-color values. `character` is one Unicode
   * scalar value; display width is interpreted by the terminal renderer.
   */
  struct Cell {
    char32_t character        = U' '; /**< Unicode character to be displayed. */
    uint32_t foreground_color = 0;    /**< Foreground color in RGB format. */
    uint32_t background_color = 0;    /**< Background color in RGB format. */
  };

  /**
   * A half-open rectangular region in canvas cell coordinates.
   *
   * The covered coordinates are `[x, x + width)` and `[y, y + height)`.
   */
  struct Rect {
    size_t x;      /**< X coordinate of the top-left corner of the rectangle. */
    size_t y;      /**< Y coordinate of the top-left corner of the rectangle. */
    size_t width;  /**< Width of the rectangle. */
    size_t height; /**< Height of the rectangle. */

    /** Compare origin and dimensions. */
    constexpr bool operator==(const Rect&) const noexcept = default;
  };

  /**
   * Construct a blank canvas with two equally sized buffers.
   *
   * Zero-sized dimensions are valid. If `width * height` cannot be represented
   * by `size_t`, construction records Status::DIMENSION_OVERFLOW and exposes an
   * empty `0 x 0` canvas; callers inspect that result with `get_status()`.
   *
   * @param[in] width Number of columns.
   * @param[in] height Number of rows.
   */
  Canvas(size_t width, size_t height);

  Canvas(const Canvas&)            = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas(Canvas&&)                 = delete;
  Canvas& operator=(Canvas&&)      = delete;

  /** Destroy both cell buffers. */
  ~Canvas() = default;

  /**
   * Return the canvas dimensions.
   *
   * @return `{width, height}` in terminal cells.
   */
  std::pair<size_t, size_t> get_dimensions() const noexcept;

  /**
   * Return the result of validating and allocating this canvas.
   *
   * @return Status::OK for a usable canvas, otherwise the construction error.
   */
  Status get_status() const noexcept;

  /**
   * Begin constructing a frame.
   *
   * This copies the currently drawable buffer into the writable buffer.
   * `clear()` and `write_cells()` may only be called between `begin_frame()`
   * and `end_frame()`. Frame transactions must not be nested.
   *
   * @return Status::OK on success, the canvas construction status if invalid,
   *         or Status::FRAME_ALREADY_IN_PROGRESS for a nested transaction.
   */
  Status begin_frame() noexcept;

  /**
   * Fill every cell in the frame under construction.
   *
   * @param[in] cell Cell value copied into the writable buffer.
   * @return Status::OK on success, or Status::NO_FRAME_IN_PROGRESS when called
   *         outside a frame transaction.
   */
  Status clear(const Cell& cell) noexcept;

  /**
   * Write cells to a rectangle on the canvas.
   *
   * The outer span must contain exactly `rect.height` rows and each inner span
   * must contain exactly `rect.width` cells. Validation completes before any
   * cells are copied, so a rejected write does not partially modify the frame.
   *
   * @param[in] rect Destination rectangle in canvas coordinates.
   * @param[in] cells Row-major source cells matching `rect` exactly.
   * @return Status::OK on success, Status::NO_FRAME_IN_PROGRESS outside a
   *         transaction, Status::RECT_OUT_OF_BOUNDS for an invalid rectangle,
   *         or Status::CELL_SHAPE_MISMATCH for incompatible source dimensions.
   */
  Status write_cells(const Rect& rect,
                     const std::span<std::span<Cell>>& cells) noexcept;

  /**
   * Publish the completed writable buffer.
   *
   * @return Status::OK on success, or Status::NO_FRAME_IN_PROGRESS when no
   *         frame transaction is active.
   */
  Status end_frame() noexcept;

  /**
   * Abandon the writable image without changing the published buffer.
   *
   * @return Status::OK on success, or Status::NO_FRAME_IN_PROGRESS when no
   *         transaction is active.
   */
  Status cancel_frame() noexcept;

  /**
   * Access the most recently published image in row-major order.
   *
   * A frame currently being assembled is not visible through this span until
   * `end_frame()` publishes it.
   *
   * @return A read-only span containing `width * height` cells.
   */
  std::span<const Cell> get_drawable_buffer() const noexcept;

 private:
  /** Selects which backing buffer is drawable and which is writable. */
  enum class BufferState {
    DRAW_A_WRITETO_B, /**< Buffer A is published; mutations target buffer B. */
    DRAW_B_WRITETO_A, /**< Buffer B is published; mutations target buffer A. */
  };

  /** Initialize the writable buffer with the currently published image. */
  void copy_drawable_to_writable() noexcept;

  /** Return the buffer currently receiving transaction writes. */
  std::vector<Cell>& writable_buffer() noexcept;

  /** First row-major cell buffer. */
  std::vector<Cell> screen_buffer_a_;
  /** Second row-major cell buffer. */
  std::vector<Cell> screen_buffer_b_;
  /** Current roles of the two buffers. */
  BufferState buffer_state_ = BufferState::DRAW_A_WRITETO_B;
  /** Whether a frame is currently being constructed. */
  bool frame_in_progress_ = false;

  /** Result of validating and allocating the canvas dimensions. */
  Status status_ = Status::OK;

  /** Width of the canvas. */
  size_t width_;
  /** Height of the canvas. */
  size_t height_;
};

}  // namespace tui
}  // namespace puc
