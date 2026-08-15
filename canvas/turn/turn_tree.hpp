#pragma once

/**
 * @file turn_tree.hpp
 * @brief In-memory reply topology over the reusable Trie.
 */

#include <span>
#include <vector>

#include "canvas/protos/turn.pb.h"
#include "canvas/turn/address.hpp"
#include "utils/containers/trie.hpp"
#include "utils/hash/sha256.hpp"

namespace puc::canvas {

/** Materialize committed Turns and incrementally hash their reply topology. */
class TurnTree final {
 public:
  /** Outcomes specific to validating and inserting committed Turns. */
  enum class Status {
    OK,               /**< The tree operation completed successfully. */
    INVALID_TURN,     /**< Required identity or topology fields are invalid. */
    PARENT_NOT_FOUND, /**< The committed parent is absent from this Trie. */
    ALREADY_EXISTS,   /**< The human address already identifies a Turn. */
  };

  /** Insert one committed Turn and rehash only its root-to-leaf path. */
  Status insert(const proto::Turn& turn);

  /** Reconstruct a complete candidate Trie and replace this tree on success. */
  Status rebuild(std::span<const proto::Turn> turns);

  /** Find the committed Turn at one parsed human-readable address. */
  const proto::Turn* find(const TurnAddress& address) const;

  /** Return the number of committed Turns, excluding the internal Trie root. */
  std::size_t size() const noexcept { return trie_.size() - 1U; }

  /** Return the content root of the currently materialized Turn Trie. */
  const hashing::Hash256& root_hash() const noexcept { return root_hash_; }

 private:
  /** Canonically hash one node from its Turn and ordered child hashes. */
  hashing::Hash256 hash_node(
      containers::Trie<AddressComponent, proto::Turn>::NodeIndex index) const;

  containers::Trie<AddressComponent, proto::Turn>
      trie_; /**< Reply topology keyed by address components. */
  std::vector<hashing::Hash256> hashes_{
      1U}; /**< Content hash parallel to each process-local Trie node. */
  hashing::Hash256 root_hash_; /**< Current content root, empty before Turns. */
};

}  // namespace puc::canvas
