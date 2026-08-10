#pragma once

/**
 * @file annotated_text_frame.hpp
 * @brief Text-frame decorator for line numbers and per-line status markers.
 */

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "puc-cli/tui/frame.hpp"

namespace puc::tui {

/** Row identity exposed by a wrapped text child after drawing. */
struct AnnotatedTextRow {
  std::size_t logical_line = 0U;   /**< Zero-based source line. */
  bool first_visual_row    = true; /**< False for hard-wrapped continuation. */
};

/** Frame contract required by the annotation decorator. */
class AnnotatedTextSource : public Frame {
 public:
  using Frame::Frame;

  /** Destroy a source through the annotation interface. */
  ~AnnotatedTextSource() override = default;

  /** Return zero for pristine input, otherwise the logical line count. */
  virtual std::size_t logical_line_count() const noexcept = 0;

  /** Return rows visible during the most recent successful draw. */
  virtual std::vector<AnnotatedTextRow> visible_text_rows() const = 0;
};

/** One optional marker rendered beside a zero-based logical line. */
struct AnnotatedLineStatus {
  std::size_t logical_line = 0U; /**< Source line receiving the marker. */
  std::u32string text;           /**< Status characters or emoji scalars. */
  Theme::ColorTypes color =
      Theme::ColorTypes::TEXT_MUTED; /**< Marker foreground role. */
};

/** Gutter geometry and colors for AnnotatedTextFrame. */
struct AnnotatedTextConfiguration {
  bool show_line_numbers = true; /**< Render numbers on first visual rows. */
  std::size_t minimum_line_number_columns =
      2U;                                /**< Stable small-count width. */
  std::size_t status_columns       = 0U; /**< Reserved marker cells per row. */
  std::size_t separator_columns    = 1U; /**< Blank cells before child text. */
  std::size_t minimum_gutter_width = 0U; /**< Optional total-width floor. */
  Theme::ColorTypes background_color =
      Theme::ColorTypes::SECONDARY; /**< Gutter background role. */
  Theme::ColorTypes line_number_color =
      Theme::ColorTypes::TEXT_MUTED; /**< Number foreground role. */
};

/**
 * Decorates a wrapped text Frame with line-number and status columns.
 *
 * Status markers are keyed by logical line, so continuation rows remain
 * visually unannotated. The child reports its post-scroll visible rows after
 * drawing; this keeps annotations aligned without duplicating wrap policy.
 */
class AnnotatedTextFrame final : public Frame {
 public:
  /** Construct a configured decorator around a required text source. */
  AnnotatedTextFrame(std::string name,
                     std::shared_ptr<AnnotatedTextSource> child,
                     AnnotatedTextConfiguration configuration = {});

  AnnotatedTextFrame(const AnnotatedTextFrame&)            = delete;
  AnnotatedTextFrame& operator=(const AnnotatedTextFrame&) = delete;
  AnnotatedTextFrame(AnnotatedTextFrame&&)                 = delete;
  AnnotatedTextFrame& operator=(AnnotatedTextFrame&&)      = delete;

  /** Destroy annotation state and shared child ownership. */
  ~AnnotatedTextFrame() override;

  /** Atomically replace gutter geometry and colors. */
  void set_configuration(AnnotatedTextConfiguration configuration);

  /** Return a copy of the active gutter configuration. */
  AnnotatedTextConfiguration configuration() const;

  /** Replace every per-line status marker. Duplicate lines use the last. */
  void set_statuses(std::vector<AnnotatedLineStatus> statuses);

  /** Remove all status markers without changing number configuration. */
  void clear_statuses();

  /** Return the current total gutter width for the child's line count. */
  std::size_t gutter_width() const noexcept;

  /** Compute the absolute child rectangle for a supplied assigned rect. */
  std::optional<Canvas::Rect> content_rect(const Canvas::Rect& rect) const;

  /** Draw the text child and its aligned annotation gutter. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

  /** Return the child Frame's selection capability. */
  bool is_selectable() const noexcept override;

  /** Translate frame-local coordinates through the gutter. */
  Status update_selection(const SelectionEvent& event) override;

  /** Delegate selected-text extraction to the child. */
  Status selected_text(std::string& output) const override;

  /** Return the child Frame's caret-placement capability. */
  bool accepts_cursor_placement() const noexcept override;

  /** Ignore gutter clicks and delegate child-region caret placement. */
  Status place_cursor(SelectionPosition position) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden synchronized annotation state. */
};

}  // namespace puc::tui
