#pragma once

/**
 * @file screen.hpp
 * @brief PUC-owned asynchronous terminal presentation and observed geometry.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "msgs/screen_msgs.hpp"
#include "msgs/terminal_msgs.hpp"
#include "puc-cli/tui/rendering/canvas.hpp"
#include "puc-cli/tui/rendering/selection.hpp"
#include "puc-cli/tui/rendering/status.hpp"
#include "puc-cli/tui/rendering/zbuf.hpp"
#include "puc-cli/tui/terminal/session.hpp"
#include "puc-cli/tui/terminal/timeouts.hpp"
#include "utils/ipc/directory.hpp"

namespace puc::tui {

/**
 * Relative physical dimensions of one terminal character cell.
 *
 * The values form a ratio rather than an absolute pixel size. For example,
 * `{1, 2}` describes a cell twice as tall as it is wide. Layout uses this
 * ratio to translate visual aspect ratios into integer columns and rows.
 */
struct CellDimensions {
  std::size_t width  = 1U; /**< Relative physical width of a cell. */
  std::size_t height = 2U; /**< Relative physical height of a cell. */

  /** Compare both relative dimensions at compile time when possible. */
  constexpr bool operator==(const CellDimensions&) const noexcept = default;
};

/** Conventional cell proportions used when a terminal omits pixel metrics. */
inline constexpr CellDimensions kDefaultCellDimensions{};

/**
 * Own PUC's presentation policy above an asynchronous TerminalSession.
 *
 * Screen renders Canvas images and sends one-way commands through
 * `//screen/commands`. TerminalSession consumes those commands on the
 * caller-owned worker pool borrowed by the IPC Directory and publishes
 * observed geometry through `//screen/resize_events`. TerminalSubsystem
 * publishes normalized input through `//terminal/input_events`; Screen is that
 * route's transport consumer and retains decoded events in order until the
 * application drains them. There are deliberately no completion or error
 * replies. Recoverable size observations converge on a later presentation;
 * unrecoverable terminal failures terminate at the worker boundary.
 *
 * The command channel retains only its newest bounded backlog. Applications
 * must call `take()`, wait until `get_dimensions()` observes the initial resize
 * event, then present frames. After `release()`, they must submit no frames.
 */
class Screen {
 public:
  /** Construct over standard streams with an explicit IPC payload limit. */
  Screen(multithreading::JobQueue& workers, std::size_t maximum_message_bytes);

  /** Construct over descriptors with a caller-owned pool and payload limit. */
  Screen(int input_fd, int output_fd, multithreading::JobQueue& workers,
         std::size_t maximum_message_bytes);

  /**
   * Construct over lifecycle-owned protocol and terminal mechanisms.
   *
   * Both borrowed objects must outlive this Screen. The Directory must already
   * contain the canonical Screen and terminal-input channels, and the session
   * must already own its command subscription. This Screen owns its resize and
   * terminal-input subscriptions but never closes or unbinds those shared
   * mechanisms.
   */
  Screen(ipc::Directory& directory, terminal::TerminalSession& session);

  Screen(const Screen&)            = delete;
  Screen& operator=(const Screen&) = delete;
  Screen(Screen&&)                 = delete;
  Screen& operator=(Screen&&)      = delete;

  /** Stop asynchronous delivery and restore an active terminal session. */
  ~Screen();

  /**
   * Request terminal ownership with Screen's interactive input contract.
   *
   * The standard contract enables alternate-screen presentation, bracketed
   * paste, focus changes, drag tracking, and enhanced keyboard reporting.
   * Success means the command channel accepted the request, not that setup has
   * completed. The initial resize event marks usable geometry.
   */
  Status take() noexcept;

  /**
   * Request style reset and terminal restoration asynchronously.
   *
   * Success means the one-way request was accepted. Destruction remains a
   * synchronous restoration backstop if queued work is discarded at teardown.
   */
  Status release() noexcept;

  /** Read the most recently published character-cell dimensions. */
  Status get_dimensions(std::size_t& width, std::size_t& height) const noexcept;

  /** Read the latest cell counts and reduced physical cell proportions. */
  Status get_dimensions(std::size_t& width, std::size_t& height,
                        CellDimensions& cell_dimensions) const noexcept;

  /** Select the valid Canvas serialized by subsequent `draw()` calls. */
  Status set_canvas(std::shared_ptr<Canvas> canvas) noexcept;

  /** Render and enqueue the newest complete Canvas image for presentation. */
  Status draw() noexcept;

