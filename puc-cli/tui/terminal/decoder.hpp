#pragma once

/**
 * @file decoder.hpp
 * @brief Incremental decoder for UTF-8 and modern terminal input protocols.
 */

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/tui/terminal/event.hpp"
#include "puc-cli/tui/terminal/input.hpp"
#include "puc-cli/tui/terminal/status.hpp"
#include "puc-cli/tui/terminal/timeouts.hpp"

namespace puc {
namespace terminal {

/** Resource limits applied to untrusted terminal input. */
struct DecoderLimits {
  std::size_t maximum_pending_bytes =
      2U * 1024U * 1024U; /**< Bytes retained between feed calls. */
  std::size_t maximum_sequence_bytes =
      4096U; /**< Bytes accepted in a non-clipboard control sequence. */
  std::size_t maximum_paste_bytes =
      16U * 1024U * 1024U; /**< Bytes emitted by one bracketed paste. */
  std::size_t maximum_clipboard_bytes =
      1024U * 1024U; /**< Decoded bytes accepted from one OSC 52 response. */
};

/**
 * Decode arbitrarily chunked terminal bytes into semantic events.
 *
 * Decoder is deliberately independent of file descriptors and clocks. A
 * caller feeds bytes obtained from any transport and explicitly delivers the
 * generation-tagged token returned by `pending_timeout()`. Every byte sequence
 * begins in one contiguous Trie; an accepting node either emits an event or
 * selects a bounded parameterized-protocol capture. The caller owns clocks
 * and synchronization.
 */
class Decoder {
 public:
  /** Construct an unconfigured decoder with default resource limits. */
  Decoder();

  /** Construct an unconfigured decoder with explicit resource limits. */
  explicit Decoder(DecoderLimits limits);

  /**
   * Load the complete input source hierarchy into this decoder.
   *
   * Setup applies terminfo first, then an optional terminal-specific profile,
   * the merged universal `input_keys.toml`, and finally the merged defaults
   * for the compile target's detected operating-system family. It is
   * transactional: failure preserves the decoder's previous mappings and
   * pending state. No mappings are compiled into the application.
   *
   * @param[in] properties Application-owned immutable properties gateway.
   * @param[in] terminal_name terminfo/profile name, or empty to use `TERM`.
   * @param[in] output_fd Descriptor supplied to terminfo initialization.
   * @return Status::OK or a configuration/terminfo error.
   */
  Status setup(properties::Properties& properties,
               std::string_view terminal_name = {}, int output_fd = 1);

  /**
   * Append bytes and emit every complete event now available.
   *
   * Incomplete UTF-8 and control sequences remain buffered across calls.
   * Malformed sequences become UnknownEvent values so decoding can recover.
   *
   * @return Status::OK or Status::INPUT_LIMIT_EXCEEDED.
   */
  Status feed(std::string_view bytes, std::vector<Event>& events);

  /** Return the current timeout token, or none when no input path is pending.
   */
  std::optional<TimeoutInput> pending_timeout() const noexcept {
    return pending_timeout_;
  }

  /**
   * Resolve one explicitly timed-out Trie or parameterized-protocol path.
   *
   * Stale generations are harmless no-ops. A current timeout commits the
   * longest exact match when one exists, reports a path with no exact match as
   * an incomplete UnknownEvent, resets the cursor, and retries any suffix from
   * the root. Bracketed paste is never interrupted.
   */
  Status handle_timeout(TimeoutInput input, std::vector<Event>& events);

  /**
   * Finish an input stream and report any truncated sequence as unknown.
   *
   * An active paste emits its remaining safe data followed by CANCEL.
   */
  Status finish(std::vector<Event>& events);

  /** Discard pending protocol state while preserving configured key mappings.
   */
  void reset() noexcept;

  /** Return the number of bytes currently awaiting more input. */
  std::size_t pending_bytes() const noexcept;

  /** Return whether input is currently inside a bracketed paste. */
  bool paste_in_progress() const noexcept { return paste_in_progress_; }

 private:
  /** Take ownership of one fully merged map built by setup(). */
  Decoder(InputMap input_map, DecoderLimits limits);

  enum class ParseResult {
    CONSUMED,
    NEED_MORE,
  };

  using InputTrie = InputMap::Trie;

  /** Persistent longest-match cursor over the immutable input trie. */
  struct TrieCursor {
    InputTrie::NodeIndex node       = InputTrie::root();
    InputTrie::NodeIndex last_match = InputTrie::kInvalidNode;
    std::size_t scanned             = 0;
    std::size_t last_match_size     = 0;

    /** Return the cursor to the sentinel root. */
    void reset() noexcept {
      node            = InputTrie::root();
      last_match      = InputTrie::kInvalidNode;
      scanned         = 0;
      last_match_size = 0;
    }
  };

  Status process(std::vector<Event>& events);
  Status advance_trie(std::vector<Event>& events, ParseResult& result);
  Status execute_action(InputTrie::NodeIndex node, std::size_t byte_count,
                        std::vector<Event>& events, ParseResult& result);
  Status advance_active_action(std::vector<Event>& events, ParseResult& result);
  Status parse_alt_key(std::vector<Event>& events, ParseResult& result);
  Status capture_ss3(std::vector<Event>& events, ParseResult& result);
  Status capture_csi(std::vector<Event>& events, ParseResult& result);
  Status capture_osc(std::vector<Event>& events, ParseResult& result);
  Status capture_st_string(std::vector<Event>& events, ParseResult& result);
  Status capture_paste(std::vector<Event>& events, ParseResult& result);
  Status append_paste_data(std::string_view data, std::vector<Event>& events);
  void reset_trie_cursor() noexcept;
  void reset_active_action() noexcept;
  void update_pending_timeout(bool rearm) noexcept;
  Status resolve_timed_out_active_action(std::vector<Event>& events);
  const EnterInputProtocol* active_protocol() const noexcept;
  Status parse_csi(std::string_view sequence, std::vector<Event>& events);
  Status parse_sgr_mouse(std::string_view sequence, std::vector<Event>& events);
  Status parse_osc(std::string_view sequence, std::vector<Event>& events);
  Status parse_osc52(std::string_view sequence, std::vector<Event>& events);
  ParseResult parse_text(std::vector<Event>& events);
  void consume(std::size_t byte_count) noexcept;
  void compact();
  std::string_view pending() const noexcept;

  DecoderLimits limits_;
  std::string buffer_;
  std::size_t offset_ = 0;
  InputTrie input_trie_;
  TrieCursor trie_cursor_;
  InputTrie::NodeIndex active_action_node_ = InputTrie::kInvalidNode;
  std::string active_sequence_;
  std::string paste_candidate_;
  std::string paste_end_sequence_;
  bool paste_in_progress_                = false;
  bool paste_discarding_                 = false;
  std::size_t paste_bytes_               = 0;
  std::uint64_t next_timeout_generation_ = 0U;
  std::optional<TimeoutInput> pending_timeout_;
};

}  // namespace terminal
}  // namespace puc
