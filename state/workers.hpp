#pragma once

/**
 * @file workers.hpp
 * @brief Application lifecycle adapter for the shared worker pool.
 */

#include <cstdint>
#include <memory>

#include "state/state.hpp"

namespace puc::multithreading {
class JobQueue;
}

namespace puc::app {

/**
 * Own the application-wide JobQueue for one running lifecycle generation.
 *
 * The queue is created by start() and synchronously joined by stop(). This
 * keeps initialization free of worker activity and allows an AppState that was
 * stopped to start again with a fresh queue. Dependent adapters may borrow the
 * returned pointer only while AppState is running. LoggerSubsystem precedes
 * worker creation and remains installed until every worker has joined.
 */
class WorkerSubsystem final : public AppSubsystem {
 public:
  /** Construct an adapter that starts `worker_count` worker threads. */
  explicit WorkerSubsystem(std::uint8_t worker_count = 4U);

  /** Destroy a released worker pool. */
  ~WorkerSubsystem() override;

  /** Validate the configured worker count without starting threads. */
  Status initialize(AppState& app) override;

  /** Construct and start a fresh JobQueue. */
  Status start(AppState& app) override;

  /** Shut down, join, and release the active JobQueue. */
  Status stop(AppState& app) noexcept override;

  /** Release any queue retained after partial lifecycle progress. */
  Status terminate(AppState& app) noexcept override;

  /** Return the running worker pool, or nullptr outside the running phase. */
  multithreading::JobQueue* workers() noexcept { return workers_.get(); }

  /** Return the running worker pool, or nullptr outside the running phase. */
  const multithreading::JobQueue* workers() const noexcept {
    return workers_.get();
  }

  /** Return the configured number of worker threads. */
  std::uint8_t configured_worker_count() const noexcept {
    return worker_count_;
  }

 private:
  /** Join and release the active queue; safe to call repeatedly. */
  void release_workers() noexcept;

  std::uint8_t worker_count_; /**< Fixed thread count for every generation. */
  std::unique_ptr<multithreading::JobQueue>
      workers_; /**< Pool present only while this adapter is started. */
};

}  // namespace puc::app
