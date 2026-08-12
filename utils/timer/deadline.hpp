#pragma once

/**
 * @file deadline.hpp
 * @brief Clock-generic one-shot and generation-token deadline primitives.
 */

#include <chrono>
#include <optional>
#include <utility>

namespace puc::timer {

/**
 * Track one one-shot deadline against an injectable monotonic clock.
 *
 * Arming replaces any previous deadline. `take_if_due()` consumes an elapsed
 * deadline exactly once, which makes the type suitable for polling loops.
 */
template <typename Clock = std::chrono::steady_clock>
class Deadline {
 public:
  using time_point = typename Clock::time_point;
  using duration   = typename Clock::duration;

  /** Arm the deadline relative to `now`. */
  void arm(duration delay, time_point now = Clock::now()) noexcept {
    deadline_ = now + delay;
  }

  /** Cancel any armed deadline. */
  void cancel() noexcept { deadline_.reset(); }

  /** Return whether a deadline is currently armed. */
  bool pending() const noexcept { return deadline_.has_value(); }

  /** Return the absolute deadline, when armed. */
  std::optional<time_point> time() const noexcept { return deadline_; }

  /** Return whether the deadline has elapsed without consuming it. */
  bool due(time_point now = Clock::now()) const noexcept {
    return deadline_.has_value() && now >= *deadline_;
  }

  /** Consume an elapsed deadline and report whether it was due. */
  bool take_if_due(time_point now = Clock::now()) noexcept {
    if (!due(now)) {
      return false;
    }
    deadline_.reset();
    return true;
  }

 private:
  std::optional<time_point> deadline_; /**< Armed absolute deadline. */
};

/**
 * Associate a restartable producer generation with its one-shot deadline.
 *
 * `synchronize()` preserves an existing deadline while the token is unchanged,
 * replaces it for a new generation, and cancels it when the producer has no
 * pending timeout. This is the common shape used by incremental decoders and
 * UI state machines.
 */
template <typename Token, typename Clock = std::chrono::steady_clock>
class TokenDeadline {
 public:
  using time_point = typename Clock::time_point;
  using duration   = typename Clock::duration;

  /** Synchronize this deadline with the producer's current token. */
  void synchronize(const std::optional<Token>& token, duration delay,
                   time_point now = Clock::now()) {
    if (!token.has_value()) {
      cancel();
      return;
    }
    if (!token_.has_value() || *token_ != *token) {
      token_ = *token;
      deadline_.arm(delay, now);
    }
  }

  /** Cancel both the token and its deadline. */
  void cancel() noexcept {
    token_.reset();
    deadline_.cancel();
  }

  /** Return whether a producer generation is pending. */
  bool pending() const noexcept { return token_.has_value(); }

  /** Return the current producer token, when pending. */
  const std::optional<Token>& token() const noexcept { return token_; }

  /**
   * Consume and return the token when its deadline has elapsed.
   *
   * The token is cleared before it is returned, so a producer may immediately
   * synchronize a successor generation after handling it.
   */
  std::optional<Token> take_if_due(time_point now = Clock::now()) {
    if (!token_.has_value() || !deadline_.take_if_due(now)) {
      return std::nullopt;
    }
    std::optional<Token> due = std::move(token_);
    token_.reset();
    return due;
  }

 private:
  std::optional<Token> token_; /**< Current producer generation. */
  Deadline<Clock> deadline_;   /**< Deadline for that generation. */
};

}  // namespace puc::timer
