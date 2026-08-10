/**
 * @file utf8_test.cpp
 * @brief Tests for complete UTF-8 validation.
 */

#include "utils/utf8/utf8.hpp"

#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace puc::utf8 {
namespace {

TEST(Utf8Test, AcceptsEmptyAsciiAndEmbeddedNull) {
  EXPECT_TRUE(is_valid(""));
  EXPECT_TRUE(is_valid("plain ASCII\n"));
  EXPECT_TRUE(is_valid(std::string_view{"a\0b", 3U}));
}

TEST(Utf8Test, AcceptsUnicodeScalarBoundaryValues) {
  EXPECT_TRUE(is_valid("\xc2\x80"));          // U+0080
  EXPECT_TRUE(is_valid("\xdf\xbf"));          // U+07FF
  EXPECT_TRUE(is_valid("\xe0\xa0\x80"));      // U+0800
  EXPECT_TRUE(is_valid("\xed\x9f\xbf"));      // U+D7FF
  EXPECT_TRUE(is_valid("\xee\x80\x80"));      // U+E000
  EXPECT_TRUE(is_valid("\xef\xbf\xbf"));      // U+FFFF
  EXPECT_TRUE(is_valid("\xf0\x90\x80\x80"));  // U+10000
  EXPECT_TRUE(is_valid("\xf4\x8f\xbf\xbf"));  // U+10FFFF
  EXPECT_TRUE(is_valid("ready \xe2\x9c\x93 \xf0\x9f\x98\x80"));
}

TEST(Utf8Test, RejectsInvalidLeadingAndContinuationBytes) {
  EXPECT_FALSE(is_valid("\x80"));
  EXPECT_FALSE(is_valid("\xbf"));
  EXPECT_FALSE(is_valid("\xc0\x80"));
  EXPECT_FALSE(is_valid("\xc1\xbf"));
  EXPECT_FALSE(is_valid("\xf5\x80\x80\x80"));
  EXPECT_FALSE(is_valid("\xff"));
  EXPECT_FALSE(is_valid("\xe2\x28\xa1"));
  EXPECT_FALSE(is_valid("\xf0\x90\x28\xbc"));
}

TEST(Utf8Test, RejectsTruncatedSequences) {
  EXPECT_FALSE(is_valid("\xc2"));
  EXPECT_FALSE(is_valid("\xe2\x82"));
  EXPECT_FALSE(is_valid("\xf0\x9f\x98"));
  EXPECT_FALSE(is_valid("valid\xf0\x9f\x98"));
}

TEST(Utf8Test, RejectsOverlongSurrogateAndOutOfRangeSequences) {
  EXPECT_FALSE(is_valid("\xe0\x80\x80"));      // Overlong U+0000
  EXPECT_FALSE(is_valid("\xf0\x80\x80\x80"));  // Overlong U+0000
  EXPECT_FALSE(is_valid("\xed\xa0\x80"));      // U+D800
  EXPECT_FALSE(is_valid("\xed\xbf\xbf"));      // U+DFFF
  EXPECT_FALSE(is_valid("\xf4\x90\x80\x80"));  // U+110000
}

}  // namespace
}  // namespace puc::utf8
