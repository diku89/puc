/**
 * @file timer_test.cpp
 * @brief Unit tests for deadlines, polling, and worker-backed scheduling.
 */

#include "utils/timer/timer.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>

#include "gtest/gtest.h"
#include "utils/multithreading/job_queue.hpp"

namespace puc::timer {
namespace {

struct ManualClock {
  using duration   = std::chrono::milliseconds;
  using rep        = duration::rep;
  using period     = duration::period;
  using time_point = std::chrono::time_point<ManualClock>;
  static time_point now() noexcept { return current; }
  static inline time_point current{};
};

TEST(DeadlineTest, ConsumesOneArmedDeadlineExactlyOnce) {
  Deadline<ManualClock> deadline;
  const auto beginning = ManualClock::time_point{};
  deadline.arm(std::chrono::milliseconds{10}, beginning);

  EXPECT_FALSE(deadline.take_if_due(beginning + std::chrono::milliseconds{9}));
  EXPECT_TRUE(deadline.take_if_due(beginning + std::chrono::milliseconds{10}));
  EXPECT_FALSE(deadline.take_if_due(beginning + std::chrono::milliseconds{11}));
}

TEST(DeadlineTest, TokenSynchronizationPreservesAndReplacesGenerations) {
  TokenDeadline<std::uint64_t, ManualClock> deadline;
  const auto beginning = ManualClock::time_point{};
  deadline.synchronize(7U, std::chrono::milliseconds{10}, beginning);
  deadline.synchronize(7U, std::chrono::milliseconds{10},
                       beginning + std::chrono::milliseconds{8});
  EXPECT_EQ(deadline.take_if_due(beginning + std::chrono::milliseconds{10}),
            7U);

  deadline.synchronize(8U, std::chrono::milliseconds{10}, beginning);
  deadline.synchronize(9U, std::chrono::milliseconds{10},
                       beginning + std::chrono::milliseconds{5});
  EXPECT_FALSE(deadline.take_if_due(beginning + std::chrono::milliseconds{10})
                   .has_value());
  EXPECT_EQ(deadline.take_if_due(beginning + std::chrono::milliseconds{15}),
            9U);
}

TEST(PollerTest, DistinguishesTimeoutReadableAndClosedDescriptors) {
  int descriptors[2]{};
  ASSERT_EQ(::pipe(descriptors), 0);
  PollResult result =
      poll_readable(descriptors[0], std::chrono::milliseconds{0});
  EXPECT_EQ(result.status, Status::TIMED_OUT);
  EXPECT_FALSE(result.readable);

  result = poll_writable(descriptors[1], std::chrono::milliseconds{0});
  EXPECT_EQ(result.status, Status::OK);
  EXPECT_TRUE(result.writable);
  EXPECT_TRUE(result);

  const char byte = 'x';
  ASSERT_EQ(::write(descriptors[1], &byte, 1U), 1);
  result = poll_readable(descriptors[0], std::chrono::milliseconds{0});
  EXPECT_EQ(result.status, Status::OK);
  EXPECT_TRUE(result.readable);
  char received = 0;
  ASSERT_EQ(::read(descriptors[0], &received, 1U), 1);
  EXPECT_EQ(received, byte);

  ASSERT_EQ(::close(descriptors[1]), 0);
  result = poll_readable(descriptors[0], std::chrono::milliseconds{0});
  EXPECT_TRUE(result.status == Status::CLOSED || result.status == Status::OK);
  EXPECT_TRUE(result.hangup);
  ASSERT_EQ(::close(descriptors[0]), 0);
}

TEST(SchedulerTest, RunsAndCancelsPeriodicCallbacks) {
  multithreading::JobQueue workers{1U};
  Scheduler scheduler{workers};
  std::atomic<std::size_t> calls = 0U;
  PeriodicHandle handle;
  ASSERT_EQ(scheduler.every(
                std::chrono::milliseconds{1},
                [&calls]() noexcept { calls.fetch_add(1U); }, handle),
            Status::OK);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{1};
  while (calls.load() == 0U && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_GT(calls.load(), 0U);
  handle.cancel();
  EXPECT_FALSE(handle.active());
  workers.wait();
}

}  // namespace
}  // namespace puc::timer
