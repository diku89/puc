/**
 * @file themes_test.cpp
 * @brief Tests for property-backed default theme loading.
 */

#include "themes/themes.hpp"

#include <filesystem>

#include "gtest/gtest.h"
#include "properties/properties.hpp"
#include "puc-cli/tui/theme.hpp"

namespace puc::themes {
namespace {

TEST(ThemesTest, LoadsEveryDefaultColorAsMutableProperties) {
  properties::Properties properties(std::filesystem::current_path(),
                                    ".puc-no-theme-overrides");
  ASSERT_EQ(register_default_dark(properties), Status::OK);

  tui::Theme theme;
  ASSERT_EQ(apply(properties, theme), Status::OK);
  const tui::Theme::Colors colors = theme.get_colors();
  EXPECT_EQ(colors.primary, 0x66d9efU);
  EXPECT_EQ(colors.text, 0xf8f8f2U);
  EXPECT_EQ(colors.diff_added_text_background, 0x263c2cU);
  EXPECT_EQ(colors.background, 0x1b2026U);

  ASSERT_EQ(properties.set("theme.colors.primary", "16711680"),
            properties::Status::OK);
  ASSERT_EQ(apply(properties, theme), Status::OK);
  EXPECT_EQ(theme.get_colors().primary, 0xff0000U);
}

}  // namespace
}  // namespace puc::themes
