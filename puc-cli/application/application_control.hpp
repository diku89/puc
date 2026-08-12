#pragma once

/**
 * @file application_control.hpp
 * @brief Thread-safe deferred application-exit requests.
 */

#include <atomic>

namespace puc::app {

/**
 * Carry a durable exit request without re-entering AppState lifecycle hooks.
 *
 * Commands and other event handlers may request exit while they are executing
 * inside a running subsystem. The outer application loop observes the request
 * after the handler returns and performs the ordinary stop/terminate sequence.
 * A request remains set across suspend-style stop/start cycles.
 */
class ApplicationControl final {
 public:
  /** Record a process-exit request; safe to call repeatedly. */
  void request_exit() noexcept {
    exit_requested_.store(true, std::memory_order_release);
  }

  /** Return whether any component has requested final application exit. */
  bool exit_requested() const noexcept {
    return exit_requested_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> exit_requested_ = false; /**< Durable one-way request. */
};

}  // namespace puc::app
