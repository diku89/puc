/**
 * @file terminal_test_runner.cpp
 * @brief Event matching and heartbeat-driven terminal conformance plan.
 */

#include "puc-cli/terminal/terminal_test_runner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace puc::terminal {
namespace {

/** Single source of truth for order, CLI names, labels, and instructions. */
constexpr std::array<InputConformanceTestDescriptor,
                     InputConformanceRunner::test_count()>
    kTests{{
        {InputConformanceTest::TEXT, "text", "Committed text",
         "Type the lowercase word: puc"},
        {InputConformanceTest::ARROW_UP, "arrow-up", "Up arrow",
         "Press the Up arrow key"},
        {InputConformanceTest::ARROW_DOWN, "arrow-down", "Down arrow",
         "Press the Down arrow key"},
        {InputConformanceTest::ARROW_LEFT, "arrow-left", "Left arrow",
         "Press the Left arrow key"},
        {InputConformanceTest::ARROW_RIGHT, "arrow-right", "Right arrow",
         "Press the Right arrow key"},
        {InputConformanceTest::ESCAPE, "escape", "Escape key timeout",
         "Press the Escape key once"},
        {InputConformanceTest::CONTROL_KEY, "control-key", "Control key",
         "Press Ctrl-A"},
        {InputConformanceTest::ALT_KEY, "alt-key", "Alt/Option key",
         "Press Alt-N (Option-N when configured as Meta)"},
        {InputConformanceTest::MOUSE_CLICK, "mouse-click", "Mouse click",
         "Left-click anywhere inside this box"},
        {InputConformanceTest::MOUSE_SCROLL, "mouse-scroll", "Mouse scroll",
         "Point inside this box, then scroll up and down"},
        {InputConformanceTest::MOUSE_DRAG, "mouse-drag", "Mouse drag",
         "Drag left mouse at least 3 cells inside this box, then release"},
        {InputConformanceTest::CLIPBOARD_PASTE, "clipboard-paste",
         "Clipboard paste",
         "Select PUC-clipboard-42, invoke the configured copy chord "
         "(Cmd-C on Darwin; Ctrl-Shift-C on Linux/BSD), then paste it here"},
        {InputConformanceTest::FILE_DROP, "file-drop", "File-drop fallback",
         "Drag any file from a file manager into this box; a pasted path is "
         "the expected portable fallback"},
        {InputConformanceTest::FOCUS, "focus", "Focus reporting",
         "Focus another window, then return focus to this terminal"},
    }};

/** Bound untrusted matcher accumulation without obscuring expected tokens. */
constexpr std::size_t kMaximumMatcherBytes = 4096U;

/** Return the active test's immutable identifier. */
InputConformanceTest test_at(std::size_t index) noexcept {
  return index < kTests.size() ? kTests[index].test : kTests.back().test;
}

/** Find immutable metadata for an enum value, including validation. */
const InputConformanceTestDescriptor* descriptor_for(
    InputConformanceTest test) noexcept {
  const auto descriptor =
      std::ranges::find(kTests, test, &InputConformanceTestDescriptor::test);
  return descriptor == kTests.end() ? nullptr : &*descriptor;
}

/** Return whether a KeyEvent is a press/repeat of one named key. */
bool is_named_key(const KeyEvent& event, NamedKey key) noexcept {
  const auto* named = std::get_if<NamedKey>(&event.key.value);
  return named != nullptr && *named == key &&
         event.action != KeyAction::RELEASE;
}

/** Return whether a KeyEvent is a press/repeat of one Unicode scalar. */
bool is_character_key(const KeyEvent& event, char32_t key) noexcept {
  const auto* character = std::get_if<char32_t>(&event.key.value);
  return character != nullptr && *character == key &&
         event.action != KeyAction::RELEASE;
}

/** Return unsigned distance between two coordinates without underflow. */
std::size_t distance(std::size_t first, std::size_t second) noexcept {
  return first > second ? first - second : second - first;
}

/** Return a concise user-facing category for a decoded event. */
std::string event_description(const Event& event) {
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (const auto* named = std::get_if<NamedKey>(&key->key.value)) {
      switch (*named) {
        case NamedKey::UP:
          return "Decoded key: UP";
        case NamedKey::DOWN:
          return "Decoded key: DOWN";
        case NamedKey::LEFT:
          return "Decoded key: LEFT";
        case NamedKey::RIGHT:
          return "Decoded key: RIGHT";
        default:
          return "Decoded named key";
      }
    }
    return "Decoded character key";
  }
  if (const auto* text = std::get_if<TextEvent>(&event)) {
    return "Decoded text (" + std::to_string(text->utf8.size()) + " bytes)";
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    return "Decoded mouse at " + std::to_string(mouse->position.x) + "," +
           std::to_string(mouse->position.y);
  }
  if (std::holds_alternative<ScrollEvent>(event)) {
    return "Decoded mouse wheel";
  }
  if (const auto* paste = std::get_if<PasteEvent>(&event)) {
    return "Decoded paste phase " +
           std::to_string(static_cast<unsigned int>(paste->phase));
  }
  if (const auto* focus = std::get_if<FocusEvent>(&event)) {
    return focus->focused ? "Decoded focus gained" : "Decoded focus lost";
  }
  if (std::holds_alternative<ClipboardEvent>(event)) {
    return "Decoded OSC 52 clipboard response";
  }
  if (const auto* command = std::get_if<CommandEvent>(&event)) {
    return command->command == Command::COPY ? "Decoded command: COPY"
                                             : "Decoded command";
  }
  if (std::holds_alternative<TerminalResponseEvent>(event)) {
    return "Decoded terminal protocol response";
  }
  return "Unknown or malformed input sequence";
}

}  // namespace

