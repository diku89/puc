/**
 * @file cmd_frame.cpp
 * @brief Composed command-entry view implementation.
 */

#include "puc-cli/tui/frames/cmd_frame.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "puc-cli/tui/frames/annotated_text_frame.hpp"
#include "puc-cli/tui/frames/text_input_frame.hpp"

namespace puc::tui {

/** Owned text surface and its line-number decorator. */
class CmdFrame::Impl {
 public:
  Impl()
      : input(std::make_shared<TextInputFrame>(
            "command text",
            TextInputFrameStyle{
                .text_color           = Theme::ColorTypes::TEXT_SUCCESS,
                .cursor_color         = Theme::ColorTypes::TEXT_SUCCESS,
                .background_color     = Theme::ColorTypes::SECONDARY,
                .selection_text_color = Theme::ColorTypes::HIGHLIGHT_TEXT,
                .selection_background_color =
                    Theme::ColorTypes::HIGHLIGHT_BACKGROUND,
            })),
        annotated(std::make_shared<AnnotatedTextFrame>(
            "annotated command text", input,
            AnnotatedTextConfiguration{
                .show_line_numbers           = true,
                .minimum_line_number_columns = 2U,
                .status_columns              = 0U,
                .separator_columns           = 1U,
                .minimum_gutter_width        = 0U,
                .background_color            = Theme::ColorTypes::SECONDARY,
                .line_number_color           = Theme::ColorTypes::TEXT_MUTED,
            })) {}

  std::shared_ptr<TextInputFrame> input; /**< Disposable command editor. */
  std::shared_ptr<AnnotatedTextFrame> annotated; /**< Numbered command view. */
};

CmdFrame::CmdFrame(std::string name)
    : Frame(std::move(name)), impl_(std::make_unique<Impl>()) {}

CmdFrame::~CmdFrame() = default;

Status CmdFrame::handle_event(const terminal::Event& event) {
  return impl_->input->handle_event(event);
}

void CmdFrame::clear() { impl_->input->clear(); }

Status CmdFrame::replace_text(std::string text) {
  impl_->input->clear();
  if (text.empty()) {
    return Status::OK;
  }
  return impl_->input->handle_event(
      terminal::Event{terminal::TextEvent{.utf8 = std::move(text)}});
}

TextEditorSnapshot CmdFrame::snapshot() const {
  return impl_->input->snapshot();
}

std::size_t CmdFrame::gutter_width() const noexcept {
  return impl_->annotated->gutter_width();
}

std::size_t CmdFrame::preferred_rows(std::size_t width) const {
  const std::size_t gutter = gutter_width();
  return width > gutter ? impl_->input->preferred_rows(width - gutter) : 0U;
}

Status CmdFrame::draw(const Theme& theme, Canvas& canvas,
                      const Canvas::Rect& rect) {
  return impl_->annotated->draw(theme, canvas, rect);
}

bool CmdFrame::is_selectable() const noexcept {
  return impl_->annotated->is_selectable();
}

Status CmdFrame::update_selection(const SelectionEvent& event) {
  return impl_->annotated->update_selection(event);
}

Status CmdFrame::selected_text(std::string& output) const {
  return impl_->annotated->selected_text(output);
}

bool CmdFrame::accepts_cursor_placement() const noexcept {
  return impl_->annotated->accepts_cursor_placement();
}

Status CmdFrame::place_cursor(SelectionPosition position) {
  return impl_->annotated->place_cursor(position);
}

}  // namespace puc::tui
