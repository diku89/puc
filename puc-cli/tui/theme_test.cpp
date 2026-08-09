/**
 * @file theme_test.cpp
 * @brief Unit tests for complete Theme palette storage and semantic lookup.
 */

#include "puc-cli/tui/theme.hpp"

#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace puc::tui {
namespace {

/** Return a palette whose unique values expose incorrect role mappings. */
Theme::Colors test_colors() {
  return Theme::Colors{
      .primary                      = 1,
      .secondary                    = 2,
      .tertiary                     = 3,
      .highlight_background         = 4,
      .highlight_text               = 5,
      .text                         = 6,
      .text_secondary               = 7,
      .text_tertiary                = 8,
      .text_muted                   = 9,
      .text_disabled                = 10,
      .text_error                   = 11,
      .text_warning                 = 12,
      .text_alert                   = 13,
      .text_success                 = 14,
      .text_info                    = 15,
      .text_link                    = 16,
      .text_emphasis                = 17,
      .text_code                    = 18,
      .alert_text_primary           = 19,
      .alert_text_secondary         = 20,
      .diff_added_text_primary      = 21,
      .diff_added_text_secondary    = 22,
      .diff_added_text_background   = 23,
      .diff_removed_text_primary    = 24,
      .diff_removed_text_secondary  = 25,
      .diff_removed_text_background = 26,
      .background                   = 27,
  };
}

TEST(ThemeTest, DefaultsToZeroInitializedColors) {
  const Theme theme;
  const Theme::Colors colors = theme.get_colors();

  EXPECT_EQ(colors.primary, 0U);
  EXPECT_EQ(colors.highlight_background, 0U);
  EXPECT_EQ(colors.highlight_text, 0U);
  EXPECT_EQ(colors.text, 0U);
  EXPECT_EQ(colors.diff_added_text_background, 0U);
  EXPECT_EQ(colors.background, 0U);
}

TEST(ThemeTest, LoadsAndReturnsColorsByValue) {
  Theme theme;
  Theme::Colors colors = test_colors();
  theme.load_colors(colors);
  colors.primary = 1000;

  const Theme::Colors loaded = theme.get_colors();
  EXPECT_EQ(loaded.primary, 1U);
  EXPECT_EQ(loaded.text, 6U);
  EXPECT_EQ(loaded.background, 27U);
}

TEST(ThemeTest, ResolvesEverySemanticColor) {
  Theme theme;
  theme.load_colors(test_colors());

  const std::vector<std::pair<Theme::ColorTypes, uint32_t>> expected{
      {Theme::ColorTypes::PRIMARY, 1},
      {Theme::ColorTypes::SECONDARY, 2},
      {Theme::ColorTypes::TERTIARY, 3},
      {Theme::ColorTypes::HIGHLIGHT_BACKGROUND, 4},
      {Theme::ColorTypes::HIGHLIGHT_TEXT, 5},
      {Theme::ColorTypes::TEXT, 6},
      {Theme::ColorTypes::TEXT_SECONDARY, 7},
      {Theme::ColorTypes::TEXT_TERTIARY, 8},
      {Theme::ColorTypes::TEXT_MUTED, 9},
      {Theme::ColorTypes::TEXT_DISABLED, 10},
      {Theme::ColorTypes::TEXT_ERROR, 11},
      {Theme::ColorTypes::TEXT_WARNING, 12},
      {Theme::ColorTypes::TEXT_ALERT, 13},
      {Theme::ColorTypes::TEXT_SUCCESS, 14},
      {Theme::ColorTypes::TEXT_INFO, 15},
      {Theme::ColorTypes::TEXT_LINK, 16},
      {Theme::ColorTypes::TEXT_EMPHASIS, 17},
      {Theme::ColorTypes::TEXT_CODE, 18},
      {Theme::ColorTypes::ALERT_TEXT_PRIMARY, 19},
      {Theme::ColorTypes::ALERT_TEXT_SECONDARY, 20},
      {Theme::ColorTypes::DIFF_ADDED_TEXT_PRIMARY, 21},
      {Theme::ColorTypes::DIFF_ADDED_TEXT_SECONDARY, 22},
      {Theme::ColorTypes::DIFF_ADDED_TEXT_BACKGROUND, 23},
      {Theme::ColorTypes::DIFF_REMOVED_TEXT_PRIMARY, 24},
      {Theme::ColorTypes::DIFF_REMOVED_TEXT_SECONDARY, 25},
      {Theme::ColorTypes::DIFF_REMOVED_TEXT_BACKGROUND, 26},
      {Theme::ColorTypes::BACKGROUND, 27},
  };

  for (const auto& [type, value] : expected) {
    EXPECT_EQ(theme.get_color(type), value);
  }
}

TEST(ThemeTest, InvalidSemanticColorResolvesToZero) {
  Theme theme;
  theme.load_colors(test_colors());

  EXPECT_EQ(theme.get_color(static_cast<Theme::ColorTypes>(-1)), 0U);
}

}  // namespace
}  // namespace puc::tui
