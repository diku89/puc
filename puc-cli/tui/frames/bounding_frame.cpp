/**
 * @file bounding_frame.cpp
 * @brief Colored bounding-box decorator implementation.
 */

#include "puc-cli/tui/frames/bounding_frame.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
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

/** Inset a rectangle without permitting unsigned underflow. */
std::optional<Canvas::Rect> inset(const Canvas::Rect& rect,
                                  const FrameMargins& margins) noexcept {
  if (margins.left > rect.width || margins.right > rect.width - margins.left ||
      margins.top > rect.height || margins.bottom > rect.height - margins.top ||
      margins.left > std::numeric_limits<std::size_t>::max() - rect.x ||
      margins.top > std::numeric_limits<std::size_t>::max() - rect.y) {
    return std::nullopt;
  }
  return Canvas::Rect{
      .x      = rect.x + margins.left,
      .y      = rect.y + margins.top,
      .width  = rect.width - margins.left - margins.right,
      .height = rect.height - margins.top - margins.bottom,
  };
}

/** Construct one fully attributed Canvas cell. */
Canvas::Cell cell(char32_t character, std::uint32_t foreground,
                  std::uint32_t background) noexcept {
  return Canvas::Cell{
      .character        = character,
      .foreground_color = foreground,
      .background_color = background,
  };
}

/** Fill one rectangle with a single cell through Canvas's row-span API. */
Status fill(Canvas& canvas, const Canvas::Rect& rect,
            const Canvas::Cell& value) {
  std::vector<std::vector<Canvas::Cell>> cells(
      rect.height, std::vector<Canvas::Cell>(rect.width, value));
  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(cells.size());
  for (auto& row : cells) {
    rows.emplace_back(row);
  }
  return canvas.write_cells(rect, std::span<std::span<Canvas::Cell>>{rows});
}

/** Convert a practical cell offset to the signed selection coordinate type. */
std::int64_t signed_offset(std::size_t value) noexcept {
  return static_cast<std::int64_t>(std::min<std::size_t>(
      value,
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())));
}

}  // namespace

/** Synchronized configuration and geometry retained by BoundingFrame. */
class BoundingFrame::Impl {
 public:
  /** Retain the required child and its initial bounding policy. */
  Impl(std::shared_ptr<Frame> supplied_child,
       BoundingFrameConfiguration supplied_configuration)
      : child(std::move(supplied_child)),
        configuration(std::move(supplied_configuration)) {}

  /** Compute child geometry from one configuration snapshot. */
  static std::optional<Canvas::Rect> content_rect(
      const Canvas::Rect& rect,
      const BoundingFrameConfiguration& configuration) noexcept {
    std::optional<Canvas::Rect> content =
        inset(rect, configuration.outer_margins);
    if (!content.has_value()) {
      return std::nullopt;
    }
    if (configuration.border_color.has_value()) {
      content = inset(*content, FrameMargins{
                                    .top    = 1U,
                                    .bottom = 1U,
                                    .left   = 1U,
                                    .right  = 1U,
                                });
      if (!content.has_value()) {
        return std::nullopt;
      }
    }
    content = inset(*content, configuration.inner_margins);
    if (!content.has_value() || content->width == 0U || content->height == 0U) {
      return std::nullopt;
    }
    return content;
  }

  std::shared_ptr<Frame> child;    /**< Required decorated child. */
  mutable std::shared_mutex mutex; /**< Synchronizes configuration/geometry. */
  BoundingFrameConfiguration configuration; /**< Active decoration policy. */
  std::size_t child_offset_x = 0U;    /**< Last child-local X translation. */
  std::size_t child_offset_y = 0U;    /**< Last child-local Y translation. */
  bool geometry_valid        = false; /**< Whether offsets came from draw. */
};

BoundingFrame::BoundingFrame(std::string name, std::shared_ptr<Frame> child,
                             BoundingFrameConfiguration configuration)
    : Frame(std::move(name)),
      impl_(
          std::make_unique<Impl>(std::move(child), std::move(configuration))) {}

BoundingFrame::~BoundingFrame() = default;

void BoundingFrame::set_configuration(
    BoundingFrameConfiguration configuration) {
  const std::unique_lock lock(impl_->mutex);
  impl_->configuration  = std::move(configuration);
  impl_->geometry_valid = false;
}

BoundingFrameConfiguration BoundingFrame::configuration() const {
  const std::shared_lock lock(impl_->mutex);
  return impl_->configuration;
}

void BoundingFrame::set_size_constraints(FrameSizeConstraints constraints) {
  const std::unique_lock lock(impl_->mutex);
  impl_->configuration.size_constraints = std::move(constraints);
  impl_->geometry_valid                 = false;
}

std::optional<Canvas::Rect> BoundingFrame::box_rect(
    const Canvas::Rect& rect) const {
  const std::shared_lock lock(impl_->mutex);
  const std::optional<Canvas::Rect> box =
      inset(rect, impl_->configuration.outer_margins);
  if (!box.has_value() || box->width == 0U || box->height == 0U) {
    return std::nullopt;
  }
  return box;
}

std::optional<Canvas::Rect> BoundingFrame::content_rect(
    const Canvas::Rect& rect) const {
  const std::shared_lock lock(impl_->mutex);
  return Impl::content_rect(rect, impl_->configuration);
}

