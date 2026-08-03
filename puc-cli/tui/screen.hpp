#pragma once

/**
 * @file screen.hpp
 * @brief POSIX terminal ownership, presentation, and event buffering.
 */

#include <termios.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "puc-cli/tui/canvas.hpp"
#include "puc-cli/tui/state.hpp"
#include "puc-cli/tui/status.hpp"

namespace puc {
namespace tui {

/**
 * Owns a POSIX terminal session and presents published Canvas images.
 *
 * `take()` switches the configured terminal to a raw alternate-screen session,
 * hides the cursor, and disables automatic line wrapping while preserving
 * signal-generating input such as Ctrl-C. `release()` restores both terminal
 * attributes and presentation state and is safe to call repeatedly.
 *
 * Screen renders the complete published Canvas as UTF-8 and 24-bit ANSI color.
 * It also detects terminal cell-count changes during `draw()` and queues resize
 * events in a bounded, mutex-protected FIFO.
 */
class Screen {
 public:
  /**
   * Kinds of input or terminal events represented by Event.
   */
  enum class EventType {
    RESIZE,    /**< Screen resize event. */
    KEY_PRESS, /**< Key press event. */
    SCROLL,    /**< Scroll event. */
  };

  /**
   * One timestamped event delivered through EventBuffer.
   *
   * `timestamp` is a monotonic-clock nanosecond count suitable for ordering,
   * not a wall-clock timestamp. Screen-generated events receive monotonically
   * increasing ids for the lifetime of the Screen.
   */
  struct Event {
    uint64_t timestamp = 0; /**< Monotonic timestamp in nanoseconds. */
    uint64_t id        = 0; /**< Event sequence identifier. */
    EventType type     = EventType::RESIZE; /**< Active member of `data`. */

    /** Payload selected by Event::type. */
    union Data {
      uint32_t key; /**< Decoded key value for KEY_PRESS. */
      struct {
        int32_t x; /**< Horizontal scroll delta. */
        int32_t y; /**< Vertical scroll delta. */
      } scroll;    /**< Payload for SCROLL. */
      struct {
        size_t width;  /**< New terminal width in columns. */
        size_t height; /**< New terminal height in rows. */
      } resize;        /**< Payload for RESIZE. */

      /** Value-initialize the union through the key member. */
      constexpr Data() : key(0) {}
    } data; /**< Event-specific payload. */
  };

  /**
   * Fixed-capacity, thread-safe FIFO shared by event producers and Screen.
   *
   * All `kMaxEvents` array slots are usable. `read_index` and `write_index`
   * wrap modulo capacity while `size` distinguishes empty from full.
   */
  struct EventBuffer {
    static constexpr size_t kMaxEvents = 1024; /**< Queue capacity. */
    std::mutex lock;          /**< Serializes every queue operation. */
    Event events[kMaxEvents]; /**< Array of events in the buffer. */
    size_t read_index  = 0;   /**< Read index for the event buffer. */
    size_t write_index = 0;   /**< Write index for the event buffer. */
    size_t size        = 0;   /**< Number of queued events. */
  };

  /**
   * Construct a Screen for standard input and standard output.
   *
   * @param[in] buffer The event buffer to use for screen events.
   */
  explicit Screen(std::shared_ptr<EventBuffer> buffer);

  /**
   * Construct a Screen using explicit terminal file descriptors.
   *
   * This overload supports tests and applications whose controlling terminal
   * is not connected to standard streams. The descriptors remain owned by the
   * caller and must outlive any operation that uses them.
   *
   * @param[in] buffer Event queue used by `pop_event()` and `draw()`.
   * @param[in] input_fd Descriptor used for terminal input and attributes.
   * @param[in] output_fd Descriptor used for terminal queries and output.
   */
  Screen(std::shared_ptr<EventBuffer> buffer, int input_fd, int output_fd);

  /** Release the terminal if this object currently owns it. */
  ~Screen();

  /**
   * Enter the alternate screen and configure raw terminal input.
   *
   * Calling `take()` again while already taken only replaces the event buffer.
   * If terminal output setup fails after attributes change, the original
   * attributes are restored before the error is returned.
   *
   * @param[in] buffer The event buffer to use for screen events.
   * @return Status::OK on success; Status::INVALID_ARGUMENT,
   *         Status::TERMINAL_NOT_AVAILABLE, Status::TERMINAL_QUERY_FAILED,
   *         Status::INVALID_DIMENSIONS, Status::TERMINAL_CONFIG_FAILED, or
   *         Status::TERMINAL_WRITE_FAILED otherwise.
   */
  Status take(std::shared_ptr<EventBuffer> buffer) noexcept;

