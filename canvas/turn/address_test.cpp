#include "canvas/turn/address.hpp"

#include <gtest/gtest.h>

namespace puc::canvas {

TEST(TurnAddressTest, ParsesAndRendersCanonicalComponents) {
  const auto address = TurnAddress::parse("1.2.aa");
  ASSERT_TRUE(address.has_value());
  EXPECT_EQ(address->string(), "1.2.aa");
  EXPECT_FALSE(TurnAddress::parse("01.2").has_value());
  EXPECT_FALSE(TurnAddress::parse("1..2").has_value());
  EXPECT_FALSE(TurnAddress::parse("1.A").has_value());
}

TEST(TurnAddressTest, OrdersNumericallyAndParentBeforeDescendants) {
  const TurnAddress one     = *TurnAddress::parse("1");
  const TurnAddress one_two = *TurnAddress::parse("1.2");
  const TurnAddress one_ten = *TurnAddress::parse("1.10");
  const TurnAddress two     = *TurnAddress::parse("2");
  EXPECT_LT(one, one_two);
  EXPECT_LT(one_two, one_ten);
  EXPECT_LT(one_ten, two);
  EXPECT_TRUE(one.is_prefix_of(one_two));
}

TEST(TurnAddressTest, RendersAlphabeticOrdinals) {
  EXPECT_EQ(TurnAddress::root(1U).alphabetic_child(1U).string(), "1.a");
  EXPECT_EQ(TurnAddress::root(1U).alphabetic_child(27U).string(), "1.aa");
}

}  // namespace puc::canvas
