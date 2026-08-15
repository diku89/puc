/** @file turn_tree.cpp @brief Turn reply-Trie materialization. */

#include "canvas/turn/turn_tree.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace puc::canvas {

TurnTree::Status TurnTree::insert(const proto::Turn& turn) {
  if (!turn.has_id() || !turn.id().has_human_address() ||
      !turn.id().has_canvas_uuid()) {
    return Status::INVALID_TURN;
  }
  const auto address = TurnAddress::parse(turn.id().human_address());
  if (!address.has_value() || turn.id().canvas_uuid().size() != 16U ||
      (turn.has_parent() &&
       (!turn.parent().has_canvas_uuid() ||
        !turn.parent().has_human_address() ||
        turn.parent().canvas_uuid() != turn.id().canvas_uuid()))) {
    return Status::INVALID_TURN;
  }
  if (trie_.contains(address->components())) return Status::ALREADY_EXISTS;
  if (turn.has_parent()) {
    const auto parent = TurnAddress::parse(turn.parent().human_address());
    if (!parent.has_value() || !parent->is_prefix_of(*address) ||
        !trie_.contains(parent->components())) {
      return Status::PARENT_NOT_FOUND;
    }
  } else if (address->components().size() != 1U) {
    return Status::INVALID_TURN;
  }
  trie_.insert(address->components(), turn);
  hashes_.resize(trie_.size());
  std::vector<decltype(trie_)::NodeIndex> path{decltype(trie_)::root()};
  auto current = decltype(trie_)::root();
  for (const AddressComponent& component : address->components()) {
    current = trie_.find_child(current, component);
    path.push_back(current);
  }
  for (auto node = path.rbegin(); node != path.rend(); ++node) {
    hashes_[*node] = hash_node(*node);
  }
  root_hash_ = hashes_[decltype(trie_)::root()];
  return Status::OK;
}

TurnTree::Status TurnTree::rebuild(std::span<const proto::Turn> turns) {
  std::vector<const proto::Turn*> ordered;
  ordered.reserve(turns.size());
  for (const proto::Turn& turn : turns) {
    if (!TurnAddress::parse(turn.id().human_address()).has_value()) {
      return Status::INVALID_TURN;
    }
    ordered.push_back(&turn);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const proto::Turn* left, const proto::Turn* right) {
              const auto left_address =
                  TurnAddress::parse(left->id().human_address());
              const auto right_address =
                  TurnAddress::parse(right->id().human_address());
              return *left_address < *right_address;
            });
  TurnTree candidate;
  for (const proto::Turn* turn : ordered) {
    const Status status = candidate.insert(*turn);
    if (status != Status::OK) return status;
  }
  *this = std::move(candidate);
  return Status::OK;
}

const proto::Turn* TurnTree::find(const TurnAddress& address) const {
  return trie_.find(address.components());
}

hashing::Hash256 TurnTree::hash_node(
    containers::Trie<AddressComponent, proto::Turn>::NodeIndex index) const {
  const auto& node = trie_.node(index);
  std::string encoded{"PUC-TURN-TRIE-V1", 16U};
  encoded.push_back(node.sequence_end ? '\1' : '\0');
  if (node.sequence_end) {
    std::string payload;
    if (!node.value.SerializeToString(&payload)) return {};
    encoded.append(payload);
  }
  for (const auto child_index : node.children) {
    const auto& child = trie_.node(child_index);
    encoded.push_back(child.key.kind == AddressComponent::Kind::NUMERIC ? '\0'
                                                                        : '\1');
    for (std::size_t byte = 0U; byte < sizeof(child.key.ordinal); ++byte) {
      const std::size_t shift = (sizeof(child.key.ordinal) - byte - 1U) * 8U;
      encoded.push_back(static_cast<char>(child.key.ordinal >> shift));
    }
    encoded.append(
        reinterpret_cast<const char*>(hashes_[child_index].bytes.data()),
        hashes_[child_index].bytes.size());
  }
  return hashing::sha256(encoded);
}

}  // namespace puc::canvas
