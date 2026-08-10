#pragma once

/**
 * @file text_input_frame.hpp
 * @brief Renderable and selectable text-entry surface.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/annotated_text_frame.hpp"
#include "puc-cli/tui/text_editor_utils.hpp"

namespace puc::tui {

/** Semantic palette roles used by one text-entry surface. */
struct TextInputFrameStyle {
  Theme::ColorTypes text_color =
      Theme::ColorTypes::TEXT_SECONDARY; /**< Ordinary text foreground. */
  Theme::ColorTypes cursor_color =
      Theme::ColorTypes::TEXT; /**< Caret background/accent. */
  Theme::ColorTypes background_color =
      Theme::ColorTypes::SECONDARY; /**< Unoccupied cell background. */
  Theme::ColorTypes selection_text_color =
      Theme::ColorTypes::HIGHLIGHT_TEXT; /**< Selected text foreground. */
  Theme::ColorTypes selection_background_color =
      Theme::ColorTypes::HIGHLIGHT_BACKGROUND; /**< Selected background. */
};

/**
 * Frame that turns TextEditor mechanics into terminal cells and input events.
 *
 * It deliberately draws no border and no annotations. Those concerns belong
 * to BoundingFrame and AnnotatedTextFrame, allowing the same editor surface to
 * be embedded in input, command, diff, or other application views.
 */
class TextInputFrame final : public AnnotatedTextSource {
 public:
  /** Construct an empty styled editor. */
  explicit TextInputFrame(std::string name          = "text input",
                          TextInputFrameStyle style = {},
                          TextEditorOptions options = {});

  TextInputFrame(const TextInputFrame&)            = delete;
  TextInputFrame& operator=(const TextInputFrame&) = delete;
  TextInputFrame(TextInputFrame&&)                 = delete;
  TextInputFrame& operator=(TextInputFrame&&)      = delete;

  /** Destroy synchronized editor state. */
  ~TextInputFrame() override;

  /** Atomically replace semantic rendering colors. */
  void set_style(TextInputFrameStyle style);

  /** Return a copy of the active rendering style. */
  TextInputFrameStyle style() const;

  /** Apply one normalized terminal event to the editor. */
  Status handle_event(const terminal::Event& event);

  /** Clear text and all derived interaction state. */
  void clear();

  /** Change the pre-layout word-wrap width. */
  void set_fallback_width(std::size_t width);

  /** Return a consistent copy of editor state. */
  TextEditorSnapshot snapshot() const;

  /** Return the wrapped-row count at an explicit content width. */
  std::size_t preferred_rows(std::size_t width) const;

  /** Draw text, selection, and caret into the exact assigned rectangle. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

  /** Text input always participates in logical selection. */
  bool is_selectable() const noexcept override;

  /** Apply a drag, word, line, all-text, or reset operation. */
  Status update_selection(const SelectionEvent& event) override;

  /** Extract selected logical UTF-8 text. */
  Status selected_text(std::string& output) const override;

  /** Text input accepts stationary-click caret placement. */
  bool accepts_cursor_placement() const noexcept override;

  /** Place the caret at one frame-local visible cell. */
  Status place_cursor(SelectionPosition position) override;

  /** Return zero for pristine input, otherwise the logical line count. */
  std::size_t logical_line_count() const noexcept override;

  /** Return rows visible during the most recent successful draw. */
  std::vector<AnnotatedTextRow> visible_text_rows() const override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden synchronized editor and viewport. */
};

}  // namespace puc::tui
