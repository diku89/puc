#pragma once

/**
 * @file terminal_test_runner.hpp
 * @brief Deterministic state machine for the interactive input conformance app.
 */

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/terminal/event.hpp"

namespace puc::terminal {

/** Stable identities of the manual input checks, in presentation order. */
enum class InputConformanceTest {
  TEXT,
  ARROW_UP,
  ARROW_DOWN,
  ARROW_LEFT,
  ARROW_RIGHT,
  ESCAPE,
  CONTROL_KEY,
  ALT_KEY,
  MOUSE_CLICK,
  MOUSE_SCROLL,
  MOUSE_DRAG,
  CLIPBOARD_PASTE,
  FILE_DROP,
  FOCUS,
};

/** Immutable metadata shared by CLI listing, rendering, and reports. */
struct InputConformanceTestDescriptor {
  InputConformanceTest test = InputConformanceTest::TEXT; /**< Stable id. */
  std::string_view cli_name;    /**< Kebab-case `--test` argument. */
  std::string_view name;        /**< Human-readable short label. */
  std::string_view instruction; /**< Operator action required to pass. */

  /** Compare every metadata field. */
  constexpr bool operator==(
      const InputConformanceTestDescriptor&) const noexcept = default;
};

/** Final result assigned to one completed manual check. */
enum class InputConformanceOutcome {
  PASSED,
  TIMED_OUT,
};

/** Current visual phase of the interactive runner. */
enum class InputConformancePhase {
  ACTIVE,
  PASSED_FEEDBACK,
  TIMED_OUT_FEEDBACK,
  COMPLETE,
};

/** Half-open terminal-cell region used by mouse checks. */
struct InputInteractionRegion {
  std::size_t x      = 0U; /**< Zero-based left column. */
  std::size_t y      = 0U; /**< Zero-based top row. */
  std::size_t width  = 0U; /**< Number of included columns. */
  std::size_t height = 0U; /**< Number of included rows. */

  /** Compare complete region geometry. */
  constexpr bool operator==(const InputInteractionRegion&) const noexcept =
      default;
};

/** Immutable report entry for one resolved conformance check. */
struct InputConformanceResult {
  InputConformanceTest test = InputConformanceTest::TEXT; /**< Check id. */
  InputConformanceOutcome outcome =
      InputConformanceOutcome::TIMED_OUT; /**< Pass or timeout. */
  std::string name;                       /**< Short report label. */
  std::string instruction;                /**< Prompt shown to the operator. */
  std::string detail; /**< Decoder evidence or timeout explanation. */

  /** Compare every report field. */
  bool operator==(const InputConformanceResult&) const = default;
};

/** Consistent render snapshot copied under the runner's lock. */
struct InputConformanceView {
  InputConformanceTest test = InputConformanceTest::TEXT; /**< Stable id. */
  std::size_t test_number   = 0U; /**< One-based current number, or total. */
  std::size_t test_count    = 0U; /**< Checks selected for this run. */
  unsigned int seconds_remaining = 0U; /**< Displayed heartbeat countdown. */
  InputConformancePhase phase =
      InputConformancePhase::ACTIVE; /**< Border and transition state. */
  std::string name;                  /**< Current short check label. */
  std::string instruction;           /**< Current operator prompt. */
  std::string last_observation; /**< Most recently decoded input category. */

  /** Compare complete snapshots. */
  bool operator==(const InputConformanceView&) const = default;
};

/**
 * Consume decoded terminal events and one-hertz ticks as a timed test plan.
 *
 * The runner contains no terminal I/O and no rendering code. Calls are
 * synchronized because heartbeat delivery, input polling, and frame rendering
 * may occur on different threads. Every test begins with fifteen one-hertz
 * ticks. A match enters a short success phase; a fifteenth unmatched tick
 * records a timeout. `update()` advances after the configured feedback
 * duration.
 */
class InputConformanceRunner {
 public:
  using Clock = std::chrono::steady_clock; /**< Monotonic transition clock. */
  using TimePoint = Clock::time_point;     /**< Injectable unit-test time. */

  /** Clipboard nonce that must return through terminal paste input. */
  inline static constexpr std::string_view kClipboardToken = "PUC-clipboard-42";

  /** Number of one-hertz ticks available to every check. */
  inline static constexpr unsigned int kTimeoutSeconds = 15U;

  /** Normal duration of green/red result feedback. */
  inline static constexpr std::chrono::milliseconds kDefaultFeedbackDuration{
      350};

