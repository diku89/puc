#pragma once

/**
 * @file turn_datastore.hpp
 * @brief Atomic human-readable Turn numbering and persistence.
 */

#include <cstdint>
#include <span>
#include <vector>

#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/turn.pb.h"

namespace puc::canvas::datastore {

/** Persist committed Turns under transactionally allocated human addresses. */
class TurnDatastore final {
 public:
  /** Borrow an initialized database for the lifetime of this wrapper. */
  explicit TurnDatastore(Database& database) noexcept : database_(database) {}

  /** Return the ordered migration catalog owned by this datastore. */
  static MigrationSet migrations() noexcept;

  /**
   * Assign the next sibling address and persist the Turn atomically.
   *
   * The submitted Turn may omit its ID entirely. If it supplies a Canvas UUID,
   * it must match `canvas_uuid`; any supplied human address is replaced by the
   * stable address assigned inside the same SQLite write transaction.
   */
  Status number_and_persist(std::span<const std::uint8_t> canvas_uuid,
                            const proto::Turn& submitted, proto::Turn& turn);

  /** Load every committed Turn belonging to one Canvas. */
  Status load_all(std::span<const std::uint8_t> canvas_uuid,
                  std::vector<proto::Turn>& turns);

 private:
  Database& database_; /**< Borrowed initialized SQLite owner. */
};

}  // namespace puc::canvas::datastore
