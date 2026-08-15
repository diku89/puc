/**
 * @file text_input_frame.cpp
 * @brief Editor event routing and text-cell rendering.
 */

#include "puc-cli/tui/frames/text_input_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace puc::tui {

/** Synchronized reusable editor, style, and latest render geometry. */
class TextInputFrame::Impl {
 public:
  /** Construct synchronized editor state with its initial style. */
  Impl(TextInputFrameStyle supplied_style, TextEditorOptions options)
      : editor(options), style(supplied_style) {}

  mutable std::shared_mutex mutex; /**< Synchronizes state and rendering. */
  TextEditor editor;               /**< Reusable logical editor mechanics. */
  TextInputFrameStyle style;       /**< Active semantic color roles. */
  Canvas::Rect viewport{};         /**< Last absolute draw rectangle. */
  bool viewport_valid = false;     /**< Whether scroll hit-testing is valid. */
  std::vector<AnnotatedTextRow> visible_rows; /**< Latest annotation rows. */
};

TextInputFrame::TextInputFrame(std::string name, TextInputFrameStyle style,
                               TextEditorOptions options)
    : AnnotatedTextSource(std::move(name)),
      impl_(std::make_unique<Impl>(style, options)) {}

TextInputFrame::~TextInputFrame() = default;

void TextInputFrame::set_style(TextInputFrameStyle style) {
  const std::unique_lock lock(impl_->mutex);
  impl_->style = style;
}

TextInputFrameStyle TextInputFrame::style() const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->style;
}

Status TextInputFrame::handle_event(const terminal::Event& event) {
  const std::unique_lock lock(impl_->mutex);
  if (const auto* key = std::get_if<terminal::KeyEvent>(&event)) {
    impl_->editor.handle_key(*key);
    return Status::OK;
  }
  if (const auto* text = std::get_if<terminal::TextEvent>(&event)) {
    impl_->editor.handle_text(*text);
    return Status::OK;
  }
  if (const auto* paste = std::get_if<terminal::PasteEvent>(&event)) {
    return impl_->editor.handle_paste(*paste);
  }
  if (const auto* command = std::get_if<terminal::CommandEvent>(&event)) {
    impl_->editor.apply_command(command->command);
    return Status::OK;
  }
  if (const auto* scroll = std::get_if<terminal::ScrollEvent>(&event)) {
    if (impl_->viewport_valid && scroll->delta_y != 0 &&
        scroll->position.x >= impl_->viewport.x &&
        scroll->position.y >= impl_->viewport.y &&
        scroll->position.x - impl_->viewport.x < impl_->viewport.width &&
        scroll->position.y - impl_->viewport.y < impl_->viewport.height) {
      impl_->editor.scroll_vertical(scroll->delta_y);
    }
    return Status::OK;
  }
  return Status::OK;
}

void TextInputFrame::clear() {
  const std::unique_lock lock(impl_->mutex);
  impl_->editor.clear();
  if (impl_->viewport_valid) {
    impl_->editor.set_viewport(impl_->viewport.width, impl_->viewport.height);
  }
  impl_->visible_rows.clear();
}

void TextInputFrame::set_fallback_width(std::size_t width) {
  const std::unique_lock lock(impl_->mutex);
  impl_->editor.set_fallback_width(width);
}

TextEditorSnapshot TextInputFrame::snapshot() const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->editor.snapshot();
}

std::size_t TextInputFrame::preferred_rows(std::size_t width) const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->editor.visual_row_count(width);
}

Status TextInputFrame::draw(const Theme& theme, Canvas& canvas,
                            const Canvas::Rect& rect) {
  if (rect.width == 0U || rect.height == 0U) {
    return Status::INVALID_DIMENSIONS;
  }

  const std::unique_lock lock(impl_->mutex);
  impl_->editor.set_viewport(rect.width, rect.height);
  const TextEditorRenderState rendered = impl_->editor.render_state();
  const std::uint32_t foreground = theme.get_color(impl_->style.text_color);
  const std::uint32_t background =
      theme.get_color(impl_->style.background_color);
  const std::uint32_t cursor = theme.get_color(impl_->style.cursor_color);
  const std::uint32_t selection_foreground =
      theme.get_color(impl_->style.selection_text_color);
  const std::uint32_t selection_background =
      theme.get_color(impl_->style.selection_background_color);

  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height,
      std::vector<Canvas::Cell>(rect.width, Canvas::Cell{
                                                .character        = U' ',
                                                .foreground_color = foreground,
                                                .background_color = background,
                                            }));
  const std::size_t row_count = std::min(rect.height, rendered.rows.size());
  for (std::size_t row = 0U; row < row_count; ++row) {
    const std::size_t column_count =
        std::min(rect.width, rendered.rows[row].cells.size());
    for (std::size_t column = 0U; column < column_count; ++column) {
      const TextEditorRenderCell& source = rendered.rows[row].cells[column];
      cells[row][column]                 = Canvas::Cell{
                          .character = source.character,
                          .foreground_color =
              source.selected ? selection_foreground : foreground,
                          .background_color =
              source.selected ? selection_background : background,
      };
    }
  }
  if (rendered.cursor_row.has_value() && *rendered.cursor_row < rect.height &&
      rendered.cursor_column < rect.width) {
    Canvas::Cell& cursor_cell =
        cells[*rendered.cursor_row][rendered.cursor_column];
    cursor_cell.foreground_color = background;
    cursor_cell.background_color = cursor;
  }

  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  const Status status =
      canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
  if (!is_ok(status)) {
    return status;
  }

  impl_->viewport       = rect;
  impl_->viewport_valid = true;
  impl_->visible_rows.clear();
  impl_->visible_rows.reserve(rendered.rows.size());
  for (const TextEditorRenderRow& row : rendered.rows) {
    impl_->visible_rows.push_back(AnnotatedTextRow{
        .logical_line     = row.logical_line,
        .first_visual_row = row.first_visual_row,
    });
  }
  return Status::OK;
}

bool TextInputFrame::is_selectable() const noexcept { return true; }

Status TextInputFrame::update_selection(const SelectionEvent& event) {
  const std::unique_lock lock(impl_->mutex);
  return impl_->editor.update_selection(event);
}

Status TextInputFrame::selected_text(std::string& output) const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->editor.selected_text(output);
}

bool TextInputFrame::accepts_cursor_placement() const noexcept { return true; }

Status TextInputFrame::place_cursor(SelectionPosition position) {
  const std::unique_lock lock(impl_->mutex);
  return impl_->editor.place_cursor(position);
}

std::size_t TextInputFrame::logical_line_count() const noexcept {
  const std::shared_lock lock(impl_->mutex);
  return impl_->editor.logical_line_count();
}

std::vector<AnnotatedTextRow> TextInputFrame::visible_text_rows() const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->visible_rows;
}

}  // namespace puc::tui
