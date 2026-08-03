#pragma once

/**
 * @file theme.hpp
 * @brief Semantic true-color palette used by terminal UI frames.
 */

#include <cstdint>

namespace puc {
namespace tui {

/**
 * Stores and resolves semantic colors independently of rendering policy.
 *
 * Frames ask for roles such as text, warning, or diff background instead of
 * embedding RGB literals. Theme itself does not emit terminal escape sequences;
 * Canvas cells carry the resolved values to Screen.
 */
class Theme {
 public:
  /**
   * Semantic roles addressable through `get_color()`.
   */
  enum class ColorTypes {
    PRIMARY,                      /**< Primary accent color. */
    SECONDARY,                    /**< Secondary accent color. */
    TERTIARY,                     /**< Tertiary accent color. */
    HIGHLIGHT,                    /**< Highlighted-region background. */
    HIGHLIGHT_TEXT,               /**< Text shown over a highlight. */
    TEXT,                         /**< Default foreground text. */
    TEXT_SECONDARY,               /**< Secondary foreground text. */
    TEXT_TERTIARY,                /**< Tertiary foreground text. */
    TEXT_MUTED,                   /**< De-emphasized text. */
    TEXT_DISABLED,                /**< Disabled-control text. */
    TEXT_ERROR,                   /**< Error diagnostic text. */
    TEXT_WARNING,                 /**< Warning diagnostic text. */
    TEXT_ALERT,                   /**< Alert text. */
    TEXT_SUCCESS,                 /**< Success diagnostic text. */
    TEXT_INFO,                    /**< Informational text. */
    TEXT_LINK,                    /**< Link text. */
    TEXT_EMPHASIS,                /**< Emphasized text. */
    TEXT_CODE,                    /**< Inline or block code text. */
    ALERT_TEXT_PRIMARY,           /**< Primary text within an alert. */
    ALERT_TEXT_SECONDARY,         /**< Secondary text within an alert. */
    DIFF_ADDED_TEXT_PRIMARY,      /**< Primary text for added diff content. */
    DIFF_ADDED_TEXT_SECONDARY,    /**< Secondary text for added diff content. */
    DIFF_ADDED_TEXT_BACKGROUND,   /**< Diff added text background color. */
    DIFF_REMOVED_TEXT_PRIMARY,    /**< Primary text for removed diff content. */
    DIFF_REMOVED_TEXT_SECONDARY,  /**< Secondary text for removed diff
                                     content. */
    DIFF_REMOVED_TEXT_BACKGROUND, /**< Diff removed text background color. */
    BACKGROUND                    /**< Default canvas background. */
  };

  /**
   * Complete semantic palette.
   *
   * All colors are RGB values represented as 32-bit unsigned integers in the
   * format `0xRRGGBB`. Aggregate-initialize every field before passing a value
   * to `load_colors()`; value-initialization produces an all-black palette.
   */
  struct Colors {
    uint32_t primary;                   /**< Primary accent color. */
    uint32_t secondary;                 /**< Secondary accent color. */
    uint32_t tertiary;                  /**< Tertiary accent color. */
    uint32_t highlight;                 /**< Highlighted-region background. */
    uint32_t highlight_text;            /**< Text shown over a highlight. */
    uint32_t text;                      /**< Default foreground text. */
    uint32_t text_secondary;            /**< Secondary text color. */
    uint32_t text_tertiary;             /**< Tertiary text color. */
    uint32_t text_muted;                /**< Muted text color. */
    uint32_t text_disabled;             /**< Disabled text color. */
    uint32_t text_error;                /**< Error text color. */
    uint32_t text_warning;              /**< Warning text color. */
    uint32_t text_alert;                /**< Alert text color. */
    uint32_t text_success;              /**< Success text color. */
    uint32_t text_info;                 /**< Info text color. */
    uint32_t text_link;                 /**< Link text color. */
    uint32_t text_emphasis;             /**< Emphasis text color. */
    uint32_t text_code;                 /**< Code text color. */
    uint32_t alert_text_primary;        /**< Primary text within an alert. */
    uint32_t alert_text_secondary;      /**< Secondary text within an alert. */
    uint32_t diff_added_text_primary;   /**< Primary added-diff text. */
    uint32_t diff_added_text_secondary; /**< Diff added text secondary
                                           color. */
    uint32_t
        diff_added_text_background; /**< Diff added text background color. */
    uint32_t diff_removed_text_primary;    /**< Primary removed-diff text. */
    uint32_t diff_removed_text_secondary;  /**< Diff removed text secondary
                                              color. */
    uint32_t diff_removed_text_background; /**< Diff removed text background
                                              color. */
    uint32_t background;                   /**< Background color. */
  };

  /** Construct a theme whose complete palette is zero-initialized. */
  Theme() = default;

  /** Destroy a theme through its public interface. */
  virtual ~Theme() = default;

  /**
   * Replace the complete semantic palette.
   *
   * @param[in] colors Palette copied into this Theme.
   */
  void load_colors(const Colors& colors);

  /**
   * Get the current color theme.
   *
   * @return A copy of the current complete palette.
   */
  Colors get_colors() const;

  /**
   * Resolve a semantic color type to its RGB value.
   *
   * @param[in] color_type The semantic color to resolve.
   * @return The corresponding `0xRRGGBB` value, or zero for an unknown enum
   *         value.
   */
  uint32_t get_color(ColorTypes color_type) const noexcept;

 private:
  /** Current palette, value-initialized to all zeros. */
  Colors colors_{};
};

}  // namespace tui
}  // namespace puc