  /**
   * Drain normalized terminal events already consumed from the input channel.
   *
   * Events retain terminal publication order. The output is cleared first and
   * receives every event currently available as one batch. An empty successful
   * batch means no input has arrived since the preceding drain. Application
   * code owns only mode- and focus-specific policy; it never subscribes to or
   * decodes the terminal-input transport directly.
   */
  Status drain_input_events(std::vector<terminal::Event>& output) noexcept;

  /**
   * Read one available terminal block through the owned TerminalSession.
   *
   * Call only after the initial geometry observation proves `take()` has
   * completed, and only when the input descriptor is known readable. The
   * caller owns Decoder configuration and event routing; this mechanism call
   * neither creates a second terminal session nor duplicates byte decoding.
   */
  terminal::Status read_input(terminal::Decoder& decoder,
                              std::vector<terminal::Event>& events,
                              std::size_t& bytes_read, bool& end_of_input);

  /**
   * Route one normalized mouse event through the selection state machine.
   *
   * The ZBuffer is inspected from front to back. The first visible frame under
   * a press owns that hit even when it is not selectable, preventing selection
   * through overlays. A left press resets any completed selection and records
   * a possible drag. Movement captures the original selectable frame. A
   * release without movement remains NONE and, for an editor that opts in,
   * places its caret. Successive clicks in the same cell select a word and then
   * a line.
   *
   * `frame_layouts` is the map from the active AbsoluteLayout. Coordinates
   * dispatched to Frame are local and signed, so captured drags may extend
   * outside the frame. The caller must serialize mouse events in terminal
   * order; Screen additionally protects selection state from other callers.
   *
   * @param[in] event Terminal-normalized mouse event.
   * @param[in] z_buffer Active frames in back-to-front order.
   * @param[in] frame_layouts Current absolute rectangle for each frame id.
   * @return Status::OK for an applied or irrelevant event, or an error from
   *         coordinate conversion, state transition, or the selected Frame.
   */
  Status handle_mouse_event(
      const terminal::MouseEvent& event, const ZBuffer& z_buffer,
      const std::map<std::string, Canvas::Rect>& frame_layouts);

  /**
   * Select all logical text in one explicitly focused selectable frame.
   *
   * This positionless keyboard operation uses the same state machine as mouse
   * drags, double-clicks, and triple-clicks. It therefore replaces any prior
   * application selection and becomes the sole completed selection available
   * to `selected_text()` and `copy_selection()`.
   *
   * @param[in] frame_id Stable id of the focused frame in the active layout.
   * @param[in] frame Shared ownership of that exact frame.
   * @return Status::OK when all text was selected, Status::NO_SELECTION for an
   *         empty frame, or a validation/Frame error.
   */
  Status select_all(std::string_view frame_id,
                    const std::shared_ptr<Frame>& frame);

  /** Return the current multi-click timeout token, if a click is pending. */
  std::optional<terminal::TimeoutInput> pending_selection_timeout() const;

  /** Expire click-chain recognition when `input` names its current generation.
   */
  Status handle_selection_timeout(terminal::TimeoutInput input) noexcept;

  /** Reset the active selection and discard pending click/drag recognition. */
  Status reset_selection();

  /** Return the current Screen-owned selection lifecycle phase. */
  SelectionPhase selection_phase() const noexcept;

  /** Return the active selected frame id, if one exists. */
  std::optional<std::string> selected_frame_id() const;

  /** Extract selected UTF-8 from the completed frame without clipboard I/O. */
  Status selected_text(std::string& output) const;

  /**
   * Extract the completed logical selection and enqueue an OSC 52 write.
   *
   * Success means the one-way command was accepted; the terminal mechanism
   * performs clipboard transport asynchronously on the borrowed worker pool.
   */
  Status copy_selection(msg::ScreenClipboardSelection selection =
                            msg::ScreenClipboardSelection::CLIPBOARD) noexcept;

  /** Return whether Screen currently desires ownership of the terminal. */
  bool is_taken() const noexcept;

  /** Return the process-local channel directory for additional subscribers. */
  ipc::Directory& ipc_directory() noexcept { return *directory_; }

  /** Return the persistent channel/bootstrap result. */
  Status setup_status() const noexcept { return setup_status_; }

  /** Return the number of Screen commands currently pending delivery. */
  std::size_t pending_commands() const noexcept;

  /** Return how many oldest pending Screen commands have been evicted. */
  std::uint64_t dropped_commands() const noexcept;

 private:
  /** One left-button gesture captured by a selectable Frame. */
  struct PointerSelectionGesture {
    std::string frame_id;         /**< Stable target id. */
    std::shared_ptr<Frame> frame; /**< Captured target implementation. */
    Canvas::Rect rect;            /**< Most recently observed target bounds. */
    terminal::CellPosition press; /**< Absolute press position. */
    SelectionPosition anchor;     /**< Frame-local press position. */
    bool extended = false;        /**< Whether a drag update was dispatched. */
  };

