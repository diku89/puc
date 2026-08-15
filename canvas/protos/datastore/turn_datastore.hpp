#pragma once

/**
 * @file turn_datastore.hpp
 * @brief Persistence for pre-addressed committed Turns.
 */

#include <cstdint>
#include <span>
#include <vector>

#include "canvas/protos/datastore/database.hpp"
#include "canvas/protos/turn.pb.h"

namespace puc::canvas::datastore {

/** Persist committed Turns under caller-reserved reply or part addresses. */
class TurnDatastore final {
 public:
  /** Borrow an initialized database for the lifetime of this wrapper. */
  explicit TurnDatastore(Database& database) noexcept : database_(database) {}

  /** Return the ordered migration catalog owned by this datastore. */
  static MigrationSet migrations() noexcept;

  /** Persist one complete, pre-addressed Turn exactly once. */
  Status persist(std::span<const std::uint8_t> canvas_uuid,
                 const proto::Turn& submitted, proto::Turn& turn);

  /** Load the latest durable state of every Turn belonging to one Canvas. */
  Status load_all(std::span<const std::uint8_t> canvas_uuid,
                  std::vector<proto::Turn>& turns);

 private:
  Database& database_; /**< Borrowed initialized SQLite owner. */
};

}  // namespace puc::canvas::datastore
