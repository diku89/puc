#include "canvas/turn/turn_tree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace puc::canvas {
namespace {

constexpr std::size_t kRootReplies  = 128U;
constexpr std::size_t kChildReplies = 64U;

std::array<std::uint8_t, 16U> canvas_uuid() {
  std::array<std::uint8_t, 16U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index] = static_cast<std::uint8_t>(index + 1U);
  }
  return result;
}

std::vector<std::uint32_t> ordinals(const std::vector<proto::Turn>& turns) {
  std::vector<std::uint32_t> result;
  result.reserve(turns.size());
  for (const proto::Turn& turn : turns) {
    const auto address = TurnAddress::parse(turn.id().human_address());
    if (address.has_value()) {
      result.push_back(
          static_cast<std::uint32_t>(address->components().back().ordinal));
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void commit(proto::Turn& turn) {
  turn.mutable_actor()->set_kind(proto::Actor::HUMAN);
  turn.mutable_payload()->set_kind(proto::Payload::TEXT);
  turn.mutable_payload()->set_content("test");
}

}  // namespace

TEST(TurnTreeTest, AllocatesRepliesPerParentAndReconstructsCounters) {
  const auto uuid = canvas_uuid();
  TurnTree tree;
  std::vector<proto::Turn> roots(kRootReplies);
  std::vector<TurnTree::Status> root_statuses(kRootReplies);
  std::vector<std::thread> threads;
  threads.reserve(kRootReplies);
  for (std::size_t index = 0U; index < roots.size(); ++index) {
    threads.emplace_back([&tree, &uuid, &roots, &root_statuses, index] {
      root_statuses[index] = tree.reply_to(uuid, nullptr, roots[index]);
    });
  }
  for (std::thread& thread : threads) thread.join();

  EXPECT_TRUE(std::all_of(
      root_statuses.begin(), root_statuses.end(),
      [](TurnTree::Status status) { return status == TurnTree::Status::OK; }));
  std::vector<std::uint32_t> expected_roots(kRootReplies);
  for (std::size_t index = 0U; index < expected_roots.size(); ++index) {
    expected_roots[index] = static_cast<std::uint32_t>(index + 1U);
  }
  EXPECT_EQ(ordinals(roots), expected_roots);
  for (proto::Turn& root : roots) {
    commit(root);
    ASSERT_EQ(tree.apply(root), TurnTree::Status::OK);
  }

  const auto first = std::find_if(
      roots.begin(), roots.end(),
      [](const proto::Turn& turn) { return turn.id().human_address() == "1"; });
  ASSERT_NE(first, roots.end());
  std::vector<proto::Turn> children(kChildReplies);
  std::vector<TurnTree::Status> child_statuses(kChildReplies);
  threads.clear();
  threads.reserve(kChildReplies);
  for (std::size_t index = 0U; index < children.size(); ++index) {
    threads.emplace_back(
        [&tree, &uuid, &first, &children, &child_statuses, index] {
          child_statuses[index] =
              tree.reply_to(uuid, &first->id(), children[index]);
        });
  }
  for (std::thread& thread : threads) thread.join();

  EXPECT_TRUE(std::all_of(
      child_statuses.begin(), child_statuses.end(),
      [](TurnTree::Status status) { return status == TurnTree::Status::OK; }));
  std::vector<std::uint32_t> expected_children(kChildReplies);
  for (std::size_t index = 0U; index < expected_children.size(); ++index) {
    expected_children[index] = static_cast<std::uint32_t>(index + 1U);
  }
  EXPECT_EQ(ordinals(children), expected_children);
  for (proto::Turn& child : children) {
    commit(child);
    ASSERT_EQ(tree.apply(child), TurnTree::Status::OK);
  }

  std::vector<proto::Turn> durable = roots;
  durable.insert(durable.end(), children.begin(), children.end());
  TurnTree restored;
  ASSERT_EQ(restored.rebuild(durable), TurnTree::Status::OK);
  proto::Turn next_root;
  ASSERT_EQ(restored.reply_to(uuid, nullptr, next_root), TurnTree::Status::OK);
  EXPECT_EQ(next_root.id().human_address(), "129");
  proto::Turn next_child;
  ASSERT_EQ(restored.reply_to(uuid, &first->id(), next_child),
            TurnTree::Status::OK);
  EXPECT_EQ(next_child.id().human_address(), "1.65");
}

TEST(TurnTreeTest, RejectsASecondCommitForOneReservedAddress) {
  const auto uuid = canvas_uuid();
  TurnTree tree;
  proto::Turn turn;
  ASSERT_EQ(tree.reply_to(uuid, nullptr, turn), TurnTree::Status::OK);
  commit(turn);
  ASSERT_EQ(tree.apply(turn), TurnTree::Status::OK);
  EXPECT_EQ(tree.apply(turn), TurnTree::Status::ALREADY_EXISTS);
}

TEST(TurnTreeTest, AllocatesTheFullUint32OrdinalRange) {
  const auto uuid = canvas_uuid();
  TurnTree tree;
  proto::Turn penultimate;
  penultimate.mutable_id()->set_canvas_uuid(uuid.data(), uuid.size());
  penultimate.mutable_id()->set_human_address(
      std::to_string(std::numeric_limits<std::uint32_t>::max() - 1U));
  commit(penultimate);
  ASSERT_EQ(tree.apply(penultimate), TurnTree::Status::OK);

  proto::Turn last;
  ASSERT_EQ(tree.reply_to(uuid, nullptr, last), TurnTree::Status::OK);
  EXPECT_EQ(last.id().human_address(),
            std::to_string(std::numeric_limits<std::uint32_t>::max()));
  proto::Turn exhausted;
  EXPECT_EQ(tree.reply_to(uuid, nullptr, exhausted),
            TurnTree::Status::ADDRESS_EXHAUSTED);
}

}  // namespace puc::canvas
