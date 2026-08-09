#pragma once

/**
 * @file trie.hpp
 * @brief Contiguous, allocation-free-lookup trie for key-sequence matching.
 */

#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace puc {
namespace containers {

/**
 * Key type accepted by Trie.
 *
 * Trie keys must support default construction for the sentinel root, copying
 * when a new node is appended, and equality comparison during traversal.
 *
 * @tparam Type Candidate key type.
 */
template <typename Type>
concept TrieKey = std::regular<Type>;

/**
 * Value type accepted by Trie.
 *
 * Values are stored directly in every node. A caller that wants indirect or
 * shared ownership can therefore select a pointer or smart pointer as the
 * value type without the trie imposing that policy.
 *
 * @tparam Type Candidate value type.
 */
template <typename Type>
concept TrieValue = std::default_initializable<Type> && std::movable<Type>;

/**
 * One node in a Trie-owned contiguous node array.
 *
 * Each child is an index into the owning trie's node array. Index zero is the
 * sentinel root, so its key does not represent a sequence element.
 * `sequence_end` and `children` independently describe whether a node is an
 * exact match and whether it is a prefix of longer matches.
 *
 * @tparam KeyType Element type used to form lookup sequences.
 * @tparam ValueType Value stored directly at complete sequences.
 */
template <TrieKey KeyType, TrieValue ValueType>
struct TrieNode {
  KeyType key{}; /**< Key labeling the edge from the parent to this node. */
  std::vector<std::size_t> children; /**< Child indexes in the owning trie. */
  bool sequence_end = false; /**< Whether this node is an exact match. */
  ValueType value{}; /**< Value meaningful when `sequence_end` is true. */
};

/**
 * Store direct values at nodes reached by arbitrary key sequences.
 *
 * All nodes occupy one append-only vector. Callers traverse with stable node
 * indexes rather than pointers, and child indexes remain valid when insertion
 * reallocates the node vector. References and pointers returned for inspection
 * remain valid only until the next insertion.
 *
 * The caller is responsible for synchronization when a trie is accessed from
 * multiple threads. Lookup methods perform no dynamic allocation or mutation;
 * node and child-vector growth occur only during `insert()`.
 *
 * @tparam KeyType Regular sequence element supporting equality comparison.
 * @tparam ValueType Default-initializable, movable directly stored value.
 */
template <TrieKey KeyType, TrieValue ValueType>
class Trie {
 public:
  /** Public node representation. */
  using Node = TrieNode<KeyType, ValueType>;

  /** Stable index into the node vector. */
  using NodeIndex = std::size_t;

  static constexpr NodeIndex kRootNode = 0; /**< Sentinel root node index. */
  static constexpr NodeIndex kInvalidNode =
      std::numeric_limits<NodeIndex>::max(); /**< Missing-node result. */

  /** Construct an empty trie containing only its sentinel root. */
  Trie() { nodes_.emplace_back(); }

  /** Destroy the contiguous node array and directly stored values. */
  ~Trie() = default;

  /**
   * Return the sentinel root index used to begin traversal.
   *
   * @return Always `kRootNode`.
   */
  static constexpr NodeIndex root() noexcept { return kRootNode; }

  /**
   * Return the number of nodes, including the sentinel root.
   *
   * @return Current node count.
   */
  NodeIndex size() const noexcept { return nodes_.size(); }

  /**
   * Inspect a node by index.
   *
   * The returned reference may be invalidated by `insert()` because insertion
   * may reallocate the node vector. The index itself remains valid.
   *
   * @param[in] node_index Valid node index obtained from this trie.
   * @return Read-only node at `node_index`.
   * @pre `node_index < size()`.
   */
  const Node& node(NodeIndex node_index) const noexcept {
    return nodes_[node_index];
  }

