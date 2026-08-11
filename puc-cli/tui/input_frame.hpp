#pragma once

/**
 * @file input_frame.hpp
 * @brief Editable, vertically scrollable terminal input frame.
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/text_editor_utils.hpp"

namespace puc::tui {

/** Surface currently occupying the inner input frame. */
enum class InputMode {
  NORMAL,   /**< Edit the ordinary prompt buffer. */
  COMMAND,  /**< Edit the temporary green command buffer. */
  TERMINAL, /**< Render the persistent libtmt virtual-terminal surface. */
};

/** Backward-compatible name for the shared reusable editor caret type. */
using InputCursor = TextCursor;

/** Read-only state snapshot intended for application orchestration and tests.
 */
struct InputFrameSnapshot {
  InputMode mode = InputMode::NORMAL; /**< Buffer currently receiving input. */
  std::string input_text;             /**< Preserved normal-mode UTF-8 text. */
  std::string command_text;           /**< Current command-mode UTF-8 text. */
  std::string notification;           /**< Bottom-row command/status text. */
  std::vector<std::string>
      command_help;   /**< Completion or usage rows above command input. */
  InputCursor cursor; /**< Caret in the active buffer. */
  std::size_t scroll_row = 0U;    /**< First visible wrapped content row. */
  bool escape_armed      = false; /**< Whether one Escape is pending. */
  bool paste_in_progress = false; /**< Whether a paste transaction is open. */
  std::size_t terminal_rows    = 0U; /**< Allocated virtual-terminal rows. */
  std::size_t terminal_columns = 0U; /**< Allocated virtual-terminal columns. */
  bool terminal_cursor_visible = true;  /**< libtmt cursor-visibility state. */
  bool terminal_session_active = false; /**< Whether a PTY owner should live. */
  std::size_t terminal_generation = 0U; /**< Identity of requested session. */
};

/**
 * Composite coordinator used for PUC's bottom-of-screen input terminal.
 *
 * The supplied rectangle is the outermost frame. Its first row is a gap and
 * its last row is a notification bar. Between them, a single-line box encloses
 * the editor. The inner editor has one-cell top, bottom, and right margins and
 * a four-cell line-number gutter that expands to five cells at 100 numbered
 * lines. The final gutter cell separates numbers from text. Every logical line
 * created by input is numbered, including newline-only lines; untouched visual
 * rows are not. When a word crosses the current width, its separating space is
 * materialized as a newline so the moved word becomes a numbered logical line.
 * A token wider than the editor remains one logical line and hard-wraps across
 * unnumbered continuation rows. Horizontal scrolling is deliberately absent.
 *
 * BoundingFrame owns the margins and border, AnnotatedTextFrame owns line
 * numbers, TextInputFrame owns normal text rendering, CmdFrame owns command
 * text, IntegratedTermFrame owns libtmt, and TextEditor owns reusable editing
 * mechanics. InputFrame only coordinates those views, mode transitions,
 * Escape timing, and the notification row.
 *
 * Mutable editor state is synchronized because rendering may run on a worker
 * while the application's input loop processes an event.
 */
class InputFrame final : public Frame {
 public:
  /** Monotonic clock used to recognize a double Escape. */
  using Clock = std::chrono::steady_clock;

  /** Smallest supported outermost width in terminal cells. */
  static constexpr std::size_t kMinimumWidth = 40U;
  /** Smallest supported outermost height in terminal cells. */
  static constexpr std::size_t kMinimumHeight = 5U;
  /** Smallest outer height that can contain libtmt's two required rows. */
  static constexpr std::size_t kTerminalMinimumHeight = 6U;
  /** Baseline maximum height before the 20-percent allowance becomes larger. */
  static constexpr std::size_t kBaselineMaximumHeight = 7U;
  /** Maximum interval between Escape presses in one double-Escape gesture. */
  static constexpr std::chrono::milliseconds kDoubleEscapeInterval{500};
  /** Prompt drawn into the input box's bottom margin after one Escape. */
  static constexpr std::string_view kClearPrompt =
      "hit escape again to clear the input";
  /** Command-mode counterpart to kClearPrompt. */
  static constexpr std::string_view kExitCommandPrompt =
      "hit escape again to exit command mode";

