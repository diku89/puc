/**
 * @file metronome_test.cpp
 * @brief Tests for cancellable one-hertz NullMessage publication.
 */

#include "utils/metronome/metronome.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "gtest/gtest.h"
#include "msgs/null_message.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/multithreading/job_queue.hpp"
#include "utils/timer/scheduler.hpp"

namespace puc::metronome {
namespace {

using namespace std::chrono_literals;

TEST(StatusTest, ReportsStableHumanReadableResults) {
  EXPECT_TRUE(is_ok(Status::OK));
  EXPECT_FALSE(is_ok(Status::CHANNEL_SETUP_FAILED));
  EXPECT_FALSE(is_ok(Status::MESSAGE_ENCODING_FAILED));
  EXPECT_FALSE(is_ok(Status::SCHEDULING_FAILED));
  EXPECT_EQ(status_message(Status::OK), "success");
  EXPECT_EQ(status_message(Status::CHANNEL_SETUP_FAILED),
            "metronome channel could not be registered");
  EXPECT_EQ(status_message(Status::MESSAGE_ENCODING_FAILED),
            "metronome NullMessage could not be encoded");
  EXPECT_EQ(status_message(Status::SCHEDULING_FAILED),
            "metronome periodic job could not be scheduled");
  EXPECT_EQ(status_message(static_cast<Status>(-1)),
            "unknown metronome status");
}

TEST(MetronomeTest, RegistersAndRemovesTheCanonicalChannel) {
  multithreading::JobQueue workers(2U);
  ipc::Directory directory(workers);
  timer::Scheduler scheduler(workers);
  Metronome metronome(directory, scheduler);

  EXPECT_EQ(directory.get_channel(kOneHertzChannel), nullptr);
  ASSERT_EQ(metronome.start(), Status::OK);
  EXPECT_TRUE(metronome.running());
  EXPECT_NE(directory.get_channel(kOneHertzChannel), nullptr);
  EXPECT_EQ(metronome.start(), Status::OK);
  EXPECT_EQ(directory.size(), 1U);

  metronome.stop();
  EXPECT_FALSE(metronome.running());
  EXPECT_EQ(directory.get_channel(kOneHertzChannel), nullptr);
  metronome.stop();
  workers.wait();
}

TEST(MetronomeTest, PublishesAValidNullMessageAfterOneSecond) {
  multithreading::JobQueue workers(2U);
  ipc::Directory directory(workers);
  timer::Scheduler scheduler(workers);
  Metronome metronome(directory, scheduler);
  ASSERT_EQ(metronome.start(), Status::OK);

  std::mutex mutex;
  std::condition_variable changed;
  std::size_t ticks = 0U;
  bool decoded      = false;
  ipc::Subscription subscription;
  ASSERT_EQ(directory.subscribe(
                kOneHertzChannel,
                [&](ipc::Channel::Bytes payload) noexcept {
                  msg::NullMessage message;
                  const bool valid = msg::is_ok(
                      msg::NullMessageCodec{}.deserialize(payload, message));
                  {
                    const std::lock_guard lock(mutex);
                    ++ticks;
                    decoded = decoded || valid;
                  }
                  changed.notify_all();
                },
                subscription),
            ipc::Status::OK);

  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return ticks >= 1U; }));
    EXPECT_TRUE(decoded);
  }

  metronome.stop();
  subscription.reset();
  workers.wait();
}

TEST(MetronomeTest, ReportsAConflictingChannelWithoutTakingOwnership) {
  multithreading::JobQueue workers;
  ipc::Directory directory(workers);
  timer::Scheduler scheduler(workers);
  auto existing =
      std::make_shared<ipc::SmemChannel>(std::string{kOneHertzChannel}, 1U);
  ipc::ChannelId channel_id = 0U;
  ASSERT_EQ(directory.open_channel(existing, channel_id), ipc::Status::OK);

  Metronome metronome(directory, scheduler);
  EXPECT_EQ(metronome.start(), Status::CHANNEL_SETUP_FAILED);
  EXPECT_FALSE(metronome.running());
  EXPECT_EQ(directory.get_channel(kOneHertzChannel), existing);
  workers.wait();
}

TEST(MetronomeTest, DestructionCancelsTheJobAndRemovesTheChannel) {
  multithreading::JobQueue workers;
  ipc::Directory directory(workers);
  timer::Scheduler scheduler(workers);
  {
    Metronome metronome(directory, scheduler);
    ASSERT_EQ(metronome.start(), Status::OK);
    ASSERT_NE(directory.get_channel(kOneHertzChannel), nullptr);
  }
  EXPECT_EQ(directory.get_channel(kOneHertzChannel), nullptr);
  workers.wait();
}

}  // namespace
}  // namespace puc::metronome