std::span<const InputConformanceTestDescriptor>
input_conformance_tests() noexcept {
  return kTests;
}

std::optional<InputConformanceTest> find_input_conformance_test(
    std::string_view cli_name) noexcept {
  const auto descriptor = std::ranges::find(
      kTests, cli_name, &InputConformanceTestDescriptor::cli_name);
  if (descriptor == kTests.end()) {
    return std::nullopt;
  }
  return descriptor->test;
}

std::string_view input_conformance_test_cli_name(
    InputConformanceTest test) noexcept {
  const InputConformanceTestDescriptor* descriptor = descriptor_for(test);
  return descriptor == nullptr ? "unknown" : descriptor->cli_name;
}

std::string_view input_conformance_test_name(
    InputConformanceTest test) noexcept {
  const InputConformanceTestDescriptor* descriptor = descriptor_for(test);
  return descriptor == nullptr ? "Unknown test" : descriptor->name;
}

std::string_view input_conformance_instruction(
    InputConformanceTest test) noexcept {
  const InputConformanceTestDescriptor* descriptor = descriptor_for(test);
  return descriptor == nullptr ? "Unknown test" : descriptor->instruction;
}

std::string_view input_conformance_outcome_name(
    InputConformanceOutcome outcome) noexcept {
  switch (outcome) {
    case InputConformanceOutcome::PASSED:
      return "PASS";
    case InputConformanceOutcome::TIMED_OUT:
      return "TIMEOUT";
  }
  return "UNKNOWN";
}

InputConformanceRunner::InputConformanceRunner(
    std::chrono::milliseconds feedback_duration,
    std::optional<InputConformanceTest> selected_test)
    : selected_test_(selected_test),
      feedback_duration_(
          std::max(feedback_duration, std::chrono::milliseconds::zero())) {
  results_.reserve(plan_size());
}

void InputConformanceRunner::set_interaction_region(
    InputInteractionRegion region) noexcept {
  const std::lock_guard lock(mutex_);
  interaction_region_ = region;
}

