/**
 * @file cmd_frame.cpp
 * @brief Composed command-entry view implementation.
 */

#include "puc-cli/tui/frames/cmd_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "puc-cli/tui/frames/annotated_text_frame.hpp"
#include "puc-cli/tui/frames/text_input_frame.hpp"

namespace puc::tui {

namespace {

/** One styled portion of a prepared command-help row. */
struct HelpSpan {
  std::u32string text;          /**< Display scalars in this portion. */
  Theme::ColorTypes foreground; /**< Semantic foreground role. */
  Theme::ColorTypes background; /**< Semantic background role. */
};

/** Prepared plain and styled representations of one command-help row. */
struct HelpRow {
  std::string plain_text;      /**< UTF-8 diagnostic representation. */
  std::vector<HelpSpan> spans; /**< Ordered styled display portions. */
};

/** Append one UTF-8 span using semantic palette roles. */
void append_span(HelpRow& row, std::string_view text,
                 Theme::ColorTypes foreground, Theme::ColorTypes background) {
  row.plain_text.append(text);
  row.spans.push_back(HelpSpan{
      .text       = text_editor::decode_utf8(text),
      .foreground = foreground,
      .background = background,
  });
}

}  // namespace

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
  mutable std::shared_mutex help_mutex; /**< Synchronizes prepared help rows. */
  std::vector<HelpRow> help;            /**< Completion or usage rows. */
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

void CmdFrame::set_completions(std::string typed_prefix,
                               std::vector<CmdCompletion> completions,
                               std::size_t selected_completion) {
  std::size_t longest_command = 0U;
  for (const CmdCompletion& completion : completions) {
    longest_command = std::max(
        longest_command, text_editor::decode_utf8(completion.command).size());
  }
  if (!completions.empty()) {
    selected_completion =
        std::min(selected_completion, completions.size() - 1U);
  }

  std::vector<HelpRow> rows;
  rows.reserve(completions.size());
  for (std::size_t index = 0U; index < completions.size(); ++index) {
    const CmdCompletion& completion = completions[index];
    HelpRow row;
    append_span(row, index == selected_completion ? "> " : "  ",
                index == selected_completion ? Theme::ColorTypes::TEXT_SUCCESS
                                             : Theme::ColorTypes::TEXT_MUTED,
                Theme::ColorTypes::BACKGROUND);

    const std::size_t matched_bytes =
        completion.command.starts_with(typed_prefix) ? typed_prefix.size() : 0U;
    append_span(row,
                std::string_view{completion.command}.substr(0U, matched_bytes),
                Theme::ColorTypes::HIGHLIGHT_TEXT,
                Theme::ColorTypes::HIGHLIGHT_BACKGROUND);
    append_span(row, std::string_view{completion.command}.substr(matched_bytes),
                Theme::ColorTypes::TEXT_SECONDARY,
                Theme::ColorTypes::BACKGROUND);

    if (!completion.description.empty()) {
      const std::size_t command_width =
          text_editor::decode_utf8(completion.command).size();
      append_span(
          row,
          std::string(longest_command - command_width + kDescriptionGap, ' '),
          Theme::ColorTypes::TEXT_MUTED, Theme::ColorTypes::BACKGROUND);
      append_span(row, completion.description, Theme::ColorTypes::TERTIARY,
                  Theme::ColorTypes::BACKGROUND);
    }
    rows.push_back(std::move(row));
  }

  const std::unique_lock lock(impl_->help_mutex);
  impl_->help = std::move(rows);
}

void CmdFrame::set_usage(std::vector<std::string> usage_rows) {
  std::vector<HelpRow> rows;
  rows.reserve(usage_rows.size());
  for (std::string& usage : usage_rows) {
    HelpRow row;
    append_span(row, usage, Theme::ColorTypes::TEXT_MUTED,
                Theme::ColorTypes::BACKGROUND);
    rows.push_back(std::move(row));
  }
  const std::unique_lock lock(impl_->help_mutex);
  impl_->help = std::move(rows);
}

void CmdFrame::clear_help() {
  const std::unique_lock lock(impl_->help_mutex);
  impl_->help.clear();
}

std::size_t CmdFrame::help_rows() const noexcept {
  const std::shared_lock lock(impl_->help_mutex);
  return impl_->help.size();
}

std::vector<std::string> CmdFrame::help_text() const {
  const std::shared_lock lock(impl_->help_mutex);
  std::vector<std::string> result;
  result.reserve(impl_->help.size());
  for (const HelpRow& row : impl_->help) {
    result.push_back(row.plain_text);
  }
  return result;
}

Status CmdFrame::draw_help(const Theme& theme, Canvas& canvas,
                           const Canvas::Rect& rect) const {
  if (rect.width == 0U || rect.height == 0U) {
    return Status::OK;
  }

  std::vector<HelpRow> help;
  {
    const std::shared_lock lock(impl_->help_mutex);
    help = impl_->help;
  }
  const std::size_t row_count = std::min(rect.height, help.size());
  for (std::size_t row_index = 0U; row_index < row_count; ++row_index) {
    std::vector<Canvas::Cell> cells;
    cells.reserve(rect.width);
    for (const HelpSpan& span : help[row_index].spans) {
      const std::uint32_t foreground = theme.get_color(span.foreground);
      const std::uint32_t background = theme.get_color(span.background);
      for (const char32_t character : span.text) {
        if (cells.size() == rect.width) {
          break;
        }
        cells.push_back(Canvas::Cell{
            .character        = character,
            .foreground_color = foreground,
            .background_color = background,
        });
      }
      if (cells.size() == rect.width) {
        break;
      }
    }
    if (cells.empty()) {
      continue;
    }
    std::span<Canvas::Cell> row{cells};
    std::span<std::span<Canvas::Cell>> rows{&row, 1U};
    const Status status =
        canvas.write_cells(Canvas::Rect{.x      = rect.x,
                                        .y      = rect.y + row_index,
                                        .width  = cells.size(),
                                        .height = 1U},
                           rows);
    if (!is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
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