Status BoundingFrame::draw(const Theme& theme, Canvas& canvas,
                           const Canvas::Rect& rect) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  BoundingFrameConfiguration configuration;
  {
    const std::shared_lock lock(impl_->mutex);
    configuration = impl_->configuration;
  }

  const auto [canvas_width, canvas_height] = canvas.get_dimensions();
  const FrameSizeConstraints& constraints  = configuration.size_constraints;
  if (constraints.minimum_width == 0U || constraints.minimum_height == 0U ||
      (constraints.maximum_width.has_value() &&
       *constraints.maximum_width < constraints.minimum_width) ||
      (constraints.maximum_height.has_value() &&
       *constraints.maximum_height < constraints.minimum_height) ||
      rect.width < constraints.minimum_width ||
      rect.height < constraints.minimum_height ||
      (constraints.maximum_width.has_value() &&
       rect.width > *constraints.maximum_width) ||
      (constraints.maximum_height.has_value() &&
       rect.height > *constraints.maximum_height) ||
      (constraints.require_full_canvas_width &&
       (rect.x != 0U || rect.width != canvas_width))) {
    return Status::INVALID_DIMENSIONS;
  }
  if (rect.x > canvas_width || rect.y > canvas_height ||
      rect.width > canvas_width - rect.x ||
      rect.height > canvas_height - rect.y) {
    return Status::RECT_OUT_OF_BOUNDS;
  }

  const std::optional<Canvas::Rect> box =
      inset(rect, configuration.outer_margins);
  const std::optional<Canvas::Rect> content =
      Impl::content_rect(rect, configuration);
  if (!box.has_value() || box->width == 0U || box->height == 0U ||
      !content.has_value()) {
    return Status::INVALID_DIMENSIONS;
  }

  const Theme::Colors colors = theme.get_colors();
  const std::uint32_t inside = theme.get_color(configuration.inside_color);
  const std::uint32_t outside =
      configuration.outside_box_color.has_value()
          ? theme.get_color(*configuration.outside_box_color)
          : colors.background;
  const std::uint32_t border =
      configuration.border_color.has_value()
          ? theme.get_color(*configuration.border_color)
          : colors.text;

  if (configuration.outside_box_color.has_value()) {
    const Status status = fill(canvas, rect, cell(U' ', colors.text, outside));
    if (!is_ok(status)) {
      return status;
    }
  }

  std::vector<std::vector<Canvas::Cell>> box_cells(
      box->height,
      std::vector<Canvas::Cell>(box->width, cell(U' ', colors.text, inside)));
  if (configuration.border_color.has_value()) {
    if (box->width < 2U || box->height < 2U) {
      return Status::INVALID_DIMENSIONS;
    }
    const std::size_t right  = box->width - 1U;
    const std::size_t bottom = box->height - 1U;
    for (std::size_t x = 1U; x < right; ++x) {
      box_cells[0U][x]     = cell(U'─', border, outside);
      box_cells[bottom][x] = cell(U'─', border, outside);
    }
    for (std::size_t y = 1U; y < bottom; ++y) {
      box_cells[y][0U]    = cell(U'│', border, outside);
      box_cells[y][right] = cell(U'│', border, outside);
    }
    box_cells[0U][0U]        = cell(U'┌', border, outside);
    box_cells[0U][right]     = cell(U'┐', border, outside);
    box_cells[bottom][0U]    = cell(U'└', border, outside);
    box_cells[bottom][right] = cell(U'┘', border, outside);
  }

  std::vector<std::span<Canvas::Cell>> rows;
  rows.reserve(box_cells.size());
  for (auto& row : box_cells) {
    rows.emplace_back(row);
  }
  Status status =
      canvas.write_cells(*box, std::span<std::span<Canvas::Cell>>{rows});
  if (!is_ok(status)) {
    return status;
  }

  {
    const std::unique_lock lock(impl_->mutex);
    impl_->child_offset_x = content->x - rect.x;
    impl_->child_offset_y = content->y - rect.y;
    impl_->geometry_valid = true;
  }
  return impl_->child->draw(theme, canvas, *content);
}

bool BoundingFrame::is_selectable() const noexcept {
  return impl_->child != nullptr && impl_->child->is_selectable();
}

Status BoundingFrame::update_selection(const SelectionEvent& event) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  SelectionEvent translated = event;
  {
    const std::shared_lock lock(impl_->mutex);
    if (impl_->geometry_valid) {
      const std::int64_t x = signed_offset(impl_->child_offset_x);
      const std::int64_t y = signed_offset(impl_->child_offset_y);
      translated.anchor.x -= x;
      translated.anchor.y -= y;
      translated.extent.x -= x;
      translated.extent.y -= y;
    }
  }
  return impl_->child->update_selection(translated);
}

Status BoundingFrame::selected_text(std::string& output) const {
  if (impl_->child == nullptr) {
    output.clear();
    return Status::INVALID_ARGUMENT;
  }
  return impl_->child->selected_text(output);
}

bool BoundingFrame::accepts_cursor_placement() const noexcept {
  return impl_->child != nullptr && impl_->child->accepts_cursor_placement();
}

Status BoundingFrame::place_cursor(SelectionPosition position) {
  if (impl_->child == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  {
    const std::shared_lock lock(impl_->mutex);
    if (impl_->geometry_valid) {
      position.x -= signed_offset(impl_->child_offset_x);
      position.y -= signed_offset(impl_->child_offset_y);
    }
  }
  return impl_->child->place_cursor(position);
}

}  // namespace puc::tui
