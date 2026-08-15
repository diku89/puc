/** @file presentation_datastore.cpp @brief Presentation Merkle persistence. */

#include "canvas/protos/datastore/presentation_datastore.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace puc::canvas::datastore {
namespace {

constexpr std::array kMigrations{
    Migration{.version = 1U, .sql = R"sql(
CREATE TABLE presentation_nodes (
  presentation_uuid BLOB NOT NULL,
  hash BLOB NOT NULL CHECK(length(hash) = 32),
  turn_canvas_uuid BLOB NOT NULL CHECK(length(turn_canvas_uuid) = 16),
  turn_human_address TEXT NOT NULL,
  left_hash BLOB,
  right_hash BLOB,
  PRIMARY KEY(presentation_uuid, hash),
  FOREIGN KEY(presentation_uuid) REFERENCES presentations(presentation_uuid)
) STRICT;
CREATE TABLE presentation_commits (
  presentation_uuid BLOB NOT NULL,
  root_hash BLOB NOT NULL CHECK(length(root_hash) = 32),
  previous_root_hash BLOB,
  inserted_human_address TEXT NOT NULL,
  PRIMARY KEY(presentation_uuid, root_hash),
  FOREIGN KEY(presentation_uuid) REFERENCES presentations(presentation_uuid)
) STRICT;
)sql"},
    Migration{.version = 2U, .sql = R"sql(
CREATE UNIQUE INDEX presentation_commits_by_turn
  ON presentation_commits(presentation_uuid, inserted_human_address);
)sql"},
};

std::span<const std::uint8_t> bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

hashing::Hash256 hash_from_blob(std::string_view blob) {
  hashing::Hash256 result;
  if (blob.size() == result.bytes.size()) {
    std::copy(blob.begin(), blob.end(), result.bytes.begin());
  }
  return result;
}

Status bind_hash(Statement& statement, std::int32_t index,
                 const hashing::Hash256& hash) {
  return hash.empty() ? statement.bind_null(index)
                      : statement.bind(index, hash.bytes);
}

}  // namespace

MigrationSet PresentationDatastore::migrations() noexcept {
  return MigrationSet{.datastore  = "canvas.presentation",
                      .migrations = kMigrations};
}

Status PresentationDatastore::load_root(
    std::span<const std::uint8_t> presentation_uuid, hashing::Hash256& root) {
  root = {};
  if (presentation_uuid.size() != 16U) return Status::INVALID_ARGUMENT;
  const Database::Operation operation = database_.acquire();
  Statement select;
  if (!is_ok(database_.prepare("SELECT root_hash FROM presentations "
                               "WHERE presentation_uuid = ?1;",
                               select)) ||
      !is_ok(select.bind(1, presentation_uuid))) {
    return Status::SQL_ERROR;
  }
  const Status status = select.step();
  if (status != Status::OK) return status;
  if (select.is_null(0)) return Status::OK;
  const std::string stored = select.blob(0);
  if (stored.size() != root.bytes.size()) return Status::CORRUPT_DATA;
  root = hash_from_blob(stored);
  return Status::OK;
}

Status PresentationDatastore::load_node(
    std::span<const std::uint8_t> presentation_uuid,
    const hashing::Hash256& hash, StoredPresentationNode& node) {
  node = {};
  if (presentation_uuid.size() != 16U || hash.empty()) {
    return Status::INVALID_ARGUMENT;
  }
  const Database::Operation operation = database_.acquire();
  Statement select;
  if (!is_ok(database_.prepare(
          "SELECT turn_canvas_uuid, turn_human_address, left_hash, right_hash "
          "FROM presentation_nodes WHERE presentation_uuid = ?1 AND hash = ?2;",
          select)) ||
      !is_ok(select.bind(1, presentation_uuid)) ||
      !is_ok(select.bind(2, hash.bytes))) {
    return Status::SQL_ERROR;
  }
  const Status status = select.step();
  if (status != Status::OK) return status;
  const std::string canvas_uuid = select.blob(0);
  if (canvas_uuid.size() != 16U) return Status::CORRUPT_DATA;
  node.turn_id.set_canvas_uuid(canvas_uuid);
  node.turn_id.set_human_address(select.text(1));
  if (!select.is_null(2)) {
    const std::string stored = select.blob(2);
    if (stored.size() != node.left_hash.bytes.size()) {
      return Status::CORRUPT_DATA;
    }
    node.left_hash = hash_from_blob(stored);
  }
  if (!select.is_null(3)) {
    const std::string stored = select.blob(3);
    if (stored.size() != node.right_hash.bytes.size()) {
      return Status::CORRUPT_DATA;
    }
    node.right_hash = hash_from_blob(stored);
  }
  return Status::OK;
}

