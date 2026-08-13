/**
 * @file trie_test.cpp
 * @brief Tests for contiguous storage and incremental trie matching.
 */

#include "utils/containers/trie.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

namespace puc::containers {
namespace {

using StringTrie = Trie<char, std::string>;

static_assert(TrieKey<char>);
static_assert(!TrieKey<std::unique_ptr<int>>);
static_assert(TrieValue<std::string>);
static_assert(TrieValue<std::shared_ptr<int>>);
static_assert(TrieValue<std::unique_ptr<int>>);

/** Convert readable test keys into the sequence representation used by Trie. */
std::vector<char> key(std::string_view text) {
  return {text.begin(), text.end()};
}

TEST(TrieTest, StartsWithOnlyTheRootAndRequiresAnExactSequence) {
  StringTrie trie;

  EXPECT_EQ(trie.root(), 0U);
  EXPECT_EQ(trie.size(), 1U);
  EXPECT_TRUE(trie.node(trie.root()).children.empty());
  EXPECT_FALSE(trie.node(trie.root()).sequence_end);
  EXPECT_EQ(trie.find_node(key("cat")), StringTrie::kInvalidNode);
  EXPECT_EQ(trie.find(key("cat")), nullptr);
  EXPECT_FALSE(trie.contains(key("cat")));

  const StringTrie::NodeIndex terminal = trie.insert(key("cat"), "animal");
  EXPECT_EQ(terminal, 3U);
  EXPECT_EQ(trie.size(), 4U);
  EXPECT_TRUE(trie.contains(key("cat")));
  ASSERT_NE(trie.find(key("cat")), nullptr);
  EXPECT_EQ(*trie.find(key("cat")), "animal");
  EXPECT_FALSE(trie.contains(key("ca")));
  EXPECT_EQ(trie.find(key("ca")), nullptr);
}

TEST(TrieTest, AppendsNodesAndStoresChildIndexes) {
  StringTrie trie;
  EXPECT_EQ(trie.insert(key("abc"), "value"), 3U);

  ASSERT_EQ(trie.node(0).children.size(), 1U);
  EXPECT_EQ(trie.node(0).children[0], 1U);
  EXPECT_EQ(trie.node(1).key, 'a');
  ASSERT_EQ(trie.node(1).children.size(), 1U);
  EXPECT_EQ(trie.node(1).children[0], 2U);
  EXPECT_EQ(trie.node(2).key, 'b');
  ASSERT_EQ(trie.node(2).children.size(), 1U);
  EXPECT_EQ(trie.node(2).children[0], 3U);
  EXPECT_EQ(trie.node(3).key, 'c');
  EXPECT_TRUE(trie.node(3).children.empty());
}

TEST(TrieTest, SequenceEndAndChildrenDescribeEveryMatchState) {
  StringTrie trie;
  ASSERT_NE(trie.insert(key("ab"), "shorter"), StringTrie::kInvalidNode);
  ASSERT_NE(trie.insert(key("abcd"), "longer"), StringTrie::kInvalidNode);

  const StringTrie::NodeIndex prefix_only      = trie.find_node(key("a"));
  const StringTrie::NodeIndex exact_and_prefix = trie.find_node(key("ab"));
  const StringTrie::NodeIndex inner_prefix     = trie.find_node(key("abc"));
  const StringTrie::NodeIndex exact_only       = trie.find_node(key("abcd"));

  ASSERT_NE(prefix_only, StringTrie::kInvalidNode);
  EXPECT_FALSE(trie.node(prefix_only).sequence_end);
  EXPECT_FALSE(trie.node(prefix_only).children.empty());

  ASSERT_NE(exact_and_prefix, StringTrie::kInvalidNode);
  EXPECT_TRUE(trie.node(exact_and_prefix).sequence_end);
  EXPECT_FALSE(trie.node(exact_and_prefix).children.empty());
  EXPECT_EQ(trie.node(exact_and_prefix).value, "shorter");

  ASSERT_NE(inner_prefix, StringTrie::kInvalidNode);
  EXPECT_FALSE(trie.node(inner_prefix).sequence_end);
  EXPECT_FALSE(trie.node(inner_prefix).children.empty());

  ASSERT_NE(exact_only, StringTrie::kInvalidNode);
  EXPECT_TRUE(trie.node(exact_only).sequence_end);
  EXPECT_TRUE(trie.node(exact_only).children.empty());
  EXPECT_EQ(trie.node(exact_only).value, "longer");

  EXPECT_EQ(trie.find_node(key("missing")), StringTrie::kInvalidNode);
}

TEST(TrieTest, StoresIndependentBranchesOnSharedPrefixes) {
  StringTrie trie;
  const StringTrie::NodeIndex cat_node = trie.insert(key("cat"), "cat value");
  const StringTrie::NodeIndex car_node = trie.insert(key("car"), "car value");
  const StringTrie::NodeIndex dog_node = trie.insert(key("dog"), "dog value");

  EXPECT_EQ(*trie.find(key("cat")), "cat value");
  EXPECT_EQ(*trie.find(key("car")), "car value");
  EXPECT_EQ(*trie.find(key("dog")), "dog value");

  const StringTrie::NodeIndex shared_prefix = trie.find_node(key("ca"));
  ASSERT_NE(shared_prefix, StringTrie::kInvalidNode);
  EXPECT_EQ(trie.find_child(shared_prefix, 't'), cat_node);
  EXPECT_EQ(trie.find_child(shared_prefix, 'r'), car_node);
  EXPECT_NE(dog_node, cat_node);
  EXPECT_NE(dog_node, car_node);
}

TEST(TrieTest, SortsChildrenDuringInsertionAndEnumeratesLexicographically) {
  StringTrie trie;
  for (const char branch : {'m', 'a', 'z', 'b', 'y', 'c', 'x', 'd', 'w', 'e'}) {
    ASSERT_NE(trie.insert({branch}, std::string{branch}),
              StringTrie::kInvalidNode);
  }

  std::vector<char> child_keys;
  for (const StringTrie::NodeIndex child : trie.node(trie.root()).children) {
    child_keys.push_back(trie.node(child).key);
  }
  EXPECT_EQ(child_keys, (std::vector<char>{'a', 'b', 'c', 'd', 'e', 'm', 'w',
                                           'x', 'y', 'z'}));
  EXPECT_EQ(trie.completions(), (std::vector<std::vector<char>>{{'a'},
                                                                {'b'},
                                                                {'c'},
                                                                {'d'},
                                                                {'e'},
                                                                {'m'},
                                                                {'w'},
                                                                {'x'},
                                                                {'y'},
                                                                {'z'}}));
}

TEST(TrieTest, FindsChildrenAboveTheLinearSearchThreshold) {
  StringTrie trie;
  static_assert(StringTrie::kLinearSearchMaximumChildren == 8U);
  for (int key_value = 16; key_value >= 0; --key_value) {
    ASSERT_NE(trie.insert({static_cast<char>('a' + key_value)},
                          std::to_string(key_value)),
              StringTrie::kInvalidNode);
  }

  for (int key_value = 0; key_value <= 16; ++key_value) {
    const char branch                 = static_cast<char>('a' + key_value);
    const StringTrie::NodeIndex child = trie.find_child(trie.root(), branch);
    ASSERT_NE(child, StringTrie::kInvalidNode);
    EXPECT_EQ(trie.node(child).key, branch);
  }
  EXPECT_EQ(trie.find_child(trie.root(), 'A'), StringTrie::kInvalidNode);
  EXPECT_EQ(trie.find_child(trie.root(), 'z'), StringTrie::kInvalidNode);
}

TEST(TrieTest, ReinsertionReusesIndexesAndPreservesDescendants) {
  StringTrie trie;
  const StringTrie::NodeIndex descendant =
      trie.insert(key("abcd"), "descendant");
  const StringTrie::NodeIndex terminal   = trie.insert(key("ab"), "original");
  const StringTrie::NodeIndex node_count = trie.size();

  const StringTrie::NodeIndex replaced = trie.insert(key("ab"), "replacement");

  EXPECT_EQ(replaced, terminal);
  EXPECT_EQ(trie.size(), node_count);
  EXPECT_EQ(trie.find_node(key("abcd")), descendant);
  EXPECT_EQ(*trie.find(key("ab")), "replacement");
  EXPECT_EQ(*trie.find(key("abcd")), "descendant");
  EXPECT_FALSE(trie.node(replaced).children.empty());
}

TEST(TrieTest, RootSupportsEmptySequencesAndIncrementalTraversal) {
  StringTrie trie;
  EXPECT_EQ(trie.insert({}, "empty"), trie.root());
  ASSERT_NE(trie.insert(key("abc"), "full"), StringTrie::kInvalidNode);

  StringTrie::NodeIndex current = trie.root();
  EXPECT_TRUE(trie.node(current).sequence_end);
  EXPECT_EQ(trie.node(current).value, "empty");
  EXPECT_FALSE(trie.node(current).children.empty());

  current = trie.find_child(current, 'a');
  ASSERT_NE(current, StringTrie::kInvalidNode);
  EXPECT_FALSE(trie.node(current).sequence_end);
  current = trie.find_child(current, 'b');
  ASSERT_NE(current, StringTrie::kInvalidNode);
  EXPECT_FALSE(trie.node(current).sequence_end);
  current = trie.find_child(current, 'c');
  ASSERT_NE(current, StringTrie::kInvalidNode);
  EXPECT_TRUE(trie.node(current).sequence_end);
  EXPECT_EQ(trie.node(current).value, "full");
  EXPECT_EQ(trie.find_child(current, 'd'), StringTrie::kInvalidNode);
}

TEST(TrieTest, StoresSmartPointersAsOrdinaryValues) {
  using SharedValueTrie = Trie<char, std::shared_ptr<std::string>>;
  SharedValueTrie trie;

  const auto present = std::make_shared<std::string>("present");
  ASSERT_NE(trie.insert(key("present"), present),
            SharedValueTrie::kInvalidNode);
  ASSERT_NE(trie.insert(key("null"), nullptr), SharedValueTrie::kInvalidNode);

  const std::shared_ptr<std::string>* stored = trie.find(key("present"));
  ASSERT_NE(stored, nullptr);
  EXPECT_EQ(*stored, present);

  const std::shared_ptr<std::string>* stored_null = trie.find(key("null"));
  ASSERT_NE(stored_null, nullptr);
  EXPECT_EQ(*stored_null, nullptr);
  EXPECT_TRUE(trie.contains(key("null")));
  EXPECT_EQ(trie.find(key("missing")), nullptr);
}

TEST(TrieTest, SupportsMoveOnlyValues) {
  Trie<char, std::unique_ptr<int>> trie;
  const auto terminal = trie.insert(key("answer"), std::make_unique<int>(42));
  ASSERT_NE(terminal, decltype(trie)::kInvalidNode);

  const std::unique_ptr<int>* stored = trie.find(key("answer"));
  ASSERT_NE(stored, nullptr);
  ASSERT_NE(*stored, nullptr);
  EXPECT_EQ(**stored, 42);
}

TEST(TrieTest, EraseRemovesOnlyTheExactValueAndRetainsItsPath) {
  StringTrie trie;
  trie.insert({'a'}, "prefix");
  const StringTrie::NodeIndex prefix = trie.find_node({'a'});
  trie.insert({'a', 'b'}, "longer");

  EXPECT_TRUE(trie.erase({'a'}));
  EXPECT_FALSE(trie.contains({'a'}));
  ASSERT_NE(trie.find({'a', 'b'}), nullptr);
  EXPECT_EQ(*trie.find({'a', 'b'}), "longer");
  EXPECT_EQ(trie.find_node({'a'}), prefix);
  EXPECT_FALSE(trie.erase({'a'}));
  EXPECT_FALSE(trie.erase({'z'}));
}

TEST(TrieTest, ReturnsCompleteSequencesBelowAnExplicitPrefix) {
  StringTrie trie;
  trie.insert(key("car"), "car");
  trie.insert(key("cart"), "cart");
  trie.insert(key("cat"), "cat");
  trie.insert(key("dog"), "dog");

  EXPECT_EQ(
      trie.completions(key("ca")),
      (std::vector<std::vector<char>>{key("car"), key("cart"), key("cat")}));
  EXPECT_EQ(trie.completions(key("car")),
            (std::vector<std::vector<char>>{key("car"), key("cart")}));
  EXPECT_EQ(trie.completions(key("dog")),
            (std::vector<std::vector<char>>{key("dog")}));
  EXPECT_TRUE(trie.completions(key("missing")).empty());
}

TEST(TrieTest, EnumeratesTheRootWithoutIncludingItsSentinelKey) {
  StringTrie trie;
  trie.insert({}, "empty");
  trie.insert(key("a"), "a");
  trie.insert(key("ab"), "ab");

  EXPECT_EQ(trie.completions(),
            (std::vector<std::vector<char>>{{}, key("a"), key("ab")}));
}

TEST(TrieTest, OmitsErasedTerminalsButRetainsTheirCompletions) {
  StringTrie trie;
  trie.insert(key("run"), "run");
  trie.insert(key("runner"), "runner");
  ASSERT_TRUE(trie.erase(key("run")));

  EXPECT_EQ(trie.completions(key("run")),
            (std::vector<std::vector<char>>{key("runner")}));
}

TEST(TrieTest, NodeIndexesSurviveNodeVectorGrowth) {
  Trie<int, int> trie;
  const auto first = trie.insert({0}, 100);
  ASSERT_EQ(first, 1U);

  for (int key_value = 1; key_value < 1024; ++key_value) {
    ASSERT_NE(trie.insert({key_value}, key_value),
              decltype(trie)::kInvalidNode);
  }

  EXPECT_EQ(trie.find_node({0}), first);
  EXPECT_EQ(trie.node(first).key, 0);
  EXPECT_EQ(trie.node(first).value, 100);
  EXPECT_EQ(trie.size(), 1025U);
}

TEST(TrieTest, LookupDoesNotCreateOrMutateNodes) {
  StringTrie trie;
  ASSERT_NE(trie.insert(key("abc"), "value"), StringTrie::kInvalidNode);
  const std::vector<char> sequence              = key("abc");
  const StringTrie::NodeIndex terminal_before   = trie.find_node(sequence);
  const StringTrie::NodeIndex node_count_before = trie.size();
  const std::size_t root_children_before =
      trie.node(trie.root()).children.size();
  const std::size_t root_capacity_before =
      trie.node(trie.root()).children.capacity();

  for (std::size_t iteration = 0; iteration < 1000; ++iteration) {
    EXPECT_EQ(trie.find_node(sequence), terminal_before);
    ASSERT_NE(trie.find(sequence), nullptr);
    EXPECT_EQ(*trie.find(sequence), "value");
    EXPECT_TRUE(trie.contains(sequence));
  }

  EXPECT_EQ(trie.size(), node_count_before);
  EXPECT_EQ(trie.node(trie.root()).children.size(), root_children_before);
  EXPECT_EQ(trie.node(trie.root()).children.capacity(), root_capacity_before);
  EXPECT_EQ(trie.find_node(sequence), terminal_before);
}

TEST(TrieTest, FindChildRejectsInvalidAndMissingIndexes) {
  StringTrie trie;
  EXPECT_EQ(trie.find_child(StringTrie::kInvalidNode, 'x'),
            StringTrie::kInvalidNode);
  EXPECT_EQ(trie.find_child(trie.size(), 'x'), StringTrie::kInvalidNode);

  const StringTrie::NodeIndex node = trie.insert(key("ab"), "value");
  ASSERT_NE(node, StringTrie::kInvalidNode);
  const StringTrie::NodeIndex parent = trie.find_node(key("a"));
  ASSERT_NE(parent, StringTrie::kInvalidNode);
  EXPECT_EQ(trie.find_child(parent, 'b'), node);
  EXPECT_EQ(trie.find_child(parent, 'x'), StringTrie::kInvalidNode);
}

}  // namespace
}  // namespace puc::containers
