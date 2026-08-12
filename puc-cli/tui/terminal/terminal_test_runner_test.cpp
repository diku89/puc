/**
 * @file terminal_test_runner_test.cpp
 * @brief Tests for the deterministic interactive input test state machine.
 */

#include "puc-cli/tui/terminal/terminal_test_runner.hpp"

#include <chrono>
#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>

#include "gtest/gtest.h"

namespace puc::terminal {
namespace {

using namespace std::chrono_literals;

/** Complete zero-duration feedback and select the next test. */
void advance(InputConformanceRunner& runner,
             InputConformanceRunner::TimePoint now) {
  runner.update(now);
}

/** Construct a normalized named-key press. */
Event named_key(NamedKey key) { return KeyEvent{.key = key}; }

TEST(InputConformanceRunnerTest, ExposesFourteenStableOperatorPrompts) {
  EXPECT_EQ(InputConformanceRunner::test_count(), 14U);
  EXPECT_EQ(InputConformanceRunner::kTimeoutSeconds, 15U);
  const std::span<const InputConformanceTestDescriptor> descriptors =
      input_conformance_tests();
  ASSERT_EQ(descriptors.size(), InputConformanceRunner::test_count());
  for (std::size_t index = 0U; index < descriptors.size(); ++index) {
    EXPECT_FALSE(descriptors[index].cli_name.empty());
    EXPECT_FALSE(descriptors[index].name.empty());
    EXPECT_FALSE(descriptors[index].instruction.empty());
    EXPECT_EQ(find_input_conformance_test(descriptors[index].cli_name),
              descriptors[index].test);
    EXPECT_EQ(input_conformance_test_cli_name(descriptors[index].test),
              descriptors[index].cli_name);
    for (std::size_t other = index + 1U; other < descriptors.size(); ++other) {
      EXPECT_NE(descriptors[index].cli_name, descriptors[other].cli_name);
    }
  }
  EXPECT_EQ(input_conformance_test_name(InputConformanceTest::TEXT),
            "Committed text");
  EXPECT_EQ(input_conformance_instruction(InputConformanceTest::CONTROL_KEY),
            "Press Ctrl-A");
  EXPECT_TRUE(
      input_conformance_instruction(InputConformanceTest::CLIPBOARD_PASTE)
          .contains(InputConformanceRunner::kClipboardToken));
  EXPECT_EQ(input_conformance_outcome_name(InputConformanceOutcome::PASSED),
            "PASS");
  EXPECT_EQ(input_conformance_outcome_name(InputConformanceOutcome::TIMED_OUT),
            "TIMEOUT");
  EXPECT_EQ(input_conformance_test_name(static_cast<InputConformanceTest>(-1)),
            "Unknown test");
  EXPECT_EQ(
      input_conformance_instruction(static_cast<InputConformanceTest>(-1)),
      "Unknown test");
  EXPECT_EQ(
      input_conformance_test_cli_name(static_cast<InputConformanceTest>(-1)),
      "unknown");
  EXPECT_EQ(find_input_conformance_test("not-a-test"), std::nullopt);
}

TEST(InputConformanceRunnerTest, SelectedTestCreatesAOneItemPlan) {
  InputConformanceRunner runner(0ms, InputConformanceTest::CLIPBOARD_PASTE);
  const InputConformanceRunner::TimePoint now{};

  const InputConformanceView initial = runner.view();
  EXPECT_EQ(runner.plan_size(), 1U);
  EXPECT_EQ(initial.test_number, 1U);
  EXPECT_EQ(initial.test_count, 1U);
  EXPECT_EQ(initial.name, "Clipboard paste");

  runner.observe(PasteEvent{.phase = PastePhase::BEGIN}, now);
  runner.observe(
      PasteEvent{
          .phase = PastePhase::DATA,
          .data  = std::string{InputConformanceRunner::kClipboardToken},
      },
      now);
  runner.observe(PasteEvent{.phase = PastePhase::END}, now);
  ASSERT_EQ(runner.view().phase, InputConformancePhase::PASSED_FEEDBACK);
  advance(runner, now);

  EXPECT_TRUE(runner.finished());
  const std::vector<InputConformanceResult> results = runner.results();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().test, InputConformanceTest::CLIPBOARD_PASTE);
  EXPECT_EQ(results.front().outcome, InputConformanceOutcome::PASSED);
}

TEST(InputConformanceRunnerTest, PassesTheCompletePlanFromNormalizedEvents) {
  InputConformanceRunner runner(0ms);
  const InputConformanceRunner::TimePoint now{};
  runner.set_interaction_region(
      InputInteractionRegion{.x = 10U, .y = 5U, .width = 30U, .height = 10U});

  runner.observe(TextEvent{.utf8 = "pu"}, now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(TextEvent{.utf8 = "c"}, now);
  advance(runner, now);

  runner.observe(named_key(NamedKey::UP), now);
  advance(runner, now);
  runner.observe(named_key(NamedKey::DOWN), now);
  advance(runner, now);
  runner.observe(named_key(NamedKey::LEFT), now);
  advance(runner, now);
  runner.observe(named_key(NamedKey::RIGHT), now);
  advance(runner, now);

  runner.observe(named_key(NamedKey::ESCAPE), now);
  advance(runner, now);

  runner.observe(
      KeyEvent{
          .key       = U'a',
          .modifiers = Modifier::CONTROL,
      },
      now);
  advance(runner, now);
  runner.observe(
      KeyEvent{
          .key       = U'n',
          .modifiers = Modifier::ALT,
      },
      now);
  advance(runner, now);

  runner.observe(
      MouseEvent{
          .position = {.x = 12U, .y = 7U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::PRESS,
      },
      now);
  advance(runner, now);

  runner.observe(
      ScrollEvent{
          .position = {.x = 12U, .y = 7U},
          .delta_y  = 1,
      },
      now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(
      ScrollEvent{
          .position = {.x = 12U, .y = 7U},
          .delta_y  = -1,
      },
      now);
  advance(runner, now);

  runner.observe(
      MouseEvent{
          .position = {.x = 12U, .y = 7U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::PRESS,
      },
      now);
  runner.observe(
      MouseEvent{
          .position = {.x = 17U, .y = 7U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::DRAG,
      },
      now);
  runner.observe(
      MouseEvent{
          .position = {.x = 17U, .y = 7U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::RELEASE,
      },
      now);
  advance(runner, now);

  runner.observe(PasteEvent{.phase = PastePhase::BEGIN}, now);
  runner.observe(
      PasteEvent{
          .phase = PastePhase::DATA,
          .data  = std::string{InputConformanceRunner::kClipboardToken},
      },
      now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(PasteEvent{.phase = PastePhase::END}, now);
  advance(runner, now);

  runner.observe(PasteEvent{.phase = PastePhase::BEGIN}, now);
  runner.observe(
      PasteEvent{
          .phase = PastePhase::DATA,
          .data  = "/tmp/dropped file.txt",
      },
      now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(PasteEvent{.phase = PastePhase::END}, now);
  advance(runner, now);

  runner.observe(FocusEvent{.focused = false}, now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(FocusEvent{.focused = true}, now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::PASSED_FEEDBACK);
  advance(runner, now);

  EXPECT_TRUE(runner.finished());
  const std::vector<InputConformanceResult> results = runner.results();
  ASSERT_EQ(results.size(), InputConformanceRunner::test_count());
  for (const InputConformanceResult& result : results) {
    EXPECT_EQ(result.outcome, InputConformanceOutcome::PASSED);
    EXPECT_FALSE(result.detail.empty());
  }
}

TEST(InputConformanceRunnerTest, AcceptsOrdinaryTextAsFileDropFallback) {
  InputConformanceRunner runner(0ms);
  const InputConformanceRunner::TimePoint now{};

  for (std::size_t test = 0U; test < 12U; ++test) {
    for (unsigned int tick = 0U; tick < InputConformanceRunner::kTimeoutSeconds;
         ++tick) {
      runner.tick(now);
    }
    runner.update(now);
  }
  ASSERT_EQ(runner.view().name, "File-drop fallback");

  runner.observe(TextEvent{.utf8 = "/tmp/example.txt"}, now);

  ASSERT_EQ(runner.view().phase, InputConformancePhase::PASSED_FEEDBACK);
  const std::vector<InputConformanceResult> results = runner.results();
  ASSERT_EQ(results.size(), 13U);
  EXPECT_EQ(results.back().test, InputConformanceTest::FILE_DROP);
  EXPECT_EQ(results.back().outcome, InputConformanceOutcome::PASSED);
  EXPECT_TRUE(results.back().detail.contains("ordinary committed text"));
}

TEST(InputConformanceRunnerTest, ConfiguredHeartbeatBudgetTimesOutCurrentTest) {
  InputConformanceRunner runner(0ms);
  const InputConformanceRunner::TimePoint now{};

  for (unsigned int tick = 1U; tick < InputConformanceRunner::kTimeoutSeconds;
       ++tick) {
    runner.tick(now);
    EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
    EXPECT_EQ(runner.view().seconds_remaining,
              InputConformanceRunner::kTimeoutSeconds - tick);
  }
  runner.tick(now);

  const InputConformanceView timed_out = runner.view();
  EXPECT_EQ(timed_out.phase, InputConformancePhase::TIMED_OUT_FEEDBACK);
  EXPECT_EQ(timed_out.seconds_remaining, 0U);
  const std::vector<InputConformanceResult> results = runner.results();
  ASSERT_EQ(results.size(), 1U);
  EXPECT_EQ(results.front().test, InputConformanceTest::TEXT);
  EXPECT_EQ(results.front().outcome, InputConformanceOutcome::TIMED_OUT);
  EXPECT_EQ(results.front().detail,
            "No matching decoded event arrived within 15 heartbeats");

  advance(runner, now);
  const InputConformanceView next = runner.view();
  EXPECT_EQ(next.test_number, 2U);
  EXPECT_EQ(next.seconds_remaining, InputConformanceRunner::kTimeoutSeconds);
  EXPECT_EQ(next.phase, InputConformancePhase::ACTIVE);
}

TEST(InputConformanceRunnerTest, FeedbackIgnoresTicksAndInputUntilUpdate) {
  InputConformanceRunner runner(250ms);
  const InputConformanceRunner::TimePoint start{};
  runner.observe(TextEvent{.utf8 = "puc"}, start);
  ASSERT_EQ(runner.view().phase, InputConformancePhase::PASSED_FEEDBACK);

  runner.tick(start + 100ms);
  runner.observe(named_key(NamedKey::UP), start + 100ms);
  runner.update(start + 249ms);
  EXPECT_EQ(runner.view().test_number, 1U);
  EXPECT_EQ(runner.results().size(), 1U);

  runner.update(start + 250ms);
  EXPECT_EQ(runner.view().test_number, 2U);
  EXPECT_EQ(runner.view().seconds_remaining,
            InputConformanceRunner::kTimeoutSeconds);
}

TEST(InputConformanceRunnerTest, MouseChecksRequireTheRenderedBoxRegion) {
  InputConformanceRunner runner(0ms);
  const InputConformanceRunner::TimePoint now{};
  runner.set_interaction_region(
      InputInteractionRegion{.x = 10U, .y = 5U, .width = 5U, .height = 5U});

  runner.observe(TextEvent{.utf8 = "puc"}, now);
  advance(runner, now);
  for (const NamedKey key :
       {NamedKey::UP, NamedKey::DOWN, NamedKey::LEFT, NamedKey::RIGHT}) {
    runner.observe(named_key(key), now);
    advance(runner, now);
  }
  runner.observe(named_key(NamedKey::ESCAPE), now);
  advance(runner, now);
  runner.observe(KeyEvent{.key = U'a', .modifiers = Modifier::CONTROL}, now);
  advance(runner, now);
  runner.observe(KeyEvent{.key = U'n', .modifiers = Modifier::ALT}, now);
  advance(runner, now);

  ASSERT_EQ(runner.view().test_number, 9U);
  runner.observe(
      MouseEvent{
          .position = {.x = 15U, .y = 5U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::PRESS,
      },
      now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::ACTIVE);
  runner.observe(
      MouseEvent{
          .position = {.x = 14U, .y = 9U},
          .button   = MouseButton::LEFT,
          .action   = MouseAction::PRESS,
      },
      now);
  EXPECT_EQ(runner.view().phase, InputConformancePhase::PASSED_FEEDBACK);
}

TEST(InputConformanceRunnerTest, CompleteViewRetainsTheFinalTestIdentity) {
  InputConformanceRunner runner(0ms);
  const InputConformanceRunner::TimePoint now{};
  for (std::size_t test = 0U; test < InputConformanceRunner::test_count();
       ++test) {
    for (unsigned int tick = 0U; tick < InputConformanceRunner::kTimeoutSeconds;
         ++tick) {
      runner.tick(now);
    }
    runner.update(now);
  }

  ASSERT_TRUE(runner.finished());
  const InputConformanceView view = runner.view();
  EXPECT_EQ(view.phase, InputConformancePhase::COMPLETE);
  EXPECT_EQ(view.test_number, InputConformanceRunner::test_count());
  EXPECT_EQ(view.test_count, InputConformanceRunner::test_count());
  ASSERT_EQ(runner.results().size(), InputConformanceRunner::test_count());
}

}  // namespace
}  // namespace puc::terminal