void InputConformanceRunner::observe(const Event& event, TimePoint now) {
  const std::lock_guard lock(mutex_);
  last_observation_ = event_description(event);
  if (phase_ != InputConformancePhase::ACTIVE ||
      current_index_ >= plan_size()) {
    return;
  }

  const InputConformanceTest current = planned_test(current_index_);
  if (const auto* text = std::get_if<TextEvent>(&event)) {
    observe_text(text->utf8, false, now);
    return;
  }
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    if (!key->text.empty()) {
      observe_text(key->text, false, now);
      if (phase_ != InputConformancePhase::ACTIVE) {
        return;
      }
    }
    const bool matched = (current == InputConformanceTest::ARROW_UP &&
                          is_named_key(*key, NamedKey::UP)) ||
                         (current == InputConformanceTest::ARROW_DOWN &&
                          is_named_key(*key, NamedKey::DOWN)) ||
                         (current == InputConformanceTest::ARROW_LEFT &&
                          is_named_key(*key, NamedKey::LEFT)) ||
                         (current == InputConformanceTest::ARROW_RIGHT &&
                          is_named_key(*key, NamedKey::RIGHT)) ||
                         (current == InputConformanceTest::ESCAPE &&
                          is_named_key(*key, NamedKey::ESCAPE)) ||
                         (current == InputConformanceTest::CONTROL_KEY &&
                          is_character_key(*key, U'a') &&
                          key->modifiers.contains(Modifier::CONTROL)) ||
                         (current == InputConformanceTest::ALT_KEY &&
                          is_character_key(*key, U'n') &&
                          key->modifiers.contains(Modifier::ALT));
    if (matched) {
      pass("Trie emitted the expected normalized KeyEvent", now);
    }
    return;
  }
  if (const auto* mouse = std::get_if<MouseEvent>(&event)) {
    if (current == InputConformanceTest::MOUSE_CLICK &&
        mouse->button == MouseButton::LEFT &&
        mouse->action == MouseAction::PRESS && contains(mouse->position)) {
      pass("SGR mouse emitted a left-button press inside the test box", now);
      return;
    }
    if (current != InputConformanceTest::MOUSE_DRAG ||
        !contains(mouse->position)) {
      return;
    }
    if (mouse->button == MouseButton::LEFT &&
        mouse->action == MouseAction::PRESS) {
      drag_started_ = true;
      drag_moved_   = false;
      drag_origin_  = mouse->position;
      return;
    }
    if (drag_started_ && mouse->action == MouseAction::DRAG &&
        (distance(mouse->position.x, drag_origin_.x) >= 3U ||
         distance(mouse->position.y, drag_origin_.y) >= 3U)) {
      drag_moved_ = true;
      return;
    }
    if (drag_started_ && drag_moved_ && mouse->action == MouseAction::RELEASE) {
      pass("SGR mouse emitted press, drag, and release with coordinates", now);
    }
    return;
  }
  if (const auto* scroll = std::get_if<ScrollEvent>(&event)) {
    if (current != InputConformanceTest::MOUSE_SCROLL ||
        !contains(scroll->position)) {
      return;
    }
    saw_scroll_up_   = saw_scroll_up_ || scroll->delta_y > 0;
    saw_scroll_down_ = saw_scroll_down_ || scroll->delta_y < 0;
    if (saw_scroll_up_ && saw_scroll_down_) {
      pass("SGR mouse emitted wheel steps in both vertical directions", now);
    }
    return;
  }
  if (const auto* paste = std::get_if<PasteEvent>(&event)) {
    if (current != InputConformanceTest::CLIPBOARD_PASTE &&
        current != InputConformanceTest::FILE_DROP) {
      return;
    }
    switch (paste->phase) {
      case PastePhase::BEGIN:
        paste_started_ = true;
        paste_buffer_.clear();
        break;
      case PastePhase::DATA:
        observe_text(paste->data, true, now);
        break;
      case PastePhase::END:
        if (paste_started_) {
          if (current == InputConformanceTest::CLIPBOARD_PASTE &&
              paste_buffer_.contains(kClipboardToken)) {
            pass("Bracketed paste returned the exact displayed clipboard token",
                 now);
          } else if (current == InputConformanceTest::FILE_DROP &&
                     !paste_buffer_.empty()) {
            pass("File drop arrived as " +
                     std::to_string(paste_buffer_.size()) +
                     " bytes of bracketed-paste text",
                 now);
          }
        }
        break;
      case PastePhase::CANCEL:
        paste_started_ = false;
        paste_buffer_.clear();
        break;
    }
    return;
  }
  if (const auto* focus = std::get_if<FocusEvent>(&event)) {
    if (current != InputConformanceTest::FOCUS) {
      return;
    }
    if (!focus->focused) {
      focus_lost_ = true;
    } else if (focus_lost_) {
      pass("Focus reporting emitted loss followed by gain", now);
    }
  }
}

void InputConformanceRunner::tick(TimePoint now) {
  const std::lock_guard lock(mutex_);
  if (phase_ != InputConformancePhase::ACTIVE) {
    return;
  }
  if (seconds_remaining_ > 0U) {
    --seconds_remaining_;
  }
  if (seconds_remaining_ == 0U) {
    time_out(now);
  }
}

void InputConformanceRunner::update(TimePoint now) {
  const std::lock_guard lock(mutex_);
  if ((phase_ == InputConformancePhase::PASSED_FEEDBACK ||
       phase_ == InputConformancePhase::TIMED_OUT_FEEDBACK) &&
      now >= feedback_until_) {
    advance_locked();
  }
}

