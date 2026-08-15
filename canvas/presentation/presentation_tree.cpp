/** @file presentation_tree.cpp @brief Persistent Presentation treap. */

#include "canvas/presentation/presentation_tree.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canvas/protos/presentation.pb.h"
#include "canvas/turn/address.hpp"

namespace puc::canvas {
namespace {

std::optional<TurnAddress> address(
    const datastore::StoredPresentationNode& node) {
  return TurnAddress::parse(node.turn_id.human_address());
}

std::uint64_t priority(const proto::TurnId& turn) {
  std::string identity = turn.canvas_uuid();
  identity.push_back('\0');
  identity.append(turn.human_address());
  const hashing::Hash256 digest = hashing::sha256(identity);
  std::uint64_t result          = 0U;
  for (std::size_t index = 0U; index < sizeof(result); ++index) {
    result = (result << 8U) | digest.bytes[index];
  }
  return result;
}

bool above(const datastore::StoredPresentationNode& left,
           const datastore::StoredPresentationNode& right) {
  const std::uint64_t left_priority  = priority(left.turn_id);
  const std::uint64_t right_priority = priority(right.turn_id);
  return left_priority < right_priority ||
         (left_priority == right_priority &&
          left.turn_id.human_address() < right.turn_id.human_address());
}

void append_hash(std::string& encoded, const hashing::Hash256& hash) {
  encoded.push_back(hash.empty() ? '\0' : '\1');
  if (!hash.empty()) {
    encoded.append(reinterpret_cast<const char*>(hash.bytes.data()),
                   hash.bytes.size());
  }
}

std::string canonical_bytes(const datastore::StoredPresentationNode& node) {
  std::string encoded{"PUC-PRESENTATION-NODE-V1", 24U};
  encoded.append(node.turn_id.canvas_uuid());
  encoded.push_back('\0');
  encoded.append(node.turn_id.human_address());
  encoded.push_back('\0');
  append_hash(encoded, node.left_hash);
  append_hash(encoded, node.right_hash);
  return encoded;
}

}  // namespace

datastore::Status PresentationTree::prepare_insert(
    const proto::TurnId& turn, PendingPresentation& pending) {
  pending           = {};
  const auto parsed = TurnAddress::parse(turn.human_address());
  if (!parsed.has_value() || turn.canvas_uuid().size() != 16U) {
    return datastore::Status::INVALID_ARGUMENT;
  }
  datastore::StoredPresentationNode inserted;
  inserted.turn_id      = turn;
  pending.previous_root = root_;
  pending.inserted_turn = turn;
  return insert(root_, inserted, pending, pending.new_root);
}

void PresentationTree::commit(const PendingPresentation& pending) noexcept {
  if (root_ == pending.previous_root) root_ = pending.new_root;
}

datastore::Status PresentationTree::insert(
    const hashing::Hash256& root,
    const datastore::StoredPresentationNode& inserted,
    PendingPresentation& pending, hashing::Hash256& output) {
  if (root.empty()) {
    output = retain(inserted, pending);
    return datastore::Status::OK;
  }
  datastore::StoredPresentationNode current;
  datastore::Status status = load(root, &pending, current);
  if (!datastore::is_ok(status)) return status;
  const auto current_address  = address(current);
  const auto inserted_address = address(inserted);
  if (!current_address.has_value() || !inserted_address.has_value() ||
      current.turn_id.canvas_uuid() != inserted.turn_id.canvas_uuid()) {
    return datastore::Status::CORRUPT_DATA;
  }
  if (*current_address == *inserted_address) {
    return datastore::Status::ALREADY_EXISTS;
  }
  if (above(inserted, current)) {
    datastore::StoredPresentationNode promoted = inserted;
    hashing::Hash256 left;
    hashing::Hash256 right;
    status = split(root, *inserted_address, pending, left, right);
    if (!datastore::is_ok(status)) return status;
    promoted.left_hash  = left;
    promoted.right_hash = right;
    output              = retain(std::move(promoted), pending);
    return datastore::Status::OK;
  }

  hashing::Hash256 child;
  if (*inserted_address < *current_address) {
    status = insert(current.left_hash, inserted, pending, child);
    if (!datastore::is_ok(status)) return status;
    current.left_hash = child;
  } else {
    status = insert(current.right_hash, inserted, pending, child);
    if (!datastore::is_ok(status)) return status;
    current.right_hash = child;
  }
  output = retain(std::move(current), pending);
  return datastore::Status::OK;
}

datastore::Status PresentationTree::split(const hashing::Hash256& root,
                                          const TurnAddress& key,
                                          PendingPresentation& pending,
                                          hashing::Hash256& left,
                                          hashing::Hash256& right) {
  left  = {};
  right = {};
  if (root.empty()) return datastore::Status::OK;
  datastore::StoredPresentationNode current;
  datastore::Status status = load(root, &pending, current);
  if (!datastore::is_ok(status)) return status;
  const auto current_address = address(current);
  if (!current_address.has_value()) return datastore::Status::CORRUPT_DATA;
  if (*current_address < key) {
    hashing::Hash256 middle;
    status = split(current.right_hash, key, pending, middle, right);
    if (!datastore::is_ok(status)) return status;
    current.right_hash = middle;
    left               = retain(std::move(current), pending);
  } else {
    hashing::Hash256 middle;
    status = split(current.left_hash, key, pending, left, middle);
    if (!datastore::is_ok(status)) return status;
    current.left_hash = middle;
    right             = retain(std::move(current), pending);
  }
  return datastore::Status::OK;
}

datastore::Status PresentationTree::load(
    const hashing::Hash256& hash, const PendingPresentation* pending,
    datastore::StoredPresentationNode& node) {
  if (hash.empty()) return datastore::Status::NOT_FOUND;
  if (pending != nullptr) {
    const auto found =
        std::find_if(pending->nodes.rbegin(), pending->nodes.rend(),
                     [&hash](const datastore::HashedPresentationNode& entry) {
                       return entry.first == hash;
                     });
    if (found != pending->nodes.rend()) {
      node = found->second;
      return datastore::Status::OK;
    }
  }
  return datastore_.load_node(presentation_uuid_, hash, node);
}

hashing::Hash256 PresentationTree::retain(
    datastore::StoredPresentationNode node, PendingPresentation& pending) {
  const hashing::Hash256 hash = hashing::sha256(canonical_bytes(node));
  pending.nodes.emplace_back(hash, std::move(node));
  return hash;
}

datastore::Status PresentationTree::ordered_turns(
    std::vector<proto::TurnId>& turns) {
  turns.clear();
  return collect(root_, turns);
}

datastore::Status PresentationTree::collect(const hashing::Hash256& root,
                                            std::vector<proto::TurnId>& turns) {
  if (root.empty()) return datastore::Status::OK;
  datastore::StoredPresentationNode node;
  datastore::Status status = load(root, nullptr, node);
  if (!datastore::is_ok(status)) return status;
  status = collect(node.left_hash, turns);
  if (!datastore::is_ok(status)) return status;
  turns.push_back(node.turn_id);
  return collect(node.right_hash, turns);
}

}  // namespace puc::canvas
