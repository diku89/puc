#include "utils/hash/sha256.hpp"

#include <gtest/gtest.h>

namespace puc::hashing {

TEST(Sha256Test, MatchesStandardVectors) {
  EXPECT_EQ(sha256("").hex(),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(sha256("abc").hex(),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, ParsesHexRoundTrip) {
  const Hash256 expected = sha256("puc");
  Hash256 parsed;
  ASSERT_TRUE(Hash256::from_hex(expected.hex(), parsed));
  EXPECT_EQ(parsed, expected);
  EXPECT_FALSE(Hash256::from_hex("bad", parsed));
}

}  // namespace puc::hashing
