#pragma once

/**
 * @file presentation_datastore.hpp
 * @brief Durable Presentation Merkle nodes and commits.
 */

#include <span>
#include <string>
#include <utility>
#include <vector>

#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/presentation.pb.h"
#include "utils/hash/sha256.hpp"

namespace puc::canvas::datastore {

/** Internal Merkle-node representation kept out of the public protobuf. */
struct StoredPresentationNode {
  proto::TurnId turn_id;       /**< Turn ordered at this node. */
  hashing::Hash256 left_hash;  /**< Immutable left-child root, if present. */
  hashing::Hash256 right_hash; /**< Immutable right-child root, if present. */
};

/** One content hash paired with the node whose canonical bytes produced it. */
using HashedPresentationNode =
    std::pair<hashing::Hash256, StoredPresentationNode>;

/** Persist immutable Presentation nodes and atomic root commits. */
class PresentationDatastore final {
 public:
  /** Borrow an initialized database for the lifetime of this wrapper. */
  explicit PresentationDatastore(Database& database) noexcept
      : database_(database) {}

  /** Return the ordered migration catalog owned by this datastore. */
  static MigrationSet migrations() noexcept;

  /** Load the current root of one identified Presentation Merkle tree. */
  Status load_root(std::span<const std::uint8_t> presentation_uuid,
                   hashing::Hash256& root);

  /** Load one immutable node from the identified Presentation tree. */
  Status load_node(std::span<const std::uint8_t> presentation_uuid,
                   const hashing::Hash256& hash, StoredPresentationNode& node);

  /** Load the Turn addresses already represented by durable commits. */
  Status load_committed_turn_addresses(
      std::span<const std::uint8_t> presentation_uuid,
      std::vector<std::string>& human_addresses);

  /** Atomically retain new nodes and advance a Presentation root. */
  Status commit(std::span<const std::uint8_t> presentation_uuid,
                const hashing::Hash256& previous_root,
                const hashing::Hash256& new_root,
                const proto::TurnId& inserted_turn,
                std::span<const HashedPresentationNode> nodes);

 private:
  Database& database_; /**< Borrowed initialized SQLite owner. */
};

}  // namespace puc::canvas::datastore
