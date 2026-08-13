/**
 * @file metronome.cpp
 * @brief One-hertz NullMessage scheduling and channel lifecycle.
 */

#include "utils/metronome/metronome.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "msgs/null_message.hpp"
#include "utils/ipc/directory.hpp"
#include "utils/ipc/smem_channel.hpp"
#include "utils/logger/logger.hpp"
#include "utils/timer/scheduler.hpp"

/** @cond METRONOME_LOGGER_MODULE */
LOGGER_MODULE("Metronome");
/** @endcond */

namespace puc::metronome {
namespace {

/** Duration between one-hertz ticks. */
constexpr std::chrono::seconds kOneSecond{1};

/** Heartbeats are state-like: retain only the newest pending tick. */
constexpr std::size_t kHeartbeatChannelDepth = 1U;

/** A NullMessage has no bytes, but Channel requires a positive byte limit. */
constexpr std::size_t kHeartbeatMaximumPayloadBytes = 1U;

/** Shared state that safely outlives a concurrently finishing tick. */
struct TickState {
  std::atomic<bool> running = true; /**< Whether publishing remains desired. */
  std::shared_ptr<ipc::Channel> channel; /**< Registered heartbeat endpoint. */
  std::vector<std::uint8_t> payload;     /**< Encoded NullMessage bytes. */
};

/** Publish one heartbeat and treat a live-route failure as unrecoverable. */
void publish_tick(const std::shared_ptr<TickState>& state) noexcept {
  if (!state->running.load(std::memory_order_acquire)) {
    return;
  }
  const ipc::TransferResult transfer = state->channel->transmit(
      ipc::Channel::Bytes{state->payload.data(), state->payload.size()});
  if (ipc::is_ok(transfer.status) && transfer.bytes == state->payload.size()) {
    return;
  }
  if (!state->running.load(std::memory_order_acquire)) {
    return;
  }
  Logger<ERROR> << "Could not publish heartbeat on '" << kOneHertzChannel
                << "': " << ipc::status_message(transfer.status);
  std::terminate();
}

}  // namespace

/** Synchronized channel, job, and cancellation ownership. */
class Metronome::Impl {
 public:
  /** Borrow dependencies that outlive this implementation. */
  Impl(ipc::Directory& configured_directory,
       timer::Scheduler& configured_scheduler) noexcept
      : directory(configured_directory), scheduler(configured_scheduler) {}

  /** Stop any active publication before borrowed dependencies disappear. */
  ~Impl() { stop(); }

  /** Register the channel and schedule the first tick transactionally. */
  Status start() {
    const std::lock_guard lock(mutex);
    if (state != nullptr) {
      return Status::OK;
    }

    std::vector<std::uint8_t> payload;
    const msg::Status encoding =
        msg::NullMessageCodec{}.serialize(msg::NullMessage{}, payload);
    if (!msg::is_ok(encoding)) {
      Logger<ERROR> << "Could not encode metronome NullMessage: "
                    << msg::status_message(encoding);
      return Status::MESSAGE_ENCODING_FAILED;
    }

    auto next_channel = std::make_shared<ipc::SmemChannel>(
        std::string{kOneHertzChannel}, kHeartbeatMaximumPayloadBytes,
        ipc::ChannelOptions{.channel_max_depth = kHeartbeatChannelDepth});
    ipc::ChannelId channel_id = 0U;
    const ipc::Status channel_status =
        directory.open_channel(next_channel, channel_id);
    if (!ipc::is_ok(channel_status)) {
      Logger<ERROR> << "Could not register metronome channel '"
                    << kOneHertzChannel
                    << "': " << ipc::status_message(channel_status);
      return Status::CHANNEL_SETUP_FAILED;
    }

    auto next_state     = std::make_shared<TickState>();
    next_state->channel = next_channel;
    next_state->payload = std::move(payload);
    timer::PeriodicHandle next_handle;
    const timer::Status scheduling = scheduler.every(
        kOneSecond, [next_state]() noexcept { publish_tick(next_state); },
        next_handle);
    if (!timer::is_ok(scheduling)) {
      next_state->running.store(false, std::memory_order_release);
      const ipc::Status close_status =
          directory.close_channel(kOneHertzChannel);
      if (!ipc::is_ok(close_status)) {
        Logger<ERROR> << "Could not roll back metronome channel: "
                      << ipc::status_message(close_status);
      }
      Logger<ERROR> << "Could not schedule metronome: "
                    << timer::status_message(scheduling);
      return Status::SCHEDULING_FAILED;
    }

    state    = std::move(next_state);
    channel  = std::move(next_channel);
    schedule = std::move(next_handle);
    Logger<INFO> << "Started one-hertz heartbeat on channel " << channel_id;
    return Status::OK;
  }

  /** Cancel periodic work before detaching its asynchronous channel. */
  void stop() noexcept {
    const std::lock_guard lock(mutex);
    if (state == nullptr) {
      return;
    }
    state->running.store(false, std::memory_order_release);
    schedule.cancel();
    const ipc::Status status = directory.close_channel(kOneHertzChannel);
    if (!ipc::is_ok(status) && status != ipc::Status::CHANNEL_NOT_FOUND) {
      Logger<ERROR> << "Could not remove metronome channel: "
                    << ipc::status_message(status);
    }
    channel.reset();
    state.reset();
    Logger<INFO> << "Stopped one-hertz heartbeat";
  }

  /** Inspect periodic ownership under the lifecycle lock. */
  bool running() const noexcept {
    const std::lock_guard lock(mutex);
    return state != nullptr && schedule.active();
  }

  ipc::Directory& directory;   /**< Borrowed named-channel registry. */
  timer::Scheduler& scheduler; /**< Borrowed periodic scheduler. */
  mutable std::mutex mutex;    /**< Serializes start, stop, and inspection. */
  std::shared_ptr<TickState> state; /**< Current in-flight-safe tick state. */
  std::shared_ptr<ipc::Channel> channel; /**< Owned registered endpoint. */
  timer::PeriodicHandle schedule;        /**< Periodic cancellation. */
};

Metronome::Metronome(ipc::Directory& directory, timer::Scheduler& scheduler)
    : impl_(std::make_unique<Impl>(directory, scheduler)) {}

Metronome::~Metronome() = default;

Status Metronome::start() { return impl_->start(); }

void Metronome::stop() noexcept { impl_->stop(); }

bool Metronome::running() const noexcept { return impl_->running(); }

}  // namespace puc::metronome