  /**
   * Construct an empty normal-mode editor.
   *
   * @param[in] name Human-readable Frame name.
   */
  explicit InputFrame(std::string name = "input");

  InputFrame(const InputFrame&)            = delete;
  InputFrame& operator=(const InputFrame&) = delete;
  InputFrame(InputFrame&&)                 = delete;
  InputFrame& operator=(InputFrame&&)      = delete;

  /** Destroy the hidden editor representation. */
  ~InputFrame() override;

  /**
   * Return the largest permitted frame height for a screen.
   *
   * The result is `max(20% of screen height, 7)`, clamped to the actual screen
   * height. Integer percentages round down to complete terminal rows.
   */
  static std::size_t maximum_height(std::size_t screen_height) noexcept;

  /** Return the lower sixty-percent height used by terminal mode. */
  static std::size_t terminal_height(std::size_t screen_height) noexcept;

  /** Return the active mode's minimum supported outer height. */
  std::size_t minimum_height() const noexcept;

  /**
   * Return the content-driven height this frame would prefer.
   *
   * The result includes the gap, notification row, and box margins, and is
   * clamped between kMinimumHeight and maximum_height(). `screen_width` is used
   * to account for wrapping and may be below kMinimumWidth during fallback
   * layout calculation.
   */
  std::size_t preferred_height(std::size_t screen_width,
                               std::size_t screen_height) const;

  /**
   * Apply one decoded terminal event at a caller-supplied monotonic time.
   *
   * Release-only key events and event alternatives unrelated to editing are
   * harmless no-ops. Paste BEGIN/DATA/END is transactional: CANCEL restores
   * the buffer as it was at BEGIN.
   */
  Status handle_event(const terminal::Event& event, Clock::time_point now);

  /** Apply one event using Clock::now(). */
  Status handle_event(const terminal::Event& event);

  /** Expire the one-Escape prompt after kDoubleEscapeInterval elapses. */
  void advance_time(Clock::time_point now);

  /** Replace the UTF-8 message drawn in the bottom notification row. */
  void set_notification(std::string notification);

  /** Replace command completion/usage rows displayed above the command box. */
  void set_command_help(std::vector<std::string> help);

  /** Replace the command buffer and place its caret after the supplied text. */
  Status replace_command_text(std::string text);

  /** Return from command mode to the preserved normal input buffer. */
  void leave_command_mode();

  /**
   * Feed process output to the persistent libtmt terminal surface.
   *
   * Bytes written before the first terminal-mode draw are retained and
   * replayed once the inner surface has dimensions. This API consumes process
   * output, not keyboard input intended for a child process.
   */
  Status write_terminal(std::string_view output);

  /**
   * Take terminal replies generated by libtmt (for example a DSR response).
   *
   * The returned bytes should be forwarded to the process that owns the
   * terminal. Reading consumes the accumulated replies.
   */
  std::string take_terminal_responses();

  /** Reset the current libtmt display without replacing its owning process. */
  void reset_terminal();

  /**
   * Request a fresh terminal session and show its cleared surface.
   *
   * The generation in snapshot() advances so the external PTY owner can
   * replace any running child process.
   */
  void start_new_terminal();

  /**
   * Mark the terminal session closed and return a visible terminal to normal.
   *
   * PTY owners call this when the child exits; command mode can use the same
   * operation for an explicit close command later.
   */
  void close_terminal();

  /** Clear the active buffer without changing modes. */
  void clear();

  /** Return a consistent copy of the externally useful editor state. */
  InputFrameSnapshot snapshot() const;

  /** Draw the complete composite frame into its assigned rectangle. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

  /** Input text participates in Screen's logical selection routing. */
  bool is_selectable() const noexcept override;

  /** Apply a drag, word, line, all-text, or reset operation to the buffer. */
  Status update_selection(const SelectionEvent& event) override;

  /** Extract the active buffer's selected logical UTF-8 text. */
  Status selected_text(std::string& output) const override;

  /** InputFrame supports Screen's stationary single-click caret operation. */
  bool accepts_cursor_placement() const noexcept override;

  /** Move the caret to a character addressed in frame-local cells. */
  Status place_cursor(SelectionPosition position) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden synchronized editor state. */
};

}  // namespace puc::tui