  /** Construct the full plan, or a plan containing only `selected_test`. */
  explicit InputConformanceRunner(
      std::chrono::milliseconds feedback_duration = kDefaultFeedbackDuration,
      std::optional<InputConformanceTest> selected_test = std::nullopt);

  InputConformanceRunner(const InputConformanceRunner&)            = delete;
  InputConformanceRunner& operator=(const InputConformanceRunner&) = delete;
  InputConformanceRunner(InputConformanceRunner&&)                 = delete;
  InputConformanceRunner& operator=(InputConformanceRunner&&)      = delete;

  /** Update the current box region used to validate pointer coordinates. */
  void set_interaction_region(InputInteractionRegion region) noexcept;

  /** Consume one decoded input event from the real terminal Decoder. */
  void observe(const Event& event, TimePoint now = Clock::now());

  /** Consume one valid NullMessage from `//metronome/1hz`. */
  void tick(TimePoint now = Clock::now());

  /** Advance after a pass/timeout feedback interval has elapsed. */
  void update(TimePoint now = Clock::now());

  /** Return a coherent copy suitable for one frame render. */
  InputConformanceView view() const;

  /** Return completed results in test order. */
  std::vector<InputConformanceResult> results() const;

  /** Return whether every test has produced a result. */
  bool finished() const noexcept;

  /** Return the number of checks selected for this runner instance. */
  std::size_t plan_size() const noexcept;

  /** Return the fixed number of checks in the complete built-in registry. */
  static constexpr std::size_t test_count() noexcept { return 14U; }

 private:
  /** Resolve one plan index to its stable test identifier. */
  InputConformanceTest planned_test(std::size_t index) const noexcept;

  /** Mark the active test passed and begin green feedback. */
  void pass(std::string detail, TimePoint now);

  /** Mark the active test timed out and begin red feedback. */
  void time_out(TimePoint now);

  /** Select the next descriptor and clear test-specific matcher state. */
  void advance_locked() noexcept;

  /** Return whether a cell is inside the latest interaction box. */
  bool contains(CellPosition position) const noexcept;

  /** Consume text relevant to ordinary typing or clipboard paste. */
  void observe_text(std::string_view text, bool bracketed, TimePoint now);

  mutable std::mutex mutex_; /**< Protects every field below. */
  const std::optional<InputConformanceTest>
      selected_test_; /**< Optional one-check plan selector. */
  std::chrono::milliseconds feedback_duration_; /**< Green/red hold time. */
  std::size_t current_index_      = 0U; /**< Zero-based descriptor index. */
  unsigned int seconds_remaining_ = kTimeoutSeconds; /**< Countdown value. */
  InputConformancePhase phase_ =
      InputConformancePhase::ACTIVE; /**< Current transition phase. */
  TimePoint feedback_until_{};       /**< Earliest next-test transition. */
  InputInteractionRegion interaction_region_;   /**< Mouse target geometry. */
  std::vector<InputConformanceResult> results_; /**< Ordered final outcomes. */
  std::string last_observation_ = "Waiting for input"; /**< UI diagnostic. */
  std::string text_buffer_;      /**< Text matcher accumulation. */
  bool saw_scroll_up_   = false; /**< Current scroll test saw positive Y. */
  bool saw_scroll_down_ = false; /**< Current scroll test saw negative Y. */
  bool drag_started_    = false; /**< Current drag test saw left press. */
  bool drag_moved_      = false; /**< Current drag traveled enough cells. */
  CellPosition drag_origin_;     /**< Initial press coordinate. */
  bool paste_started_ = false;   /**< Paste-backed check saw a BEGIN event. */
  std::string paste_buffer_;     /**< Clipboard token or dropped-path bytes. */
  bool focus_lost_ = false;      /**< Focus test saw the terminal blur. */
};

/** Return every conformance descriptor in default execution order. */
std::span<const InputConformanceTestDescriptor>
input_conformance_tests() noexcept;

/** Find a test by the exact kebab-case name accepted by `--test`. */
std::optional<InputConformanceTest> find_input_conformance_test(
    std::string_view cli_name) noexcept;

/** Return the stable kebab-case command-line name for one test. */
std::string_view input_conformance_test_cli_name(
    InputConformanceTest test) noexcept;

/** Return a stable short label for one conformance test. */
std::string_view input_conformance_test_name(
    InputConformanceTest test) noexcept;

/** Return the operator prompt for one conformance test. */
std::string_view input_conformance_instruction(
    InputConformanceTest test) noexcept;

/** Return stable report text for one final outcome. */
std::string_view input_conformance_outcome_name(
    InputConformanceOutcome outcome) noexcept;

}  // namespace puc::terminal
