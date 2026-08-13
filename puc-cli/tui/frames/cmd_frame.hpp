#pragma once

/**
 * @file cmd_frame.hpp
 * @brief Green command-entry view composed from reusable text frames.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "puc-cli/tui/rendering/frame.hpp"
#include "puc-cli/tui/terminal/event.hpp"
#include "puc-cli/tui/text_input/text_editor_utils.hpp"

namespace puc::tui {

/** One command spelling and its completion-list description. */
struct CmdCompletion {
  std::string command;     /**< Registered command spelling or alias. */
  std::string description; /**< Short description displayed beside it. */
};

/**
 * Command-mode editor with its own buffer, annotations, and green visual role.
 *
 * CmdFrame intentionally owns a TextInputFrame rather than sharing the normal
 * draft. Entering command mode can therefore clear this view without touching
 * preserved application input.
 */
class CmdFrame final : public Frame {
 public:
  /** Blank cells between the longest command and every description. */
  static constexpr std::size_t kDescriptionGap = 8U;

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

  /**
   * Present command-name completions above the editor.
   *
   * The matched prefix uses the highlight palette, the remaining command uses
   * the secondary text role, and descriptions share one column beginning
   * kDescriptionGap cells after the longest supplied command.
   */
  void set_completions(std::string typed_prefix,
                       std::vector<CmdCompletion> completions,
                       std::size_t selected_completion);

  /** Present exact-command usage as unsegmented help rows. */
  void set_usage(std::vector<std::string> usage_rows);

  /** Remove completion and usage presentation without changing command text. */
  void clear_help();

  /** Return the number of help rows currently presented above the editor. */
  std::size_t help_rows() const noexcept;

  /** Return flattened help text for state snapshots and diagnostics. */
  std::vector<std::string> help_text() const;

  /** Draw as many prepared help rows as fit in the supplied rectangle. */
  Status draw_help(const Theme& theme, Canvas& canvas,
                   const Canvas::Rect& rect) const;

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
