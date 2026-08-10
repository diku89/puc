/**
 * @file annotated_text_frame.cpp
 * @brief Line-number and status-gutter decorator implementation.
 */

#include "puc-cli/tui/annotated_text_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace puc::tui {
namespace {

/** Return the decimal column count needed by a positive value. */
std::size_t decimal_columns(std::size_t value) noexcept {
  std::size_t columns = 1U;
  while (value >= 10U) {
    value /= 10U;
    ++columns;
  }
  return columns;
}

/** Convert a practical gutter width to signed selection coordinates. */
std::int64_t signed_width(std::size_t value) noexcept {
  return static_cast<std::int64_t>(std::min<std::size_t>(
      value,
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
}

}  // namespace

/** Synchronized gutter policy, markers, and most recent geometry. */
class AnnotatedTextFrame::Impl {
 public:
  /** Retain the required text source and its initial annotation policy. */
  Impl(std::shared_ptr<AnnotatedTextSource> supplied_child,
       AnnotatedTextConfiguration supplied_configuration)
      : child(std::move(supplied_child)),
        configuration(std::move(supplied_configuration)) {}

  /** Compute the active gutter width from a configuration snapshot. */
  static std::size_t gutter_width(
      const AnnotatedTextConfiguration& configuration,
      std::size_t logical_lines) noexcept {
    const std::size_t line_columns =
        configuration.show_line_numbers
            ? std::max(
                  configuration.minimum_line_number_columns,
                  decimal_columns(std::max<std::size_t>(logical_lines, 1U)))
            : 0U;
    const std::size_t requested =
        configuration.status_columns >
                std::numeric_limits<std::size_t>::max() - line_columns
            ? std::numeric_limits<std::size_t>::max()
            : configuration.status_columns + line_columns;
    const std::size_t with_separator =
        requested > std::numeric_limits<std::size_t>::max() -
                        configuration.separator_columns
            ? std::numeric_limits<std::size_t>::max()
            : requested + configuration.separator_columns;
    return std::max(configuration.minimum_gutter_width, with_separator);
  }

  std::shared_ptr<AnnotatedTextSource> child; /**< Required wrapped source. */
  mutable std::shared_mutex mutex; /**< Synchronizes policy and marker state. */
  AnnotatedTextConfiguration configuration; /**< Active gutter policy. */
  std::map<std::size_t, AnnotatedLineStatus> statuses; /**< Markers by line. */
  std::size_t last_gutter_width = 0U;    /**< Translation from latest draw. */
  bool geometry_valid           = false; /**< Whether translation is current. */
};

AnnotatedTextFrame::AnnotatedTextFrame(
    std::string name, std::shared_ptr<AnnotatedTextSource> child,
    AnnotatedTextConfiguration configuration)
    : Frame(std::move(name)),
      impl_(
          std::make_unique<Impl>(std::move(child), std::move(configuration))) {}

AnnotatedTextFrame::~AnnotatedTextFrame() = default;

void AnnotatedTextFrame::set_configuration(
    AnnotatedTextConfiguration configuration) {
  const std::unique_lock lock(impl_->mutex);
  impl_->configuration  = std::move(configuration);
  impl_->geometry_valid = false;
}

AnnotatedTextConfiguration AnnotatedTextFrame::configuration() const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->configuration;
}

void AnnotatedTextFrame::set_statuses(
    std::vector<AnnotatedLineStatus> statuses) {
  const std::unique_lock lock(impl_->mutex);
  impl_->statuses.clear();
  for (AnnotatedLineStatus& status : statuses) {
    impl_->statuses[status.logical_line] = std::move(status);
  }
}

void AnnotatedTextFrame::clear_statuses() {
  const std::unique_lock lock(impl_->mutex);
  impl_->statuses.clear();
}

std::size_t AnnotatedTextFrame::gutter_width() const noexcept {
  AnnotatedTextConfiguration configuration;
  {
    const std::shared_lock lock(impl_->mutex);
    configuration = impl_->configuration;
  }
  const std::size_t lines =
      impl_->child == nullptr ? 0U : impl_->child->logical_line_count();
  return Impl::gutter_width(configuration, lines);
}

std::optional<Canvas::Rect> AnnotatedTextFrame::content_rect(
    const Canvas::Rect& rect) const {
  const std::size_t gutter = gutter_width();
  if (gutter >= rect.width || rect.height == 0U ||
      gutter > std::numeric_limits<std::size_t>::max() - rect.x) {
    return std::nullopt;
  }
  return Canvas::Rect{
      .x      = rect.x + gutter,
      .y      = rect.y,
      .width  = rect.width - gutter,
      .height = rect.height,
  };
}

Status AnnotatedTextFrame::draw(const Theme& theme, Canvas& canvas,
                                const Canvas::Rect& rect) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }

  AnnotatedTextConfiguration configuration;
  std::map<std::size_t, AnnotatedLineStatus> statuses;
  {
    const std::shared_lock lock(impl_->mutex);
    configuration = impl_->configuration;
    statuses      = impl_->statuses;
  }
  const std::size_t logical_lines = impl_->child->logical_line_count();
  const std::size_t gutter = Impl::gutter_width(configuration, logical_lines);
  if (rect.height == 0U || gutter >= rect.width ||
      gutter > std::numeric_limits<std::size_t>::max() - rect.x) {
    return Status::INVALID_DIMENSIONS;
  }
  const Canvas::Rect child_rect{
      .x      = rect.x + gutter,
      .y      = rect.y,
      .width  = rect.width - gutter,
      .height = rect.height,
  };
  Status status = impl_->child->draw(theme, canvas, child_rect);
  if (!is_ok(status)) {
    return status;
  }

  {
    const std::unique_lock lock(impl_->mutex);
    impl_->last_gutter_width = gutter;
    impl_->geometry_valid    = true;
  }
  if (gutter == 0U) {
    return Status::OK;
  }

  const Theme::Colors colors = theme.get_colors();
  const std::uint32_t background =
      theme.get_color(configuration.background_color);
  const std::uint32_t line_color =
      theme.get_color(configuration.line_number_color);
  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height,
      std::vector<Canvas::Cell>(gutter, Canvas::Cell{
                                            .character        = U' ',
                                            .foreground_color = colors.text,
                                            .background_color = background,
                                        }));

  const std::size_t line_columns =
      configuration.show_line_numbers
          ? std::max(configuration.minimum_line_number_columns,
                     decimal_columns(std::max<std::size_t>(logical_lines, 1U)))
          : 0U;
  const std::vector<AnnotatedTextRow> visible =
      impl_->child->visible_text_rows();
  const std::size_t row_count = std::min(rect.height, visible.size());
  for (std::size_t row_index = 0U; row_index < row_count; ++row_index) {
    const AnnotatedTextRow& row = visible[row_index];
    if (!row.first_visual_row) {
      continue;
    }

    const auto marker = statuses.find(row.logical_line);
    if (marker != statuses.end()) {
      const std::size_t count =
          std::min(configuration.status_columns, marker->second.text.size());
      const std::uint32_t marker_color = theme.get_color(marker->second.color);
      for (std::size_t column = 0U; column < count; ++column) {
        cells[row_index][column] = Canvas::Cell{
            .character        = marker->second.text[column],
            .foreground_color = marker_color,
            .background_color = background,
        };
      }
    }

    if (configuration.show_line_numbers && logical_lines != 0U &&
        row.logical_line < logical_lines) {
      const std::string number = std::to_string(row.logical_line + 1U);
      const std::size_t count  = std::min(line_columns, number.size());
      const std::size_t first =
          configuration.status_columns + line_columns - count;
      for (std::size_t column = 0U; column < count; ++column) {
        if (first + column >= gutter) {
          break;
        }
        cells[row_index][first + column] = Canvas::Cell{
            .character = static_cast<unsigned char>(
                number[number.size() - count + column]),
            .foreground_color = line_color,
            .background_color = background,
        };
      }
    }
  }

  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(
      Canvas::Rect{
          .x = rect.x, .y = rect.y, .width = gutter, .height = rect.height},
      std::span<std::span<Canvas::Cell>>{rows});
}