Status PresentationDatastore::load_committed_turn_addresses(
    std::span<const std::uint8_t> presentation_uuid,
    std::vector<std::string>& human_addresses) {
  human_addresses.clear();
  if (presentation_uuid.size() != 16U) return Status::INVALID_ARGUMENT;
  const Database::Operation operation = database_.acquire();
  Statement select;
  if (!is_ok(database_.prepare(
          "SELECT inserted_human_address FROM presentation_commits "
          "WHERE presentation_uuid = ?1;",
          select)) ||
      !is_ok(select.bind(1, presentation_uuid))) {
    return Status::SQL_ERROR;
  }
  while (true) {
    const Status status = select.step();
    if (status == Status::NOT_FOUND) return Status::OK;
    if (status != Status::OK) {
      human_addresses.clear();
      return Status::SQL_ERROR;
    }
    human_addresses.push_back(select.text(0));
  }
}

Status PresentationDatastore::commit(
    std::span<const std::uint8_t> presentation_uuid,
    const hashing::Hash256& previous_root, const hashing::Hash256& new_root,
    const proto::TurnId& inserted_turn,
    std::span<const HashedPresentationNode> nodes) {
  if (presentation_uuid.size() != 16U || new_root.empty() || nodes.empty()) {
    return Status::INVALID_ARGUMENT;
  }
  const Database::Operation operation = database_.acquire();
  if (!is_ok(database_.begin_immediate())) return Status::SQL_ERROR;

  hashing::Hash256 stored_root;
  Statement current;
  if (!is_ok(database_.prepare(
          "SELECT root_hash FROM presentations WHERE presentation_uuid = ?1;",
          current)) ||
      !is_ok(current.bind(1, presentation_uuid)) ||
      current.step() != Status::OK) {
    database_.rollback();
    return Status::NOT_FOUND;
  }
  if (!current.is_null(0)) {
    const std::string stored = current.blob(0);
    if (stored.size() != stored_root.bytes.size()) {
      database_.rollback();
      return Status::CORRUPT_DATA;
    }
    stored_root = hash_from_blob(stored);
  }
  if (stored_root != previous_root) {
    database_.rollback();
    return Status::INVALID_STATE;
  }

  Statement insert_node;
  if (!is_ok(database_.prepare(
          "INSERT OR IGNORE INTO presentation_nodes("
          "presentation_uuid, hash, turn_canvas_uuid, turn_human_address, "
          "left_hash, right_hash) VALUES(?1, ?2, ?3, ?4, ?5, ?6);",
          insert_node))) {
    database_.rollback();
    return Status::SQL_ERROR;
  }
  for (const HashedPresentationNode& entry : nodes) {
    if (!is_ok(insert_node.bind(1, presentation_uuid)) ||
        !is_ok(insert_node.bind(2, entry.first.bytes)) ||
        !is_ok(
            insert_node.bind(3, bytes(entry.second.turn_id.canvas_uuid()))) ||
        !is_ok(insert_node.bind(4, entry.second.turn_id.human_address())) ||
        !is_ok(bind_hash(insert_node, 5, entry.second.left_hash)) ||
        !is_ok(bind_hash(insert_node, 6, entry.second.right_hash)) ||
        insert_node.step() != Status::NOT_FOUND ||
        !is_ok(insert_node.reset())) {
      database_.rollback();
      return Status::SQL_ERROR;
    }
  }

  Statement record;
  if (!is_ok(database_.prepare(
          "INSERT INTO presentation_commits(presentation_uuid, root_hash, "
          "previous_root_hash, inserted_human_address) "
          "VALUES(?1, ?2, ?3, ?4);",
          record)) ||
      !is_ok(record.bind(1, presentation_uuid)) ||
      !is_ok(record.bind(2, new_root.bytes)) ||
      !(previous_root.empty() ? is_ok(record.bind_null(3))
                              : is_ok(record.bind(3, previous_root.bytes))) ||
      !is_ok(record.bind(4, inserted_turn.human_address())) ||
      record.step() != Status::NOT_FOUND) {
    database_.rollback();
    return Status::SQL_ERROR;
  }

  Statement update;
  if (!is_ok(database_.prepare("UPDATE presentations SET root_hash = ?1 "
                               "WHERE presentation_uuid = ?2;",
                               update)) ||
      !is_ok(update.bind(1, new_root.bytes)) ||
      !is_ok(update.bind(2, presentation_uuid)) ||
      update.step() != Status::NOT_FOUND || !is_ok(database_.commit())) {
    database_.rollback();
    return Status::SQL_ERROR;
  }
  return Status::OK;
}

}  // namespace puc::canvas::datastore