  /**
   * Find the node reached by a sequence path.
   *
   * The result may be prefix-only. Inspect its `sequence_end` member to test
   * for an exact match and its children to test for longer matches.
   *
   * @param[in] key_sequence Sequence path to traverse from the root.
   * @return Final node index, or `kInvalidNode` at the first missing key.
   */
  NodeIndex find_node(const std::vector<KeyType>& key_sequence) const {
    NodeIndex current = kRootNode;
    for (const KeyType& key : key_sequence) {
      current = find_child(current, key);
      if (current == kInvalidNode) {
        return kInvalidNode;
      }
    }
    return current;
  }

  /**
   * Find the directly stored value for an exact key sequence.
   *
   * The returned pointer refers into the node vector and may be invalidated by
   * `insert()`. A non-null pointer can itself point to a null smart-pointer
   * value; `sequence_end`, rather than value nullness, defines an exact match.
   *
   * @param[in] key_sequence Sequence to look up.
   * @return Stored value address for an exact match, or `nullptr` otherwise.
   */
  const ValueType* find(const std::vector<KeyType>& key_sequence) const {
    const NodeIndex node_index = find_node(key_sequence);
    if (node_index == kInvalidNode || !nodes_[node_index].sequence_end) {
      return nullptr;
    }
    return &nodes_[node_index].value;
  }

  /**
   * Find one direct child without allocating or modifying the trie.
   *
   * @param[in] node_index Parent node index.
   * @param[in] key Key to match against the parent's children.
   * @return Matching child index, or `kInvalidNode` when absent or invalid.
   */
  NodeIndex find_child(NodeIndex node_index, const KeyType& key) const {
    if (node_index >= nodes_.size()) {
      return kInvalidNode;
    }
    for (const NodeIndex child_index : nodes_[node_index].children) {
      if (nodes_[child_index].key == key) {
        return child_index;
      }
    }
    return kInvalidNode;
  }

  /**
   * Insert a sequence or replace its existing terminal value.
   *
   * Missing nodes are appended in traversal order. Existing prefix and
   * descendant indexes remain unchanged when a value is inserted or replaced.
   *
   * @param[in] key_sequence Sequence to store; an empty sequence selects root.
   * @param[in] value Value moved into the sequence's terminal node.
   * @return Stable terminal node index.
   */
  NodeIndex insert(const std::vector<KeyType>& key_sequence, ValueType value) {
    NodeIndex current = kRootNode;
    for (const KeyType& key : key_sequence) {
      NodeIndex child = find_child(current, key);
      if (child == kInvalidNode) {
        child = nodes_.size();
        nodes_.emplace_back();
        nodes_.back().key = key;
        nodes_[current].children.push_back(child);
      }
      current = child;
    }
    nodes_[current].sequence_end = true;
    nodes_[current].value        = std::move(value);
    return current;
  }

  /**
   * Remove the value associated with an exact sequence.
   *
   * Nodes are intentionally retained so indexes held by active cursors remain
   * valid and a later insertion can reuse the existing path. Descendant
   * sequences are unaffected.
   *
   * @param[in] key_sequence Exact sequence whose value should be removed.
   * @return `true` when an exact value existed and was removed.
   */
  bool erase(const std::vector<KeyType>& key_sequence) {
    const NodeIndex node_index = find_node(key_sequence);
    if (node_index == kInvalidNode || !nodes_[node_index].sequence_end) {
      return false;
    }
    nodes_[node_index].sequence_end = false;
    nodes_[node_index].value        = ValueType{};
    return true;
  }

  /**
   * Test whether a sequence terminates at an exact-match node.
   *
   * This tests `sequence_end`, not the stored value. An exact sequence holding
   * a null smart pointer therefore still counts as present.
   *
   * @param[in] key_sequence Sequence to inspect.
   * @return `true` only for an inserted complete sequence.
   */
  bool contains(const std::vector<KeyType>& key_sequence) const {
    const NodeIndex node_index = find_node(key_sequence);
    return node_index != kInvalidNode && nodes_[node_index].sequence_end;
  }

 private:
  /** Append-only storage whose indexes identify every node. */
  std::vector<Node> nodes_;
};

}  // namespace containers
}  // namespace puc