bool AnnotatedTextFrame::is_selectable() const noexcept {
  return impl_->child != nullptr && impl_->child->is_selectable();
}

Status AnnotatedTextFrame::update_selection(const SelectionEvent& event) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  SelectionEvent translated = event;
  {
    const std::shared_lock lock(impl_->mutex);
    if (impl_->geometry_valid) {
      const std::int64_t gutter = signed_width(impl_->last_gutter_width);
      translated.anchor.x -= gutter;
      translated.extent.x -= gutter;
    }
  }
  return impl_->child->update_selection(translated);
}

Status AnnotatedTextFrame::selected_text(std::string& output) const {
  if (impl_->child == nullptr) {
    output.clear();
    return Status::INVALID_ARGUMENT;
  }
  return impl_->child->selected_text(output);
}

bool AnnotatedTextFrame::accepts_cursor_placement() const noexcept {
  return impl_->child != nullptr && impl_->child->accepts_cursor_placement();
}

Status AnnotatedTextFrame::place_cursor(SelectionPosition position) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  {
    const std::shared_lock lock(impl_->mutex);
    if (impl_->geometry_valid) {
      const std::int64_t gutter = signed_width(impl_->last_gutter_width);
      if (position.x < gutter) {
        return Status::OK;
      }
      position.x -= gutter;
    }
  }
  return impl_->child->place_cursor(position);
}

}  // namespace puc::tui
