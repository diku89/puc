/** @file turn_datastore.cpp @brief Transactional Turn numbering and storage. */

#include "canvas/protos/datastore/turn_datastore.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "canvas/turn/address.hpp"

namespace puc::canvas::datastore {
namespace {

constexpr std::size_t kCanvasUuidBytes = 16U;

constexpr std::array kMigrations{
    Migration{.version = 1U, .sql = R"sql(
CREATE TABLE turns (
  canvas_uuid BLOB NOT NULL,
  human_address TEXT NOT NULL,
  parent_address TEXT,
  sibling_ordinal INTEGER NOT NULL CHECK(sibling_ordinal > 0),
  payload BLOB NOT NULL,
  PRIMARY KEY(canvas_uuid, human_address),
  FOREIGN KEY(canvas_uuid) REFERENCES canvases(canvas_uuid)
) STRICT;
CREATE INDEX turns_by_parent
  ON turns(canvas_uuid, parent_address, sibling_ordinal);
)sql"},
};

std::span<const std::uint8_t> bytes(std::string_view value) {
  return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

Status bind_parent(Statement& statement, std::int32_t index,
                   const proto::Turn& turn) {
  return turn.has_parent()
             ? statement.bind(index, turn.parent().human_address())
             : statement.bind_null(index);
}

}  // namespace

MigrationSet TurnDatastore::migrations() noexcept {
  return MigrationSet{.datastore = "canvas.turns", .migrations = kMigrations};
}

Status TurnDatastore::number_and_persist(
    std::span<const std::uint8_t> canvas_uuid, const proto::Turn& submitted,
    proto::Turn& turn) {
  turn.Clear();
  const std::string_view canvas_bytes{
      reinterpret_cast<const char*>(canvas_uuid.data()), canvas_uuid.size()};
  if (canvas_uuid.size() != kCanvasUuidBytes ||
      (submitted.has_id() && submitted.id().has_canvas_uuid() &&
       submitted.id().canvas_uuid() != canvas_bytes) ||
      !submitted.has_actor() || !submitted.actor().has_kind() ||
      submitted.actor().kind() == proto::Actor::UNSPECIFIED ||
      !submitted.has_payload() || !submitted.payload().has_kind() ||
      submitted.payload().kind() == proto::Payload::KIND_UNSPECIFIED ||
      (submitted.has_parent() &&
       ((submitted.parent().has_canvas_uuid() &&
         submitted.parent().canvas_uuid() != canvas_bytes) ||
        !submitted.parent().has_human_address() ||
        !TurnAddress::parse(submitted.parent().human_address()).has_value()))) {
    return Status::INVALID_ARGUMENT;
  }
  if (!is_ok(database_.begin_immediate())) return Status::SQL_ERROR;

  if (submitted.has_parent()) {
    Statement parent;
    if (!is_ok(
            database_.prepare("SELECT 1 FROM turns WHERE canvas_uuid = ?1 AND "
                              "human_address = ?2;",
                              parent)) ||
        !is_ok(parent.bind(1, canvas_uuid)) ||
        !is_ok(parent.bind(2, submitted.parent().human_address())) ||
        parent.step() != Status::OK) {
      database_.rollback();
      return Status::NOT_FOUND;
    }
  }

  Statement next;
  if (!is_ok(database_.prepare(
          "SELECT COALESCE(MAX(sibling_ordinal), 0) + 1 FROM turns "
          "WHERE canvas_uuid = ?1 AND parent_address IS ?2;",
          next)) ||
      !is_ok(next.bind(1, canvas_uuid)) ||
      !is_ok(bind_parent(next, 2, submitted)) || next.step() != Status::OK) {
    database_.rollback();
    return Status::SQL_ERROR;
  }
  const std::int64_t next_ordinal = next.integer(0);
  if (next_ordinal <= 0) {
    database_.rollback();
    return Status::CORRUPT_DATA;
  }

  const TurnAddress address =
      submitted.has_parent()
          ? TurnAddress::parse(submitted.parent().human_address())
                ->numeric_child(static_cast<std::uint64_t>(next_ordinal))
          : TurnAddress::root(static_cast<std::uint64_t>(next_ordinal));

  turn = submitted;
  turn.mutable_id()->set_canvas_uuid(canvas_uuid.data(), canvas_uuid.size());
  turn.mutable_id()->set_human_address(address.string());
  if (turn.has_parent() && !turn.parent().has_canvas_uuid()) {
    turn.mutable_parent()->set_canvas_uuid(canvas_uuid.data(),
                                           canvas_uuid.size());
  }

  std::string payload;
  if (!turn.SerializeToString(&payload)) {
    database_.rollback();
    turn.Clear();
    return Status::SERIALIZATION_ERROR;
  }

  Statement insert;
  if (!is_ok(database_.prepare(
          "INSERT INTO turns(canvas_uuid, human_address, parent_address, "
          "sibling_ordinal, payload) VALUES(?1, ?2, ?3, ?4, ?5);",
          insert)) ||
      !is_ok(insert.bind(1, canvas_uuid)) ||
      !is_ok(insert.bind(2, address.string())) ||
      !is_ok(bind_parent(insert, 3, submitted)) ||
      !is_ok(insert.bind(4, next_ordinal)) ||
      !is_ok(insert.bind(5, bytes(payload)))) {
    database_.rollback();
    turn.Clear();
    return Status::SQL_ERROR;
  }
  const Status inserted = insert.step();
  if (inserted != Status::NOT_FOUND) {
    database_.rollback();
    turn.Clear();
    return inserted == Status::ALREADY_EXISTS ? Status::ALREADY_EXISTS
                                              : Status::SQL_ERROR;
  }
  if (!is_ok(database_.commit())) {
    database_.rollback();
    turn.Clear();
    return Status::SQL_ERROR;
  }
  return Status::OK;
}

Status TurnDatastore::load_all(std::span<const std::uint8_t> canvas_uuid,
                               std::vector<proto::Turn>& turns) {
  turns.clear();
  if (canvas_uuid.size() != kCanvasUuidBytes) return Status::INVALID_ARGUMENT;
  Statement select;
  if (!is_ok(database_.prepare(
          "SELECT payload FROM turns WHERE canvas_uuid = ?1;", select)) ||
      !is_ok(select.bind(1, canvas_uuid))) {
    return Status::SQL_ERROR;
  }
  while (true) {
    const Status status = select.step();
    if (status == Status::NOT_FOUND) return Status::OK;
    if (status != Status::OK) return Status::SQL_ERROR;
    proto::Turn turn;
    const std::string payload = select.blob(0);
    if (!turn.ParseFromString(payload)) {
      turns.clear();
      return Status::CORRUPT_DATA;
    }
    turns.push_back(std::move(turn));
  }
}

}  // namespace puc::canvas::datastore
