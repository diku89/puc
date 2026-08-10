#pragma once

/**
 * @file integrated_term_frame.hpp
 * @brief Persistent libtmt virtual-terminal rendering surface.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "puc-cli/tui/frame.hpp"

namespace puc::tui {

/** Read-only virtual-terminal display and lifecycle state. */
struct IntegratedTermFrameSnapshot {
  std::size_t rows    = 0U;   /**< Allocated libtmt rows. */
  std::size_t columns = 0U;   /**< Allocated libtmt columns. */
  bool cursor_visible = true; /**< Last DECTCEM cursor-visibility state. */
  bool session_active =
      false; /**< Whether an external PTY owner should live. */
  std::size_t generation = 0U; /**< Identity of the requested PTY session. */
};

/** Semantic colors for default terminal cells and the terminal caret. */
struct IntegratedTermFrameStyle {
  Theme::ColorTypes text_color =
      Theme::ColorTypes::TEXT_SECONDARY; /**< Default terminal foreground. */
  Theme::ColorTypes background_color =
      Theme::ColorTypes::SECONDARY; /**< Default terminal background. */
  Theme::ColorTypes cursor_color =
      Theme::ColorTypes::TERTIARY; /**< Terminal caret background. */
};

/**
 * A libtmt-owned screen that renders process output but owns no process.
 *
 * The application remains responsible for creating a PTY, forwarding keyboard
 * input, feeding output through write(), and observing lifecycle generations.
 * This separation lets command mode later reset, replace, or close terminals
 * without coupling the reusable view to one process implementation.
 */
class IntegratedTermFrame final : public Frame {
 public:
  /** Construct an unallocated virtual-terminal view. */
  explicit IntegratedTermFrame(std::string name = "integrated terminal",
                               IntegratedTermFrameStyle style = {});

  IntegratedTermFrame(const IntegratedTermFrame&)            = delete;
  IntegratedTermFrame& operator=(const IntegratedTermFrame&) = delete;
  IntegratedTermFrame(IntegratedTermFrame&&)                 = delete;
  IntegratedTermFrame& operator=(IntegratedTermFrame&&)      = delete;

  /** Close libtmt after synchronized users have stopped. */
  ~IntegratedTermFrame() override;

  /** Feed child-process output now or queue it until the first draw. */
  Status write(std::string_view output);

  /** Consume terminal replies such as device-status responses. */
  std::string take_responses();

  /** Reset display state while retaining the current PTY generation. */
  void reset();

  /** Activate a session, preserving output queued before first use. */
  void activate_session();

  /** Request a new cleared terminal and advance the PTY generation. */
  void start_new_session();

  /** End the PTY lifecycle and destroy the virtual screen. */
  void close_session();

  /** Return a consistent display/lifecycle snapshot. */
  IntegratedTermFrameSnapshot snapshot() const;

  /** Draw and resize the virtual terminal to the exact assigned rectangle. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden synchronized libtmt state. */
};

}  // namespace puc::tui
