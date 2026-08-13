#pragma once

/**
 * @file session.hpp
 * @brief RAII ownership of POSIX terminal transport and active modes.
 */

#include <termios.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "msgs/screen_msgs.hpp"
#include "puc-cli/tui/terminal/decoder.hpp"
#include "puc-cli/tui/terminal/event.hpp"
#include "puc-cli/tui/terminal/sequences.hpp"
#include "puc-cli/tui/terminal/status.hpp"
#include "utils/ipc/directory.hpp"

namespace puc {
namespace terminal {

/** Raw terminal geometry reported by the operating system. */
struct TerminalSize {
  std::size_t width  = 0; /**< Character-cell columns. */
  std::size_t height = 0; /**< Character-cell rows. */
  std::size_t pixel_width =
      0; /**< Total pixel width, or zero if unavailable. */
  std::size_t pixel_height =
      0; /**< Total pixel height, or zero if unavailable. */

  /** Compare cell and optional pixel dimensions. */
  constexpr bool operator==(const TerminalSize&) const noexcept = default;
};

/**
 * Own the reversible operating-system and protocol state of one terminal.
 *
 * File descriptors are borrowed and must outlive the session. This class is a
 * PUC has one interactive terminal contract: signal-generating input remains
 * enabled; alternate-screen presentation, bracketed paste, focus events, drag
 * tracking, and enhanced keyboard reporting are active for the session.
 * `release()` reverses that contract and restores the original termios state.
 */
class TerminalSession {
 public:
  /** Construct a session over standard input and standard output. */
  TerminalSession() noexcept;

  /** Construct a session over caller-owned descriptors. */
  TerminalSession(int input_fd, int output_fd) noexcept;

  TerminalSession(const TerminalSession&)            = delete;
  TerminalSession& operator=(const TerminalSession&) = delete;

  /** Release an active session without propagating teardown failures. */
  ~TerminalSession();

  /** Enter raw mode and activate PUC's fixed interactive terminal contract. */
  Status take() noexcept;

  /** Restore every mode changed by take(). Safe to call repeatedly. */
  Status release() noexcept;

  /** Write bytes to the active terminal, retrying interrupted writes. */
  Status write(std::string_view bytes) noexcept;

  /** Read one available block and feed it to a decoder. */
  Status read(Decoder& decoder, std::vector<Event>& events,
              std::size_t& bytes_read, bool& end_of_input);

  /** Query current character-cell dimensions using TIOCGWINSZ. */
  Status get_size(TerminalSize& size) const noexcept;

  /** Set a host-terminal clipboard through OSC 52. */
  Status set_clipboard(ClipboardSelection selection,
                       std::string_view data) noexcept;

  /** Request clipboard data asynchronously through OSC 52. */
  Status query_clipboard(ClipboardSelection selection) noexcept;

  /** Ask the terminal to report its Kitty keyboard flags. */
  Status query_keyboard_protocol() noexcept;

  /**
   * Subscribe this mechanism adapter to the one-way Screen command channel.
   *
   * The Directory must already contain both channels named by
   * `msgs/screen_msgs.hpp` and must outlive this binding. Delivery policy and
   * worker ownership remain the Directory's responsibility.
   */
  Status bind_screen_channels(ipc::Directory& directory);

  /** Disable the Screen command subscription after delivery has quiesced. */
  void unbind_screen_channels() noexcept;

  /** Return whether this session currently has a Screen command subscription.
   */
  bool screen_channels_bound() const noexcept {
    return screen_command_subscription_.active();
  }

  /** Return whether take() has completed successfully. */
  bool active() const noexcept { return active_; }

 private:
  enum ActiveMode : std::uint32_t {
    ACTIVE_ALTERNATE_SCREEN = 1U << 0U,
    ACTIVE_HIDDEN_CURSOR    = 1U << 1U,
    ACTIVE_DISABLED_WRAP    = 1U << 2U,
    ACTIVE_BRACKETED_PASTE  = 1U << 3U,
    ACTIVE_FOCUS            = 1U << 4U,
    ACTIVE_MOUSE_DRAG       = 1U << 5U,
    ACTIVE_SGR_MOUSE        = 1U << 6U,
    ACTIVE_KITTY_KEYBOARD   = 1U << 7U,
  };

  Status write_all(std::string_view bytes) noexcept;
  void build_enter_sequence(std::string& output);
  void build_leave_sequence(std::string& output) const;
  void receive_screen_command(ipc::Channel::Bytes payload) noexcept;
  void execute_screen_command(const msg::ScreenCommand& command) noexcept;
  void publish_size_if_changed() noexcept;

  int input_fd_;
  int output_fd_;
  termios original_terminal_state_{};
  bool has_original_terminal_state_ = false;
  bool active_                      = false;
  std::uint32_t active_modes_       = 0;
  ipc::Directory* screen_directory_ = nullptr;    /**< Bound event directory. */
  ipc::Subscription screen_command_subscription_; /**< Command callback. */
  msg::ScreenCommandCodec screen_command_codec_;  /**< Command decoder. */
  msg::ScreenResizeEventCodec resize_event_codec_; /**< Resize encoder. */
  std::optional<TerminalSize>
      last_published_size_;        /**< Last successfully published geometry. */
  std::string screen_final_bytes_; /**< Screen policy applied before release. */
};

}  // namespace terminal
}  // namespace puc