  /** Completed click retained only for double/triple-click recognition. */
  struct ClickHistory {
    std::string frame_id;         /**< Target id of the preceding click. */
    std::shared_ptr<Frame> frame; /**< Target object of the preceding click. */
    terminal::CellPosition position; /**< Absolute click cell. */
    terminal::Modifiers modifiers;   /**< Modifiers held for that click. */
    std::size_t count = 0U;          /**< Consecutive clicks, at most two. */
  };

  /** Discard click recognition and invalidate any scheduled timeout token. */
  void clear_click_history() noexcept;

  /** Retain click recognition and issue a fresh timeout generation. */
  void arm_click_timeout() noexcept;

  /** Create or resolve channels, subscribe, and bind TerminalSession. */
  Status setup_channels(bool create_channels);

  /** Encode and transmit one command without waiting for its execution. */
  Status send_command(const msg::ScreenCommand& command) noexcept;

  /** Decode one asynchronously delivered geometry observation. */
  void receive_resize_event(ipc::Channel::Bytes payload) noexcept;

  /** Decode and retain one normalized terminal-input publication. */
  void receive_input_event(ipc::Channel::Bytes payload) noexcept;

  /** Current published Canvas, owned on the caller's presentation thread. */
  std::shared_ptr<Canvas> canvas_;
  /** TerminalSession owned only by standalone Screen constructors. */
  std::unique_ptr<terminal::TerminalSession> owned_terminal_session_;
  /** Borrowed active terminal mechanism. */
  terminal::TerminalSession* terminal_session_ = nullptr;
  /** Shared-memory channel carrying bounded one-way Screen commands. */
  std::shared_ptr<ipc::Channel> command_channel_;
  /** Shared-memory channel carrying latest-only geometry observations. */
  std::shared_ptr<ipc::Channel> resize_channel_;
  /** Ordered channel carrying normalized terminal input. */
  std::shared_ptr<ipc::Channel> input_channel_;
  /** Keeps Screen's resize callback active for the Screen lifetime. */
  ipc::Subscription resize_subscription_;
  /** Keeps Screen's terminal-input callback active for the Screen lifetime. */
  ipc::Subscription input_subscription_;
  /** Protects desired ownership and asynchronously observed geometry. */
  mutable std::mutex state_mutex_;
  /** Latest successfully decoded terminal geometry. */
  std::optional<msg::ScreenResizeEvent> latest_size_;
  /** Whether take has been requested without a later release request. */
  bool terminal_requested_ = false;
  /** Persistent channel setup result recorded by the constructor. */
  Status setup_status_ = Status::CHANNEL_SETUP_FAILED;
  /** Typed command encoder. */
  msg::ScreenCommandCodec command_codec_;
  /** Typed resize-event decoder. */
  msg::ScreenResizeEventCodec resize_codec_;
  /** Typed normalized terminal-event decoder. */
  msg::TerminalInputEventCodec input_codec_;
  /** Serializes event ingestion and application drains. */
  mutable std::mutex input_mutex_;
  /** Decoded terminal events awaiting application-specific handling. */
  std::vector<terminal::Event> input_events_;
  /** Persistent decoding failure for the current Screen generation. */
  Status input_status_ = Status::OK;
  /** Serializes pointer recognition, Frame dispatch, and selection queries. */
  mutable std::mutex selection_mutex_;
  /** Applies semantic operations and retains the active selected Frame. */
  SelectionStateMachine selection_state_machine_;
  /** Left-button press and capture currently awaiting release. */
  std::optional<PointerSelectionGesture> pointer_selection_gesture_;
  /** Previous completed click used to recognize words and lines. */
  std::optional<ClickHistory> click_history_;
  /** Monotonically changing identity for multi-click timeout inputs. */
  std::uint64_t next_click_timeout_generation_ = 0U;
  /** Token accepted by handle_selection_timeout(), if one is armed. */
  std::optional<terminal::TimeoutInput> selection_timeout_;
  /** Directory owned only by standalone Screen constructors. */
  std::unique_ptr<ipc::Directory> owned_directory_;
  /** Borrowed process-local channel directory. */
  ipc::Directory* directory_ = nullptr;
  /** Whether this Screen registered and must close its protocol channels. */
  bool owns_channels_ = false;
  /** Explicit limit used only when constructing local channels. */
  std::size_t maximum_message_bytes_ = 0U;
};

}  // namespace puc::tui
