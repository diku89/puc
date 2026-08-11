#pragma once

/**
 * @file cmd_frame.hpp
 * @brief Green command-entry view composed from reusable text frames.
 */

#include <cstddef>
#include <memory>
#include <string>

#include "puc-cli/terminal/event.hpp"
#include "puc-cli/tui/frame.hpp"
#include "puc-cli/tui/text_editor_utils.hpp"

namespace puc::tui {

/**
 * Command-mode editor with its own buffer, annotations, and green visual role.
 *
 * CmdFrame intentionally owns a TextInputFrame rather than sharing the normal
 * draft. Entering command mode can therefore clear this view without touching
 * preserved application input.
 */
class CmdFrame final : public Frame {
 public:
  /** Construct an empty line-numbered command editor. */
  explicit CmdFrame(std::string name = "command");

  CmdFrame(const CmdFrame&)            = delete;
  CmdFrame& operator=(const CmdFrame&) = delete;
  CmdFrame(CmdFrame&&)                 = delete;
  CmdFrame& operator=(CmdFrame&&)      = delete;

  /** Destroy composed text and annotation frames. */
  ~CmdFrame() override;

  /** Apply one normalized editor event. */
  Status handle_event(const terminal::Event& event);

  /** Clear the disposable command buffer. */
  void clear();

  /** Replace the complete command buffer and place the caret at its end. */
  Status replace_text(std::string text);

  /** Return a consistent copy of command-editor state. */
  TextEditorSnapshot snapshot() const;

  /** Return the current line-number gutter width. */
  std::size_t gutter_width() const noexcept;

  /** Return wrapped content rows for a total annotated width. */
  std::size_t preferred_rows(std::size_t width) const;

  /** Draw line annotations and the green text/caret surface. */
  Status draw(const Theme& theme, Canvas& canvas,
              const Canvas::Rect& rect) override;

  /** Command text is selectable. */
  bool is_selectable() const noexcept override;

  /** Delegate selection through the annotation gutter. */
  Status update_selection(const SelectionEvent& event) override;

  /** Extract selected command text. */
  Status selected_text(std::string& output) const override;

  /** Command text accepts stationary-click caret placement. */
  bool accepts_cursor_placement() const noexcept override;

  /** Delegate caret placement through the annotation gutter. */
  Status place_cursor(SelectionPosition position) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_; /**< Hidden composed command view. */
};

}  // namespace puc::tui