  /**
   * Restore the terminal attributes and leave the alternate screen.
   *
   * @return Status::OK when released or already idle; otherwise the first
   *         output or attribute-restoration failure.
   */
  Status release() noexcept;

  /**
   * Query terminal dimensions in character cells.
   *
   * @param[out] width Receives the width in terminal cells.
   * @param[out] height Receives the height in terminal cells.
   * On error, `width` and `height` are set to zero.
   *
   * @return Status::OK, Status::TERMINAL_QUERY_FAILED, or
   *         Status::INVALID_DIMENSIONS.
   */
  Status get_dimensions(size_t& width, size_t& height) const noexcept;

  /**
   * Query terminal dimensions and relative physical cell proportions.
   *
   * When the terminal reports total pixel dimensions, this method reduces the
   * average cell width-to-height ratio to its smallest integer terms. Terminals
   * that omit pixel dimensions use the conventional `{1, 2}` fallback.
   *
   * @param[out] width Receives the width in terminal cells.
   * @param[out] height Receives the height in terminal cells.
   * @param[out] cell_dimensions Receives relative cell width and height.
   * @return Status::OK, Status::TERMINAL_QUERY_FAILED, or
   *         Status::INVALID_DIMENSIONS.
   */
  Status get_dimensions(size_t& width, size_t& height,
                        CellDimensions& cell_dimensions) const noexcept;

  /**
   * Select the Canvas presented by `draw()`.
   *
   * The canvas may be replaced when the terminal is resized.
   *
   * @param[in] canvas Valid canvas for which ownership is shared.
   * @return Status::OK on success, Status::CANVAS_NOT_SET for null, or the
   *         construction status recorded by an invalid Canvas.
   */
  Status set_canvas(std::shared_ptr<Canvas> canvas) noexcept;

  /**
   * Append an event to a shared buffer without blocking on queue capacity.
   *
   * @param[in] buffer The event buffer to push the event to.
   * @param[in] event The event to be pushed to the buffer.
   * @return Status::OK, Status::INVALID_ARGUMENT for a null buffer, or
   *         Status::EVENT_BUFFER_FULL when every slot is occupied.
   */
  static Status push_event(const std::shared_ptr<EventBuffer>& buffer,
                           const Event& event) noexcept;

  /**
   * Pop an event from the event buffer.
   *
   * @return The oldest queued event, or `std::nullopt` when the queue is empty
   *         or this Screen has no event buffer.
   */
  std::optional<Event> pop_event() noexcept;

  /**
   * Get the number of pending events in the event buffer.
   *
   * @return Number of queued events, or zero when no buffer is configured.
   */
  size_t pending_events() const noexcept;

  /**
   * Present the current canvas and detect terminal cell-count changes.
   *
   * A resize event is queued before presentation when the current dimensions
   * differ from the last observed values. Presentation still occurs if the
   * event queue is full; in that case the method returns
   * Status::EVENT_BUFFER_FULL after writing the canvas.
   *
   * @return Status::OK on success, Status::TERMINAL_NOT_AVAILABLE or
   *         Status::CANVAS_NOT_SET when not ready, or the first query,
   *         rendering, output, or event-buffer error.
   */
  Status draw() noexcept;

  /**
   * Return whether this object currently owns the terminal session.
   *
   * @return `true` after successful `take()` and before `release()`.
   */
  bool is_taken() const noexcept;

 private:
  /** Event queue shared with producers and consumers. */
  std::shared_ptr<EventBuffer> event_buffer_;
  /** Current canvas. */
  std::shared_ptr<Canvas> canvas_;
  /** Borrowed input terminal file descriptor. */
  int input_fd_;
  /** Borrowed output terminal file descriptor. */
  int output_fd_;
  /** Terminal settings restored by release(). */
  termios original_terminal_state_{};
  /** Whether original_terminal_state_ contains valid settings. */
  bool has_original_terminal_state_ = false;
  /** Whether this screen currently owns the terminal. */
  bool terminal_taken_ = false;
  /** Last terminal column count observed by `take()` or `draw()`. */
  size_t last_width_ = 0;
  /** Last terminal row count observed by `take()` or `draw()`. */
  size_t last_height_ = 0;
  /** Identifier assigned to the next generated event. */
  uint64_t next_event_id_ = 1;
};

}  // namespace tui
}  // namespace puc
