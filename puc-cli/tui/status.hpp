#pragma once

/**
 * @file status.hpp
 * @brief Non-throwing status codes shared by terminal UI components.
 */

#include <string_view>

namespace puc {

/**
 * Terminal user-interface primitives, layout, rendering, and terminal control.
 */
namespace tui {

/**
 * Result codes returned by expected TUI operations.
 *
 * Status::OK is the sole success value. All other values describe failures a
 * caller can log, propagate, or handle without exceptions.
 */
enum class Status {
  OK,                 /**< Operation completed successfully. */
  INVALID_ARGUMENT,   /**< Required object, id, or value is invalid. */
  INVALID_DIMENSIONS, /**< Dimensions are zero where positivity is required. */
  DIMENSION_OVERFLOW, /**< Dimension arithmetic exceeds addressable storage. */
  FRAME_ALREADY_IN_PROGRESS, /**< A Canvas transaction is already active. */
  NO_FRAME_IN_PROGRESS, /**< A Canvas mutation requires an active transaction.
                         */
  RECT_OUT_OF_BOUNDS,   /**< A rectangle extends outside its Canvas. */
  CELL_SHAPE_MISMATCH,  /**< Source cell rows do not match their rectangle. */
  DUPLICATE_FRAME_ID,   /**< A ZBuffer already contains the frame id. */
  FRAME_NOT_FOUND,      /**< A requested or referenced frame is absent. */
  INVALID_PERCENTAGE,   /**< A percentage is non-finite or outside `[0, 1]`. */
  INVALID_RATIO,        /**< An aspect-ratio dimension is not positive. */
  INVALID_CONSTRAINT, /**< A layout constraint is malformed or contradictory. */
  CONSTRAINT_CYCLE,   /**< Named frame anchors form a dependency cycle. */
  CANVAS_NOT_SET,     /**< Screen has no valid Canvas to present. */
  TERMINAL_NOT_AVAILABLE, /**< Configured descriptors are not usable terminals.
                           */
  TERMINAL_QUERY_FAILED,  /**< Terminal dimensions could not be queried. */
  TERMINAL_CONFIG_FAILED, /**< Terminal attributes could not be
                             changed/restored. */
  TERMINAL_WRITE_FAILED, /**< The complete terminal output could not be written.
                          */
  EVENT_BUFFER_FULL,     /**< A Screen event could not be queued. */
};

/**
 * Test whether a status represents success.
 *
 * @param[in] status Status value to inspect.
 * @return `true` only for Status::OK.
 */
constexpr bool is_ok(Status status) noexcept { return status == Status::OK; }

/**
 * Return a stable, human-readable description of a status code.
 *
 * Unknown enum values map to `"unknown terminal UI status"` so diagnostic
 * paths never need a second failure channel.
 *
 * @param[in] status Status value to describe.
 * @return A static string suitable for logs and diagnostics.
 */
constexpr std::string_view status_message(Status status) noexcept {
  switch (status) {
    case Status::OK:
      return "success";
    case Status::INVALID_ARGUMENT:
      return "invalid argument";
    case Status::INVALID_DIMENSIONS:
      return "invalid dimensions";
    case Status::DIMENSION_OVERFLOW:
      return "dimensions overflow addressable storage";
    case Status::FRAME_ALREADY_IN_PROGRESS:
      return "a canvas frame is already in progress";
    case Status::NO_FRAME_IN_PROGRESS:
      return "no canvas frame is in progress";
    case Status::RECT_OUT_OF_BOUNDS:
      return "rectangle is outside the canvas";
    case Status::CELL_SHAPE_MISMATCH:
      return "cell dimensions do not match the rectangle";
    case Status::DUPLICATE_FRAME_ID:
      return "frame id already exists";
    case Status::FRAME_NOT_FOUND:
      return "frame id was not found";
    case Status::INVALID_PERCENTAGE:
      return "percentage must be between zero and one";
    case Status::INVALID_RATIO:
      return "ratio width and height must be positive";
    case Status::INVALID_CONSTRAINT:
      return "constraint is invalid or contradictory";
    case Status::CONSTRAINT_CYCLE:
      return "named constraints contain a cycle";
    case Status::CANVAS_NOT_SET:
      return "screen has no canvas";
    case Status::TERMINAL_NOT_AVAILABLE:
      return "terminal is not available";
    case Status::TERMINAL_QUERY_FAILED:
      return "terminal dimensions could not be queried";
    case Status::TERMINAL_CONFIG_FAILED:
      return "terminal mode could not be configured";
    case Status::TERMINAL_WRITE_FAILED:
      return "terminal output could not be written";
    case Status::EVENT_BUFFER_FULL:
      return "event buffer is full";
  }
  return "unknown terminal UI status";
}

}  // namespace tui
}  // namespace puc
