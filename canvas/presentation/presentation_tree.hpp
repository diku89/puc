#pragma once

/**
 * @file presentation_tree.hpp
 * @brief Persistent ordered Presentation Merkle tree.
 */

#include <cstdint>
#include <utility>
#include <vector>

#include "canvas/protos/datastore/presentation_datastore.hpp"
#include "canvas/protos/turn.pb.h"
#include "canvas/turn/address.hpp"
#include "utils/hash/sha256.hpp"

namespace puc::canvas {

/** Nodes and roots produced while preparing one immutable insertion. */
struct PendingPresentation {
  hashing::Hash256 previous_root; /**< Root that the commit must replace. */
  hashing::Hash256 new_root;   /**< Root produced by incremental insertion. */
  proto::TurnId inserted_turn; /**< Turn added by this presentation commit. */
  std::vector<datastore::HashedPresentationNode>
      nodes; /**< Newly materialized immutable nodes. */
};

/** Immutable deterministic treap keyed solely by human Turn address. */
class PresentationTree final {
 public:
  /** Construct one materialized view over a persistent Presentation ID. */
  PresentationTree(datastore::PresentationDatastore& datastore,
                   std::vector<std::uint8_t> presentation_uuid,
                   hashing::Hash256 root) noexcept
      : datastore_(datastore),
        presentation_uuid_(std::move(presentation_uuid)),
        root_(root) {}

  /** Prepare an immutable insertion without advancing the visible root. */
  datastore::Status prepare_insert(const proto::TurnId& turn,
                                   PendingPresentation& pending);

  /** Advance to a successfully persisted prepared root. */
  void commit(const PendingPresentation& pending) noexcept;

  /** Replace the materialized root during restore. */
  void reset(hashing::Hash256 root) noexcept { root_ = root; }

  /** Collect Turn IDs in stable semantic presentation order. */
  datastore::Status ordered_turns(std::vector<proto::TurnId>& turns);

  /** Return the current committed Presentation root. */
  const hashing::Hash256& root() const noexcept { return root_; }

 private:
  /** Insert one node beneath an immutable root and return its replacement. */
  datastore::Status insert(const hashing::Hash256& root,
                           const datastore::StoredPresentationNode& inserted,
                           PendingPresentation& pending,
                           hashing::Hash256& output);
  /** Split one immutable root into keys below and at-or-above `key`. */
  datastore::Status split(const hashing::Hash256& root, const TurnAddress& key,
                          PendingPresentation& pending, hashing::Hash256& left,
                          hashing::Hash256& right);
  /** Load a node from pending materialization or durable storage. */
  datastore::Status load(const hashing::Hash256& hash,
                         const PendingPresentation* pending,
                         datastore::StoredPresentationNode& node);
  /** Canonically hash and retain one newly materialized immutable node. */
  hashing::Hash256 retain(datastore::StoredPresentationNode node,
                          PendingPresentation& pending);
  /** Append one subtree's Turn IDs using an in-order traversal. */
  datastore::Status collect(const hashing::Hash256& root,
                            std::vector<proto::TurnId>& turns);

  datastore::PresentationDatastore& datastore_; /**< Borrowed durable store. */
  std::vector<std::uint8_t>
      presentation_uuid_; /**< Stable identity of the owned tree. */
  hashing::Hash256 root_; /**< Current materialized immutable root. */
};

}  // namespace puc::canvas
