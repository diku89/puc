#pragma once

/**
 * @file turn_tree.hpp
 * @brief In-memory reply topology over the reusable Trie.
 */

#include <atomic>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "canvas/protos/turn.pb.h"
#include "canvas/turn/address.hpp"
#include "utils/containers/trie.hpp"
#include "utils/hash/sha256.hpp"

namespace puc::canvas {

/** Runtime Turn state stored directly in each authoritative Trie node. */
class TurnNode final {
 public:
  /** Construct an empty prefix or synthetic-root node. */
  TurnNode() = default;

  /** Construct a committed Turn node with its first reply available. */
  explicit TurnNode(proto::Turn value) : turn_(std::move(value)) {}

  /** Copy protobuf and allocator state for transactional Trie replacement. */
  TurnNode(const TurnNode& other)
      : turn_(other.turn_),
        next_numeric_child_(
            other.next_numeric_child_.load(std::memory_order_relaxed)),
        next_alphabetic_child_(
            other.next_alphabetic_child_.load(std::memory_order_relaxed)) {}

  /** Copy protobuf and allocator state for transactional Trie replacement. */
  TurnNode& operator=(const TurnNode& other) {
    if (this == &other) {
      return *this;
    }
    turn_ = other.turn_;
    next_numeric_child_.store(
        other.next_numeric_child_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    next_alphabetic_child_.store(
        other.next_alphabetic_child_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return *this;
  }

  /** Move protobuf and atomic state during contiguous Trie growth. */
  TurnNode(TurnNode&& other) noexcept
      : turn_(std::move(other.turn_)),
        next_numeric_child_(
            other.next_numeric_child_.load(std::memory_order_relaxed)),
        next_alphabetic_child_(
            other.next_alphabetic_child_.load(std::memory_order_relaxed)) {}

  /** Move protobuf and atomic state during Trie replacement. */
  TurnNode& operator=(TurnNode&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    turn_ = std::move(other.turn_);
    next_numeric_child_.store(
        other.next_numeric_child_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    next_alphabetic_child_.store(
        other.next_alphabetic_child_.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return *this;
  }

 private:
  friend class TurnTree;

  /** Select the independent allocator for one child component namespace. */
  std::atomic<std::uint32_t>& allocator(
      AddressComponent::Kind kind) const noexcept {
    return kind == AddressComponent::Kind::NUMERIC ? next_numeric_child_
                                                   : next_alphabetic_child_;
  }

  proto::Turn turn_; /**< Durable Turn payload for an exact Trie sequence. */
  mutable std::atomic<std::uint32_t> next_numeric_child_{
      1U}; /**< Next numeric reply, or zero after exhaustion. */
  mutable std::atomic<std::uint32_t> next_alphabetic_child_{
      1U}; /**< Next alphabetic part, or zero after exhaustion. */
};

/** Materialize Turns and allocate replies from authoritative Trie nodes. */
class TurnTree final {
 public:
  /** Construct an empty tree with one authoritative synthetic-root counter. */
  TurnTree() = default;

  /** Outcomes specific to reply allocation and Turn materialization. */
  enum class Status {
    OK,                /**< The tree operation completed successfully. */
    INVALID_TURN,      /**< Required identity or topology fields are invalid. */
    PARENT_NOT_FOUND,  /**< The committed parent is absent from this Trie. */
    ALREADY_EXISTS,    /**< The human address already identifies a Turn. */
    ADDRESS_EXHAUSTED, /**< The parent has no representable child ordinal. */
  };

  /**
   * Allocate one parent-derived ID and construct an uncommitted Turn shell.
   *
   * The synthetic root is selected when `parent` is null. Allocation is
   * process-local and authoritative. Concurrent allocations are safe while
   * the caller prevents apply() or rebuild() from mutating the Trie.
   */
  Status reply_to(std::span<const std::uint8_t> canvas_uuid,
                  const proto::TurnId* parent, proto::Turn& started);

  /**
   * Allocate one alphabetic response-part ID beneath a committed Turn.
   *
   * Parts use an allocator independent of numeric replies. Concurrent calls
   * are safe while the caller prevents apply() or rebuild() from mutating the
   * Trie.
   */
  Status append_part(std::span<const std::uint8_t> canvas_uuid,
                     const proto::TurnId& parent, proto::Turn& started);

  /** Insert one committed Turn and rehash its root-to-leaf path. */
  Status apply(const proto::Turn& turn);

  /**
   * Reconstruct a durable candidate and retain all live allocator watermarks.
   *
   * A rebuild never makes a process-local reserved ordinal available again
   * beneath any committed parent present in both trees.
   */
  Status rebuild(std::span<const proto::Turn> turns);

  /** Find the durable Turn at one parsed human-readable address. */
  const proto::Turn* find(const TurnAddress& address) const;

  /** Return the number of durable Turns, excluding the internal Trie root. */
  std::size_t size() const noexcept { return trie_.size() - 1U; }

  /** Return the content root of the currently materialized Turn Trie. */
  const hashing::Hash256& root_hash() const noexcept { return root_hash_; }

 private:
  using Trie = containers::Trie<AddressComponent, TurnNode>;

  /** Allocate one child in the selected component namespace. */
  Status allocate_child(std::span<const std::uint8_t> canvas_uuid,
                        const proto::TurnId* parent,
                        AddressComponent::Kind kind, proto::Turn& started);

  /** Merge current process-local allocator watermarks into a rebuilt tree. */
  void preserve_reservations(TurnTree& rebuilt) const;

  /** Canonically hash one node from its Turn and ordered child hashes. */
  hashing::Hash256 hash_node(Trie::NodeIndex index) const;

  Trie trie_; /**< Reply topology keyed by address components. */
  std::vector<hashing::Hash256> hashes_{
      1U}; /**< Content hash parallel to each process-local Trie node. */
  hashing::Hash256 root_hash_; /**< Current content root, empty before Turns. */
};

}  // namespace puc::canvas