InputConformanceView InputConformanceRunner::view() const {
  const std::lock_guard lock(mutex_);
  const std::size_t count         = plan_size();
  const std::size_t display_index = std::min(current_index_, count - 1U);
  const InputConformanceTest test = planned_test(display_index);
  return InputConformanceView{
      .test              = test,
      .test_number       = std::min(current_index_ + 1U, count),
      .test_count        = count,
      .seconds_remaining = seconds_remaining_,
      .phase             = phase_,
      .name              = std::string{input_conformance_test_name(test)},
      .instruction       = std::string{input_conformance_instruction(test)},
      .last_observation  = last_observation_,
  };
}

std::vector<InputConformanceResult> InputConformanceRunner::results() const {
  const std::lock_guard lock(mutex_);
  return results_;
}

bool InputConformanceRunner::finished() const noexcept {
  const std::lock_guard lock(mutex_);
  return phase_ == InputConformancePhase::COMPLETE;
}

std::size_t InputConformanceRunner::plan_size() const noexcept {
  return selected_test_.has_value() ? 1U : kTests.size();
}

InputConformanceTest InputConformanceRunner::planned_test(
    std::size_t index) const noexcept {
  return selected_test_.has_value() ? *selected_test_ : test_at(index);
}

void InputConformanceRunner::pass(std::string detail, TimePoint now) {
  const InputConformanceTest test = planned_test(current_index_);
  results_.push_back(InputConformanceResult{
      .test        = test,
      .outcome     = InputConformanceOutcome::PASSED,
      .name        = std::string{input_conformance_test_name(test)},
      .instruction = std::string{input_conformance_instruction(test)},
      .detail      = std::move(detail),
  });
  phase_          = InputConformancePhase::PASSED_FEEDBACK;
  feedback_until_ = now + feedback_duration_;
}

void InputConformanceRunner::time_out(TimePoint now) {
  const InputConformanceTest test = planned_test(current_index_);
  results_.push_back(InputConformanceResult{
      .test        = test,
      .outcome     = InputConformanceOutcome::TIMED_OUT,
      .name        = std::string{input_conformance_test_name(test)},
      .instruction = std::string{input_conformance_instruction(test)},
      .detail =
          std::format("No matching decoded event arrived within {} heartbeats",
                      kTimeoutSeconds),
  });
  phase_          = InputConformancePhase::TIMED_OUT_FEEDBACK;
  feedback_until_ = now + feedback_duration_;
}

void InputConformanceRunner::advance_locked() noexcept {
  ++current_index_;
  if (current_index_ >= plan_size()) {
    phase_             = InputConformancePhase::COMPLETE;
    seconds_remaining_ = 0U;
    return;
  }
  seconds_remaining_ = kTimeoutSeconds;
  phase_             = InputConformancePhase::ACTIVE;
  text_buffer_.clear();
  saw_scroll_up_   = false;
  saw_scroll_down_ = false;
  drag_started_    = false;
  drag_moved_      = false;
  drag_origin_     = {};
  paste_started_   = false;
  paste_buffer_.clear();
  focus_lost_       = false;
  last_observation_ = "Waiting for input";
}

bool InputConformanceRunner::contains(CellPosition position) const noexcept {
  return position.x >= interaction_region_.x &&
         position.x - interaction_region_.x < interaction_region_.width &&
         position.y >= interaction_region_.y &&
         position.y - interaction_region_.y < interaction_region_.height;
}

void InputConformanceRunner::observe_text(std::string_view text, bool bracketed,
                                          TimePoint now) {
  const InputConformanceTest current = planned_test(current_index_);
  if (current == InputConformanceTest::TEXT) {
    const std::size_t available = kMaximumMatcherBytes - text_buffer_.size();
    text_buffer_.append(text.substr(0U, available));
    if (text_buffer_.contains("puc")) {
      pass("Trie root decoded ordinary UTF-8 text", now);
    }
    return;
  }
  if (current != InputConformanceTest::CLIPBOARD_PASTE &&
      current != InputConformanceTest::FILE_DROP) {
    return;
  }

  const std::size_t available = kMaximumMatcherBytes - paste_buffer_.size();
  paste_buffer_.append(text.substr(0U, available));
  if (!bracketed && current == InputConformanceTest::CLIPBOARD_PASTE &&
      paste_buffer_.contains(kClipboardToken)) {
    pass("Terminal paste returned the exact token as committed text", now);
  } else if (!bracketed && current == InputConformanceTest::FILE_DROP &&
             !paste_buffer_.empty()) {
    pass("File drop arrived as " + std::to_string(paste_buffer_.size()) +
             " bytes of ordinary committed text",
         now);
  }
}

}  // namespace puc::terminal
