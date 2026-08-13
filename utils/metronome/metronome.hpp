#pragma once

/**
 * @file metronome.hpp
 * @brief Process-local one-hertz heartbeat publication.
 */

#include <memory>
#include <string_view>

#include "utils/metronome/status.hpp"

namespace puc::ipc {
class Directory;
}

namespace puc::timer {
class Scheduler;
}

namespace puc::metronome {

/**
 * Canonical channel carrying one empty heartbeat payload per second.
 *
 * \channel{//metronome/1hz||Publishes a process-local one-hertz heartbeat as
 * puc::msg::NullMessage. Only the newest pending heartbeat is retained, so a
 * delayed consumer never receives a burst of stale clock ticks.||
 * puc::metronome::Metronome.||TUI timers and other elapsed-time consumers.}
 */
inline constexpr std::string_view kOneHertzChannel = "//metronome/1hz";

/**
 * Publish NullMessage heartbeats using a caller-owned Directory and Scheduler.
 *
 * `start()` registers `kOneHertzChannel` and schedules its first tick one
 * second later. `stop()` cancels only this periodic job and removes its
 * channel; it never shuts down the shared timer. The Directory and Scheduler
 * must outlive this object and remain active until it is stopped or destroyed.
 */
class Metronome {
 public:
  /** Borrow the routing directory and timer scheduler used by this publisher.
   */
  Metronome(ipc::Directory& directory, timer::Scheduler& scheduler);

  Metronome(const Metronome&)            = delete;
  Metronome& operator=(const Metronome&) = delete;
  Metronome(Metronome&&)                 = delete;
  Metronome& operator=(Metronome&&)      = delete;

  /** Cancel publication and remove the channel before releasing state. */
  ~Metronome();

  /** Register the heartbeat channel and begin periodic publication. */
  Status start();

  /** Stop publication and remove the channel; safe to call repeatedly. */
  void stop() noexcept;

  /** Return whether this object currently owns an active periodic job. */
  bool running() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Synchronized publisher implementation. */
};

}  // namespace puc::metronome
