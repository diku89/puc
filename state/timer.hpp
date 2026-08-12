#pragma once

/**
 * @file timer.hpp
 * @brief Lifecycle adapter for timed work bound to a worker generation.
 */

#include <memory>

#include "state/state.hpp"

namespace puc::timer {
class Scheduler;
}

namespace puc::app {

/**
 * Own the timer scheduler facade for each running worker-pool generation.
 *
 * Pure deadline and polling utilities have no lifecycle state. Scheduled work
 * does borrow live workers, so this adapter recreates its Scheduler on every
 * start and releases it before WorkerSubsystem stops.
 */
class TimerSubsystem final : public AppSubsystem {
 public:
  /** Declare the worker-generation dependency. */
  TimerSubsystem();

  /** Destroy a scheduler already released by stop(). */
  ~TimerSubsystem() override;

  /** Validate that WorkerSubsystem is registered. */
  Status initialize(AppState& app) override;

  /** Bind a fresh Scheduler to the current worker generation. */
  Status start(AppState& app) override;

  /** Release the Scheduler before workers are stopped. */
  Status stop(AppState& app) noexcept override;

  /** Release any Scheduler retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the running timer scheduler, or nullptr while stopped. */
  timer::Scheduler* scheduler() noexcept { return scheduler_.get(); }

  /** Return the running timer scheduler, or nullptr while stopped. */
  const timer::Scheduler* scheduler() const noexcept {
    return scheduler_.get();
  }

 private:
  std::unique_ptr<timer::Scheduler>
      scheduler_; /**< Facade over the current worker generation. */
};

}  // namespace puc::app
