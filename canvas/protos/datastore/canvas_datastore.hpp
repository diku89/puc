#pragma once

/**
 * @file canvas_datastore.hpp
 * @brief Durable Canvas aggregate ownership and Merkle-root identities.
 */

#include <span>
#include <vector>

#include "canvas/protos/canvas.pb.h"
#include "canvas/protos/datastore/database.hpp"
#include "utils/hash/sha256.hpp"

namespace puc::canvas::datastore {

/** Persist and retrieve Canvas aggregate roots and their stable tree IDs. */
class CanvasDatastore final {
 public:
  /** Borrow an initialized database for the lifetime of this wrapper. */
  explicit CanvasDatastore(Database& database) noexcept : database_(database) {}

  /** Return the ordered migration catalog owned by this datastore. */
  static MigrationSet migrations() noexcept;

  /** Atomically create one Canvas and its empty owned Merkle trees. */
  Status create(const proto::Canvas& canvas);

  /** Load the oldest Canvas, or NOT_FOUND when the database has none. */
  Status first(proto::Canvas& canvas);

  /** Persist the current root of the identified Turn Trie. */
  Status update_turn_trie_root(std::span<const std::uint8_t> turn_trie_uuid,
                               const hashing::Hash256& root);

 private:
  Database& database_; /**< Borrowed initialized SQLite owner. */
};

}  // namespace puc::canvas::datastore
