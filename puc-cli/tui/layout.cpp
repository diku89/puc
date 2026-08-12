/**
 * @file layout.cpp
 * @brief Constraint validation, dependency resolution, sizing, and composition.
 */

#include "puc-cli/tui/layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "puc-cli/tui/status.hpp"
#include "utils/logger/logger.hpp"

/** @cond TUI_LOGGER_MODULE */
LOGGER_MODULE("TUI Layout");
/** @endcond */

namespace puc {
namespace tui {
namespace {

/** Number of valid Layout::ConstraintType enumerators. */
constexpr size_t kConstraintTypeCount =
    static_cast<size_t>(Layout::ConstraintType::VERTICAL_CENTER) + 1;

/**
 * Validated, constant-time lookup view over one frame's constraints.
 *
 * Pointers refer to values owned by LayoutDescription and remain valid for one
 * solve because the description is not mutated during parsing or resolution.
 */
struct ParsedConstraints {
  /** Constraint pointer indexed by its ConstraintType value. */
  std::array<const Layout::Constraint*, kConstraintTypeCount> values{};

  /**
   * Look up a constraint by type.
   *
   * @param[in] type Constraint type to retrieve.
   * @return The constraint, or `nullptr` if absent or outside the valid enum.
   */
  const Layout::Constraint* get(Layout::ConstraintType type) const noexcept {
    const size_t index = static_cast<size_t>(type);
    if (index >= values.size()) {
      return nullptr;
    }
    return values[index];
  }
};

/** Parsed constraints indexed by frame id. */
using ParsedLayout = std::map<std::string, ParsedConstraints>;

/** Depth-first state used to resolve named frame dependencies. */
enum class VisitState {
  UNVISITED, /**< Frame has not entered dependency traversal. */
  VISITING,  /**< Frame is on the active dependency path. */
  RESOLVED,  /**< Frame has a rectangle in the output map. */
};

/** Return whether a constraint type selects horizontal placement. */
bool is_horizontal_placement(Layout::ConstraintType type) noexcept {
  return type == Layout::ConstraintType::LEFT_ANCHOR ||
         type == Layout::ConstraintType::RIGHT_ANCHOR ||
         type == Layout::ConstraintType::HORIZONTAL_CENTER;
}

/** Return whether a constraint type selects vertical placement. */
bool is_vertical_placement(Layout::ConstraintType type) noexcept {
  return type == Layout::ConstraintType::TOP_ANCHOR ||
         type == Layout::ConstraintType::BOTTOM_ANCHOR ||
         type == Layout::ConstraintType::VERTICAL_CENTER;
}

/** Return whether a constraint can align with an edge of a named frame. */
bool is_edge_anchor(Layout::ConstraintType type) noexcept {
  return type == Layout::ConstraintType::LEFT_ANCHOR ||
         type == Layout::ConstraintType::RIGHT_ANCHOR ||
         type == Layout::ConstraintType::TOP_ANCHOR ||
         type == Layout::ConstraintType::BOTTOM_ANCHOR;
}

/**
 * Test whether the description's Z-buffer contains an id.
 *
 * @param[in] description Layout whose owned frames are searched.
 * @param[in] frame_id Identifier to find.
 * @return `true` if exactly such an entry exists.
 */
bool frame_exists(const Layout::LayoutDescription& description,
                  std::string_view frame_id) noexcept {
  return std::any_of(
      description.z_buffer.frames().begin(),
      description.z_buffer.frames().end(),
      [&](const ZBuffer::Entry& entry) { return entry.frame_id == frame_id; });
}

/**
 * Validate one constraint's enum values, unit, and variant alternative.
 *
 * Cross-constraint conflicts are handled by `parse_constraints()`.
 *
 * @param[in] constraint Constraint to inspect.
 * @return Status::OK or the applicable constraint, percentage, ratio, or
 *         argument error.
 */
Status validate_constraint(const Layout::Constraint& constraint) noexcept {
  const size_t type_index = static_cast<size_t>(constraint.type);
  if (type_index >= kConstraintTypeCount) {
    return Status::INVALID_CONSTRAINT;
  }

  switch (constraint.unit) {
    case Layout::Unit::CHARACTERS:
      if (constraint.type == Layout::ConstraintType::ASPECT_RATIO ||
          !std::holds_alternative<size_t>(constraint.value)) {
        return Status::INVALID_CONSTRAINT;
      }
      return Status::OK;

    case Layout::Unit::PERCENT: {
      if (constraint.type == Layout::ConstraintType::ASPECT_RATIO) {
        return Status::INVALID_CONSTRAINT;
      }
      const float* percentage = std::get_if<float>(&constraint.value);
      if (percentage == nullptr || !std::isfinite(*percentage) ||
          *percentage < 0.0F || *percentage > 1.0F) {
        return Status::INVALID_PERCENTAGE;
      }
      return Status::OK;
    }

    case Layout::Unit::RATIO: {
      if (constraint.type != Layout::ConstraintType::ASPECT_RATIO) {
        return Status::INVALID_CONSTRAINT;
      }
      const auto* ratio = std::get_if<Layout::AspectRatio>(&constraint.value);
      if (ratio == nullptr || ratio->width <= 0 || ratio->height <= 0) {
        return Status::INVALID_RATIO;
      }
      return Status::OK;
    }

    case Layout::Unit::NAME: {
      if (!is_edge_anchor(constraint.type)) {
        return Status::INVALID_CONSTRAINT;
      }
      const std::string* name = std::get_if<std::string>(&constraint.value);
      if (name == nullptr || name->empty()) {
        return Status::INVALID_ARGUMENT;
      }
      return Status::OK;
    }
  }

  return Status::INVALID_CONSTRAINT;
}

/**
 * Reject contradictory minimum and maximum values expressed in the same unit.
 *
 * Mixed-unit bounds depend on screen dimensions and are resolved later.
 *
 * @param[in] minimum Optional minimum constraint.
 * @param[in] maximum Optional maximum constraint.
 * @return Status::OK or Status::INVALID_CONSTRAINT.
 */
Status validate_bound_pair(const Layout::Constraint* minimum,
                           const Layout::Constraint* maximum) noexcept {
  if (minimum == nullptr || maximum == nullptr ||
      minimum->unit != maximum->unit) {
    return Status::OK;
  }

  if (minimum->unit == Layout::Unit::CHARACTERS) {
    const size_t* minimum_value = std::get_if<size_t>(&minimum->value);
    const size_t* maximum_value = std::get_if<size_t>(&maximum->value);
    if (minimum_value == nullptr || maximum_value == nullptr ||
        *minimum_value > *maximum_value) {
      return Status::INVALID_CONSTRAINT;
    }
  }

  if (minimum->unit == Layout::Unit::PERCENT) {
    const float* minimum_value = std::get_if<float>(&minimum->value);
    const float* maximum_value = std::get_if<float>(&maximum->value);
    if (minimum_value == nullptr || maximum_value == nullptr ||
        *minimum_value > *maximum_value) {
      return Status::INVALID_CONSTRAINT;
    }
  }

  return Status::OK;
}

/**
 * Validate and index one frame's complete constraint list.
 *
 * Duplicate types and multiple placements on the same axis are ambiguous and
 * therefore rejected.
 *
 * @param[in] constraints Source constraints owned by the description.
 * @param[out] parsed Receives pointers indexed by constraint type.
 * @return Status::OK or the first validation error.
 */
Status parse_constraints(const std::vector<Layout::Constraint>& constraints,
                         ParsedConstraints& parsed) noexcept {
  parsed                       = ParsedConstraints{};
  size_t horizontal_placements = 0;
  size_t vertical_placements   = 0;

  for (const Layout::Constraint& constraint : constraints) {
    const Status validation = validate_constraint(constraint);
    if (!is_ok(validation)) {
      return validation;
    }

    const size_t index = static_cast<size_t>(constraint.type);
    if (parsed.values[index] != nullptr) {
      return Status::INVALID_CONSTRAINT;
    }
    parsed.values[index] = &constraint;

    if (is_horizontal_placement(constraint.type)) {
      ++horizontal_placements;
    }
    if (is_vertical_placement(constraint.type)) {
      ++vertical_placements;
    }
  }

  if (horizontal_placements > 1 || vertical_placements > 1) {
    return Status::INVALID_CONSTRAINT;
  }

  Status status =
      validate_bound_pair(parsed.get(Layout::ConstraintType::MIN_WIDTH),
                          parsed.get(Layout::ConstraintType::MAX_WIDTH));
  if (!is_ok(status)) {
    return status;
  }
  return validate_bound_pair(parsed.get(Layout::ConstraintType::MIN_HEIGHT),
                             parsed.get(Layout::ConstraintType::MAX_HEIGHT));
}

/**
 * Validate every frame and build the immutable lookup form used by the solver.
 *
 * @param[in] description Layout description to parse.
 * @param[out] parsed_layout Receives one ParsedConstraints value per frame.
 * @return Status::OK, a constraint validation error, or
 *         Status::FRAME_NOT_FOUND for orphaned constraint entries.
 */
Status parse_layout(const Layout::LayoutDescription& description,
                    ParsedLayout& parsed_layout) {
  parsed_layout.clear();

  for (const ZBuffer::Entry& entry : description.z_buffer.frames()) {
    ParsedConstraints parsed;
    const auto constraints = description.constraints.find(entry.frame_id);
    if (constraints != description.constraints.end()) {
      const Status status = parse_constraints(constraints->second, parsed);
      if (!is_ok(status)) {
        Logger<ERROR> << "Invalid constraints for frame '" << entry.frame_id
                      << "': " << status_message(status);
        return status;
      }
    }
    parsed_layout.emplace(entry.frame_id, parsed);
  }

  for (const auto& [frame_id, constraints] : description.constraints) {
    static_cast<void>(constraints);
    if (!frame_exists(description, frame_id)) {
      Logger<ERROR> << "Layout contains constraints for unknown frame '"
                    << frame_id << "'";
      return Status::FRAME_NOT_FOUND;
    }
  }

  return Status::OK;
}

/**
 * Resolve an absolute or percentage measurement against one screen axis.
 *
 * Percentage products round down to whole terminal cells. A percentage of one
 * is handled explicitly so floating-point representation cannot lose the last
 * cell.
 *
 * @param[in] constraint Optional CHARACTERS or PERCENT constraint.
 * @param[in] extent Full screen extent on the relevant axis.
 * @param[in] default_value Value used when `constraint` is null.
 * @param[out] value Receives the resolved cell count.
 * @return Status::OK or a unit/value validation error.
 */
Status resolve_measure(const Layout::Constraint* constraint, size_t extent,
                       size_t default_value, size_t& value) noexcept {
  if (constraint == nullptr) {
    value = default_value;
    return Status::OK;
  }

  if (constraint->unit == Layout::Unit::CHARACTERS) {
    const size_t* characters = std::get_if<size_t>(&constraint->value);
    if (characters == nullptr) {
      return Status::INVALID_CONSTRAINT;
    }
    value = *characters;
    return Status::OK;
  }

  if (constraint->unit == Layout::Unit::PERCENT) {
    const float* percentage = std::get_if<float>(&constraint->value);
    if (percentage == nullptr || !std::isfinite(*percentage) ||
        *percentage < 0.0F || *percentage > 1.0F) {
      return Status::INVALID_PERCENTAGE;
    }
    if (*percentage == 1.0F) {
      value = extent;
      return Status::OK;
    }
    value = static_cast<size_t>(static_cast<long double>(extent) *
                                static_cast<long double>(*percentage));
    return Status::OK;
  }

  return Status::INVALID_CONSTRAINT;
}

/** Integer rounding policy for aspect-ratio dimension conversion. */
enum class DimensionRounding {
  DOWN, /**< Stay within available bounds. */
  UP,   /**< Satisfy a declared minimum. */
};

/**
 * Convert a cell count on one visual axis into a count on the other axis.
 *
 * The calculation applies both the requested visual aspect ratio and the
 * physical proportions of a terminal cell, using `long double` to avoid
 * intermediate integer overflow.
 *
 * @param[in] source_cells Known cell count on the source axis.
 * @param[in] source_cell_extent Physical size of one source-axis cell.
 * @param[in] source_ratio_extent Requested visual size on the source axis.
 * @param[in] target_cell_extent Physical size of one target-axis cell.
 * @param[in] target_ratio_extent Requested visual size on the target axis.
 * @param[in] rounding Whether to round the result down or up.
 * @param[out] result Receives the target-axis cell count, or zero on error.
 * @return Status::OK, Status::INVALID_DIMENSIONS, Status::INVALID_RATIO, or
 *         Status::DIMENSION_OVERFLOW.
 */
Status checked_derived_dimension(size_t source_cells, size_t source_cell_extent,
                                 int32_t source_ratio_extent,
                                 size_t target_cell_extent,
                                 int32_t target_ratio_extent,
                                 DimensionRounding rounding,
                                 size_t& result) noexcept {
  if (source_cell_extent == 0 || target_cell_extent == 0) {
    result = 0;
    return Status::INVALID_DIMENSIONS;
  }
  if (source_ratio_extent <= 0 || target_ratio_extent <= 0) {
    result = 0;
    return Status::INVALID_RATIO;
  }

  const long double scaled = static_cast<long double>(source_cells) *
                             static_cast<long double>(source_cell_extent) *
                             static_cast<long double>(target_ratio_extent) /
                             (static_cast<long double>(target_cell_extent) *
                              static_cast<long double>(source_ratio_extent));
  const long double rounded = rounding == DimensionRounding::UP
                                  ? std::ceil(scaled)
                                  : std::floor(scaled);
  if (!std::isfinite(rounded) ||
      rounded > static_cast<long double>(std::numeric_limits<size_t>::max())) {
    result = 0;
    return Status::DIMENSION_OVERFLOW;
  }
  result = static_cast<size_t>(rounded);
  return Status::OK;
}

/** Derive rows from columns for a visual aspect ratio. */
Status height_for_width(size_t width, const Layout::AspectRatio& ratio,
                        const CellDimensions& cell_dimensions,
                        DimensionRounding rounding, size_t& height) noexcept {
  return checked_derived_dimension(width, cell_dimensions.width, ratio.width,
                                   cell_dimensions.height, ratio.height,
                                   rounding, height);
}

/** Derive columns from rows for a visual aspect ratio. */
Status width_for_height(size_t height, const Layout::AspectRatio& ratio,
                        const CellDimensions& cell_dimensions,
                        DimensionRounding rounding, size_t& width) noexcept {
  return checked_derived_dimension(height, cell_dimensions.height, ratio.height,
                                   cell_dimensions.width, ratio.width, rounding,
                                   width);
}

/**
 * Reduce the available frame rectangle to its requested visual aspect ratio.
 *
 * Width is retained when its derived height fits; otherwise height is retained
 * and width is reduced. A non-empty available rectangle never collapses a
 * derived dimension below one cell.
 *
 * @param[in] constraints Parsed constraints for one frame.
 * @param[in,out] width Available width, reduced if required.
 * @param[in,out] height Available height, reduced if required.
 * @param[in] cell_dimensions Relative physical terminal-cell dimensions.
 * @return Status::OK or an aspect-ratio conversion error.
 */
Status apply_aspect_ratio(const ParsedConstraints& constraints, size_t& width,
                          size_t& height,
                          const CellDimensions& cell_dimensions) noexcept {
  const Layout::Constraint* aspect_ratio =
      constraints.get(Layout::ConstraintType::ASPECT_RATIO);
  if (aspect_ratio == nullptr) {
    return Status::OK;
  }

  const auto* ratio = std::get_if<Layout::AspectRatio>(&aspect_ratio->value);
  if (ratio == nullptr || ratio->width <= 0 || ratio->height <= 0) {
    return Status::INVALID_RATIO;
  }

  size_t height_from_width = 0;
  Status status            = height_for_width(width, *ratio, cell_dimensions,
                                              DimensionRounding::DOWN, height_from_width);
  if (!is_ok(status)) {
    return status;
  }

  if (width > 0 && height > 0 && height_from_width == 0) {
    height_from_width = 1;
  }
  if (height_from_width <= height) {
    height = height_from_width;
    return Status::OK;
  }

  size_t width_from_height = 0;
  status                   = width_for_height(height, *ratio, cell_dimensions,
                                              DimensionRounding::DOWN, width_from_height);
  if (!is_ok(status)) {
    return status;
  }
  if (width > 0 && height > 0 && width_from_height == 0) {
    width_from_height = 1;
  }
  width = std::min(width, width_from_height);
  return Status::OK;
}

/**
 * Resolve the half-open bounds left after margins on one axis.
 *
 * Margins are clamped to the screen. Crossing margins collapse the available
 * interval to zero cells instead of underflowing.
 *
 * @param[in] constraints Parsed constraints for one frame.
 * @param[in] horizontal Select width/left/right instead of height/top/bottom.
 * @param[in] screen_extent Full cell count on the selected axis.
 * @param[out] lower Receives the inclusive lower bound.
 * @param[out] upper Receives the exclusive upper bound.
 * @return Status::OK or a measurement error.
 */
Status resolve_axis_bounds(const ParsedConstraints& constraints,
                           bool horizontal, size_t screen_extent, size_t& lower,
                           size_t& upper) noexcept {
  const Layout::ConstraintType lower_type =
      horizontal ? Layout::ConstraintType::LEFT_MARGIN
                 : Layout::ConstraintType::TOP_MARGIN;
  const Layout::ConstraintType upper_type =
      horizontal ? Layout::ConstraintType::RIGHT_MARGIN
                 : Layout::ConstraintType::BOTTOM_MARGIN;

  size_t lower_margin = 0;
  Status status = resolve_measure(constraints.get(lower_type), screen_extent, 0,
                                  lower_margin);
  if (!is_ok(status)) {
    return status;
  }
  size_t upper_margin = 0;
  status = resolve_measure(constraints.get(upper_type), screen_extent, 0,
                           upper_margin);
  if (!is_ok(status)) {
    return status;
  }

  lower = std::min(lower_margin, screen_extent);
  upper = screen_extent - std::min(upper_margin, screen_extent);
  if (upper < lower) {
    upper = lower;
  }
  return Status::OK;
}

/**
 * Find the sole validated placement constraint for one axis.
 *
 * @param[in] constraints Parsed constraints for one frame.
 * @param[in] horizontal Select horizontal rather than vertical placement.
 * @return The placement constraint, or `nullptr` for default lower anchoring.
 */
const Layout::Constraint* placement_constraint(
    const ParsedConstraints& constraints, bool horizontal) noexcept {
  const std::array horizontal_types{
      Layout::ConstraintType::LEFT_ANCHOR,
      Layout::ConstraintType::RIGHT_ANCHOR,
      Layout::ConstraintType::HORIZONTAL_CENTER,
  };
  const std::array vertical_types{
      Layout::ConstraintType::TOP_ANCHOR,
      Layout::ConstraintType::BOTTOM_ANCHOR,
      Layout::ConstraintType::VERTICAL_CENTER,
  };

  if (horizontal) {
    for (const Layout::ConstraintType type : horizontal_types) {
      if (const Layout::Constraint* constraint = constraints.get(type)) {
        return constraint;
      }
    }
    return nullptr;
  }

  for (const Layout::ConstraintType type : vertical_types) {
    if (const Layout::Constraint* constraint = constraints.get(type)) {
      return constraint;
    }
  }
  return nullptr;
}

/**
 * Clamp an origin so its frame extent remains within half-open axis bounds.
 *
 * @return The clamped origin, or `lower` if the frame cannot fit.
 */
size_t clamp_origin(size_t origin, size_t lower, size_t upper,
                    size_t frame_extent) noexcept {
  if (upper < lower || frame_extent > upper - lower) {
    return lower;
  }
  return std::clamp(origin, lower, upper - frame_extent);
}

/**
 * Resolve one frame origin from an offset-based or named-edge placement.
 *
 * Numeric lower-edge offsets move inward from the lower bound, upper-edge
 * offsets move inward from the upper bound, and center offsets move right or
 * down from exact centering. Named anchors align corresponding frame edges.
 * The final result is always clamped to the margin bounds.
 *
 * @param[in] placement Optional placement constraint.
 * @param[in] horizontal Select x/width rather than y/height semantics.
 * @param[in] screen_extent Full screen extent for percentage offsets.
 * @param[in] lower Inclusive margin bound.
 * @param[in] upper Exclusive margin bound.
 * @param[in] frame_extent Resolved frame size on this axis.
 * @param[in] reference Solved referenced frame for a NAME constraint.
 * @param[out] origin Receives the clamped frame origin.
 * @return Status::OK or a placement/reference/measurement error.
 */
Status resolve_axis_origin(const Layout::Constraint* placement, bool horizontal,
                           size_t screen_extent, size_t lower, size_t upper,
                           size_t frame_extent, const Canvas::Rect* reference,
                           size_t& origin) noexcept {
  if (placement == nullptr) {
    origin = lower;
    return Status::OK;
  }

  if (placement->unit == Layout::Unit::NAME) {
    if (reference == nullptr) {
      return Status::FRAME_NOT_FOUND;
    }

    if (placement->type == Layout::ConstraintType::LEFT_ANCHOR) {
      origin = reference->x;
    } else if (placement->type == Layout::ConstraintType::RIGHT_ANCHOR) {
      const size_t reference_edge =
          reference->width > std::numeric_limits<size_t>::max() - reference->x
              ? std::numeric_limits<size_t>::max()
              : reference->x + reference->width;
      origin =
          frame_extent > reference_edge ? 0 : reference_edge - frame_extent;
    } else if (placement->type == Layout::ConstraintType::TOP_ANCHOR) {
      origin = reference->y;
    } else if (placement->type == Layout::ConstraintType::BOTTOM_ANCHOR) {
      const size_t reference_edge =
          reference->height > std::numeric_limits<size_t>::max() - reference->y
              ? std::numeric_limits<size_t>::max()
              : reference->y + reference->height;
      origin =
          frame_extent > reference_edge ? 0 : reference_edge - frame_extent;
    } else {
      return Status::INVALID_CONSTRAINT;
    }
    origin = clamp_origin(origin, lower, upper, frame_extent);
    return Status::OK;
  }

  size_t offset = 0;
  Status status = resolve_measure(placement, screen_extent, 0, offset);
  if (!is_ok(status)) {
    return status;
  }

  const bool lower_anchor =
      placement->type == Layout::ConstraintType::LEFT_ANCHOR ||
      placement->type == Layout::ConstraintType::TOP_ANCHOR;
  const bool upper_anchor =
      placement->type == Layout::ConstraintType::RIGHT_ANCHOR ||
      placement->type == Layout::ConstraintType::BOTTOM_ANCHOR;
  const bool center =
      placement->type == Layout::ConstraintType::HORIZONTAL_CENTER ||
      placement->type == Layout::ConstraintType::VERTICAL_CENTER;

  if (lower_anchor) {
    origin = offset > std::numeric_limits<size_t>::max() - lower
                 ? std::numeric_limits<size_t>::max()
                 : lower + offset;
  } else if (upper_anchor) {
    const size_t needed =
        offset > std::numeric_limits<size_t>::max() - frame_extent
            ? std::numeric_limits<size_t>::max()
            : offset + frame_extent;
    origin = needed > upper ? lower : upper - needed;
  } else if (center) {
    const size_t available = upper - lower;
    const size_t centered  = frame_extent > available
                                 ? lower
                                 : lower + (available - frame_extent) / 2;
    origin = offset > std::numeric_limits<size_t>::max() - centered
                 ? std::numeric_limits<size_t>::max()
                 : centered + offset;
  } else {
    return Status::INVALID_CONSTRAINT;
  }

  origin = clamp_origin(origin, lower, upper, frame_extent);
  return Status::OK;
}

/**
 * Resolves parsed frame constraints and named-anchor dependencies.
 *
 * The resolver performs a depth-first traversal. Encountering a VISITING frame
 * proves a named-anchor cycle; a RESOLVED frame can be reused by any number of
 * dependents. Successful rectangles are written directly into `output_`.
 */
class Resolver {
 public:
  /**
   * Construct a resolver over data that outlives `resolve_all()`.
   *
   * @param[in] description Source frame ownership and order.
   * @param[in] parsed_layout Validated constraint lookup.
   * @param[in] screen_width Screen columns.
   * @param[in] screen_height Screen rows.
   * @param[in] cell_dimensions Relative physical cell dimensions.
   * @param[out] output Destination for solved rectangles.
   */
  Resolver(const Layout::LayoutDescription& description,
           const ParsedLayout& parsed_layout, size_t screen_width,
           size_t screen_height, const CellDimensions& cell_dimensions,
           Layout::AbsoluteLayout& output,
           std::vector<std::string>& resolution_order)
      : description_(description),
        parsed_layout_(parsed_layout),
        screen_width_(screen_width),
        screen_height_(screen_height),
        cell_dimensions_(cell_dimensions),
        output_(output),
        resolution_order_(resolution_order) {
    for (const ZBuffer::Entry& entry : description_.z_buffer.frames()) {
      visit_states_.emplace(entry.frame_id, VisitState::UNVISITED);
    }
  }

  /**
   * Resolve every frame, including any named dependencies encountered early.
   *
   * @return Status::OK or the first resolution error.
   */
  Status resolve_all(const std::vector<std::string>* cached_order = nullptr) {
    if (cached_order != nullptr) {
      for (const std::string& frame_id : *cached_order) {
        const Status status = resolve(frame_id);
        if (!is_ok(status)) {
          return status;
        }
      }
      return Status::OK;
    }

    for (const ZBuffer::Entry& entry : description_.z_buffer.frames()) {
      const Status status = resolve(entry.frame_id);
      if (!is_ok(status)) {
        return status;
      }
    }
    return Status::OK;
  }

 private:
  /**
   * Resolve one frame and recursively solve frames referenced by its anchors.
   *
   * Size is chosen from margin bounds and maximum constraints, then reduced to
   * the aspect ratio. Placement is resolved only after referenced rectangles
   * are available.
   *
   * @param[in] frame_id Frame id to solve.
   * @return Status::OK or a lookup, cycle, measurement, or placement error.
   */
  Status resolve(const std::string& frame_id) {
    auto visit = visit_states_.find(frame_id);
    if (visit == visit_states_.end()) {
      return Status::FRAME_NOT_FOUND;
    }
    if (visit->second == VisitState::RESOLVED) {
      return Status::OK;
    }
    if (visit->second == VisitState::VISITING) {
      Logger<ERROR> << "Named constraints contain a cycle at frame '"
                    << frame_id << "'";
      return Status::CONSTRAINT_CYCLE;
    }
    visit->second = VisitState::VISITING;

    const auto parsed = parsed_layout_.find(frame_id);
    if (parsed == parsed_layout_.end()) {
      return Status::FRAME_NOT_FOUND;
    }

    size_t left  = 0;
    size_t right = 0;
    Status status =
        resolve_axis_bounds(parsed->second, true, screen_width_, left, right);
    if (!is_ok(status)) {
      return status;
    }
    size_t top    = 0;
    size_t bottom = 0;
    status =
        resolve_axis_bounds(parsed->second, false, screen_height_, top, bottom);
    if (!is_ok(status)) {
      return status;
    }

    size_t width = right - left;
    status =
        resolve_measure(parsed->second.get(Layout::ConstraintType::MAX_WIDTH),
                        screen_width_, width, width);
    if (!is_ok(status)) {
      return status;
    }
    width = std::min(width, right - left);

    size_t height = bottom - top;
    status =
        resolve_measure(parsed->second.get(Layout::ConstraintType::MAX_HEIGHT),
                        screen_height_, height, height);
    if (!is_ok(status)) {
      return status;
    }
    height = std::min(height, bottom - top);

    status =
        apply_aspect_ratio(parsed->second, width, height, cell_dimensions_);
    if (!is_ok(status)) {
      return status;
    }

    const Layout::Constraint* horizontal =
        placement_constraint(parsed->second, true);
    const Canvas::Rect* horizontal_reference = nullptr;
    status = resolve_reference(horizontal, horizontal_reference);
    if (!is_ok(status)) {
      return status;
    }

    const Layout::Constraint* vertical =
        placement_constraint(parsed->second, false);
    const Canvas::Rect* vertical_reference = nullptr;
    status = resolve_reference(vertical, vertical_reference);
    if (!is_ok(status)) {
      return status;
    }

    size_t x = 0;
    status   = resolve_axis_origin(horizontal, true, screen_width_, left, right,
                                   width, horizontal_reference, x);
    if (!is_ok(status)) {
      return status;
    }
    size_t y = 0;
    status   = resolve_axis_origin(vertical, false, screen_height_, top, bottom,
                                   height, vertical_reference, y);
    if (!is_ok(status)) {
      return status;
    }

    output_.frame_layouts.insert_or_assign(
        frame_id,
        Canvas::Rect{.x = x, .y = y, .width = width, .height = height});
    visit->second = VisitState::RESOLVED;
    resolution_order_.push_back(frame_id);
    Logger<DEBUG> << "Resolved frame '" << frame_id << "' to " << x << ',' << y
                  << ' ' << width << 'x' << height;
    return Status::OK;
  }

  /**
   * Resolve and return the frame required by a named placement constraint.
   *
   * @param[in] placement Optional placement constraint.
   * @param[out] reference Receives the referenced rectangle, or `nullptr` when
   *             the placement is absent or numeric.
   * @return Status::OK or a dependency resolution error.
   */
  Status resolve_reference(const Layout::Constraint* placement,
                           const Canvas::Rect*& reference) {
    reference = nullptr;
    if (placement == nullptr || placement->unit != Layout::Unit::NAME) {
      return Status::OK;
    }

    const std::string* referenced_name =
        std::get_if<std::string>(&placement->value);
    if (referenced_name == nullptr || referenced_name->empty()) {
      return Status::INVALID_ARGUMENT;
    }

    Status status = resolve(*referenced_name);
    if (!is_ok(status)) {
      return status;
    }
    const auto referenced_rect = output_.frame_layouts.find(*referenced_name);
    if (referenced_rect == output_.frame_layouts.end()) {
      return Status::FRAME_NOT_FOUND;
    }
    reference = &referenced_rect->second;
    return Status::OK;
  }

  /** Layout owning the frames being solved. */
  const Layout::LayoutDescription& description_;

  /** Validated constraints whose pointers refer into `description_`. */
  const ParsedLayout& parsed_layout_;

  /** Screen width in columns. */
  size_t screen_width_;

  /** Screen height in rows. */
  size_t screen_height_;

  /** Relative physical dimensions used by visual aspect ratios. */
  CellDimensions cell_dimensions_;

  /** Destination map receiving solved frame rectangles. */
  Layout::AbsoluteLayout& output_;
  /** Dependency-first order discovered while resolving named anchors. */
  std::vector<std::string>& resolution_order_;

  /** Depth-first traversal state indexed by frame id. */
  std::map<std::string, VisitState> visit_states_;
};

/**
 * Add two cell counts without wrapping `size_t`.
 *
 * @param[in] value Base value.
 * @param[in] addition Value to add.
 * @param[out] result Receives the sum, or zero on overflow.
 * @return Status::OK or Status::DIMENSION_OVERFLOW.
 */
Status checked_add(size_t value, size_t addition, size_t& result) noexcept {
  if (addition > std::numeric_limits<size_t>::max() - value) {
    result = 0;
    return Status::DIMENSION_OVERFLOW;
  }
  result = value + addition;
  return Status::OK;
}

/**
 * Extract an absolute frame minimum from a constraint.
 *
 * Percentage minimums do not establish a fixed terminal requirement and
 * therefore resolve to zero here.
 *
 * @param[in] constraint Optional minimum constraint.
 * @param[out] minimum Receives its fixed character-cell contribution.
 * @return Status::OK or Status::INVALID_CONSTRAINT.
 */
Status character_minimum(const Layout::Constraint* constraint,
                         size_t& minimum) noexcept {
  minimum = 0;
  if (constraint == nullptr || constraint->unit == Layout::Unit::PERCENT) {
    return Status::OK;
  }
  if (constraint->unit != Layout::Unit::CHARACTERS) {
    return Status::INVALID_CONSTRAINT;
  }
  const size_t* value = std::get_if<size_t>(&constraint->value);
  if (value == nullptr) {
    return Status::INVALID_CONSTRAINT;
  }
  minimum = *value;
  return Status::OK;
}

/**
 * Accumulate one margin or placement contribution to an axis minimum.
 *
 * Character values add to `fixed`; percentage values add to `fraction`; named
 * frame anchors add neither because another frame's edge does not impose an
 * absolute screen extent. Center offsets use a multiplier of two because an
 * equal amount of room is needed on both sides to retain centering.
 *
 * @param[in] constraint Optional component constraint.
 * @param[in] multiplier Number of times its contribution is required.
 * @param[in,out] fixed Accumulated fixed cell count.
 * @param[in,out] fraction Accumulated fraction of the screen axis.
 * @return Status::OK or a constraint/overflow error.
 */
Status add_minimum_component(const Layout::Constraint* constraint,
                             size_t multiplier, size_t& fixed,
                             long double& fraction) noexcept {
  if (constraint == nullptr) {
    return Status::OK;
  }

  if (constraint->unit == Layout::Unit::CHARACTERS) {
    const size_t* value = std::get_if<size_t>(&constraint->value);
    if (value == nullptr ||
        (multiplier != 0 &&
         *value > std::numeric_limits<size_t>::max() / multiplier)) {
      return Status::DIMENSION_OVERFLOW;
    }
    return checked_add(fixed, *value * multiplier, fixed);
  }

  if (constraint->unit == Layout::Unit::PERCENT) {
    const float* value = std::get_if<float>(&constraint->value);
    if (value == nullptr) {
      return Status::INVALID_CONSTRAINT;
    }
    fraction +=
        static_cast<long double>(*value) * static_cast<long double>(multiplier);
    return Status::OK;
  }

  if (constraint->unit == Layout::Unit::NAME) {
    return Status::OK;
  }
  return Status::INVALID_CONSTRAINT;
}

/**
 * Find the screen extent needed for a maximum to admit a frame minimum.
 *
 * Percentage conversion rounds down during layout. After computing the
 * mathematical ceiling, this helper verifies the resolved maximum and advances
 * by cells if floating-point truncation still produces too small a value.
 *
 * @param[in] maximum Optional maximum constraint.
 * @param[in] frame_minimum Minimum frame size on the same axis.
 * @param[out] required Receives the required full-screen extent.
 * @return Status::OK or a percentage, constraint, or overflow error.
 */
Status required_for_percentage_maximum(const Layout::Constraint* maximum,
                                       size_t frame_minimum,
                                       size_t& required) noexcept {
  required = 0;
  if (maximum == nullptr) {
    return Status::OK;
  }

  if (maximum->unit == Layout::Unit::CHARACTERS) {
    const size_t* value = std::get_if<size_t>(&maximum->value);
    if (value == nullptr || *value < frame_minimum) {
      return Status::INVALID_CONSTRAINT;
    }
    return Status::OK;
  }

  if (maximum->unit != Layout::Unit::PERCENT) {
    return Status::INVALID_CONSTRAINT;
  }
  const float* percentage = std::get_if<float>(&maximum->value);
  if (percentage == nullptr || *percentage < 0.0F || *percentage > 1.0F) {
    return Status::INVALID_PERCENTAGE;
  }
  if (frame_minimum == 0) {
    return Status::OK;
  }
  if (*percentage == 0.0F) {
    return Status::INVALID_CONSTRAINT;
  }

  const long double estimate =
      std::ceil(static_cast<long double>(frame_minimum) /
                static_cast<long double>(*percentage));
  if (estimate > static_cast<long double>(std::numeric_limits<size_t>::max())) {
    return Status::DIMENSION_OVERFLOW;
  }
  required = static_cast<size_t>(estimate);

  size_t resolved = 0;
  Status status   = resolve_measure(maximum, required, 0, resolved);
  while (is_ok(status) && resolved < frame_minimum) {
    if (required == std::numeric_limits<size_t>::max()) {
      return Status::DIMENSION_OVERFLOW;
    }
    ++required;
    status = resolve_measure(maximum, required, 0, resolved);
  }
  return status;
}

/**
 * Compute the full-screen minimum required on one axis for one frame.
 *
 * The result incorporates the frame minimum, margins, placement offset, and
 * any percentage maximum that must grow enough to contain the frame.
 *
 * @param[in] constraints Parsed constraints for one frame.
 * @param[in] horizontal Select width-related rather than height-related values.
 * @param[in] frame_minimum Frame's fixed minimum on this axis.
 * @param[out] minimum Receives required screen columns or rows.
 * @return Status::OK or a constraint/overflow error.
 */
Status minimum_axis(const ParsedConstraints& constraints, bool horizontal,
                    size_t frame_minimum, size_t& minimum) noexcept {
  const Layout::ConstraintType lower_margin =
      horizontal ? Layout::ConstraintType::LEFT_MARGIN
                 : Layout::ConstraintType::TOP_MARGIN;
  const Layout::ConstraintType upper_margin =
      horizontal ? Layout::ConstraintType::RIGHT_MARGIN
                 : Layout::ConstraintType::BOTTOM_MARGIN;
  const Layout::ConstraintType maximum =
      horizontal ? Layout::ConstraintType::MAX_WIDTH
                 : Layout::ConstraintType::MAX_HEIGHT;

  size_t fixed         = frame_minimum;
  long double fraction = 0.0L;
  Status status =
      add_minimum_component(constraints.get(lower_margin), 1, fixed, fraction);
  if (!is_ok(status)) {
    return status;
  }
  status =
      add_minimum_component(constraints.get(upper_margin), 1, fixed, fraction);
  if (!is_ok(status)) {
    return status;
  }

  const Layout::Constraint* placement =
      placement_constraint(constraints, horizontal);
  const bool centered =
      placement != nullptr &&
      (placement->type == Layout::ConstraintType::HORIZONTAL_CENTER ||
       placement->type == Layout::ConstraintType::VERTICAL_CENTER);
  status =
      add_minimum_component(placement, centered ? 2U : 1U, fixed, fraction);
  if (!is_ok(status)) {
    return status;
  }

  if (fraction >= 1.0L) {
    if (fixed != 0) {
      return Status::INVALID_CONSTRAINT;
    }
    minimum = 0;
  } else {
    const long double required =
        std::ceil(static_cast<long double>(fixed) / (1.0L - fraction));
    if (required >
        static_cast<long double>(std::numeric_limits<size_t>::max())) {
      return Status::DIMENSION_OVERFLOW;
    }
    minimum = static_cast<size_t>(required);
  }

  size_t maximum_requirement = 0;
  status = required_for_percentage_maximum(constraints.get(maximum),
                                           frame_minimum, maximum_requirement);
  if (!is_ok(status)) {
    return status;
  }
  minimum = std::max(minimum, maximum_requirement);
  return Status::OK;
}

/**
 * Compute one frame's fixed minimum width and height.
 *
 * Explicit character minimums seed the result. If an aspect ratio is present,
 * one dimension is rounded upward until both minimums and the ratio can be
 * represented simultaneously.
 *
 * @param[in] constraints Parsed constraints for one frame.
 * @param[in] cell_dimensions Relative physical terminal-cell dimensions.
 * @param[out] width Receives minimum frame columns.
 * @param[out] height Receives minimum frame rows.
 * @return Status::OK or an aspect/constraint/overflow error.
 */
Status frame_minimum_size(const ParsedConstraints& constraints,
                          const CellDimensions& cell_dimensions, size_t& width,
                          size_t& height) noexcept {
  Status status = character_minimum(
      constraints.get(Layout::ConstraintType::MIN_WIDTH), width);
  if (!is_ok(status)) {
    return status;
  }
  status = character_minimum(
      constraints.get(Layout::ConstraintType::MIN_HEIGHT), height);
  if (!is_ok(status)) {
    return status;
  }

  const Layout::Constraint* aspect_ratio =
      constraints.get(Layout::ConstraintType::ASPECT_RATIO);
  if (aspect_ratio == nullptr) {
    return Status::OK;
  }
  const auto* ratio = std::get_if<Layout::AspectRatio>(&aspect_ratio->value);
  if (ratio == nullptr || ratio->width <= 0 || ratio->height <= 0) {
    return Status::INVALID_RATIO;
  }

  size_t height_from_width = 0;
  status                   = height_for_width(width, *ratio, cell_dimensions,
                                              DimensionRounding::UP, height_from_width);
  if (!is_ok(status)) {
    return status;
  }
  if (height >= height_from_width) {
    size_t width_from_height = 0;
    status                   = width_for_height(height, *ratio, cell_dimensions,
                                                DimensionRounding::UP, width_from_height);
    if (!is_ok(status)) {
      return status;
    }
    width = std::max(width, width_from_height);
  } else {
    height = height_from_width;
  }
  return Status::OK;
}

/** Return whether two non-empty half-open rectangles intersect. */
bool rectangles_intersect(const Canvas::Rect& first,
                          const Canvas::Rect& second) noexcept {
  if (first.width == 0U || first.height == 0U || second.width == 0U ||
      second.height == 0U) {
    return false;
  }
  return first.x < second.x + second.width &&
         second.x < first.x + first.width &&
         first.y < second.y + second.height &&
         second.y < first.y + first.height;
}

/**
 * Build the render DAG implied by absolute overlap and Z-buffer ordering.
 *
 * Disjoint frames have no edge and may render concurrently. For every
 * intersecting pair, the backmost index becomes a prerequisite of the
 * frontmost index, preserving deterministic compositing without serializing
 * unrelated regions.
 */
Status build_frame_dependencies(const Layout::LayoutDescription& description,
                                Layout::AbsoluteLayout& absolute_layout) {
  absolute_layout.frame_dependencies.clear();
  const std::vector<ZBuffer::Entry>& frames = description.z_buffer.frames();
  for (size_t back = 0U; back < frames.size(); ++back) {
    const auto back_rect =
        absolute_layout.frame_layouts.find(frames[back].frame_id);
    if (back_rect == absolute_layout.frame_layouts.end()) {
      return Status::FRAME_NOT_FOUND;
    }
    for (size_t front = back + 1U; front < frames.size(); ++front) {
      const auto front_rect =
          absolute_layout.frame_layouts.find(frames[front].frame_id);
      if (front_rect == absolute_layout.frame_layouts.end()) {
        return Status::FRAME_NOT_FOUND;
      }
      if (rectangles_intersect(back_rect->second, front_rect->second)) {
        absolute_layout.frame_dependencies.push_back(
            Layout::FrameDependency{.prerequisite = back, .dependent = front});
      }
    }
  }
  return Status::OK;
}

}  // namespace

Layout::Constraint Layout::make_percentage_constraint(ConstraintType type,
                                                      float percent) {
  return Constraint{.type = type, .unit = Unit::PERCENT, .value = percent};
}

Layout::Constraint Layout::make_character_constraint(ConstraintType type,
                                                     size_t characters) {
  return Constraint{
      .type = type, .unit = Unit::CHARACTERS, .value = characters};
}

Layout::Constraint Layout::make_ratio_constraint(ConstraintType type,
                                                 int32_t width,
                                                 int32_t height) {
  return Constraint{
      .type  = type,
      .unit  = Unit::RATIO,
      .value = AspectRatio{.width = width, .height = height},
  };
}

Layout::Constraint Layout::make_name_constraint(ConstraintType type,
                                                const std::string& name) {
  return Constraint{.type = type, .unit = Unit::NAME, .value = name};
}

std::shared_ptr<Layout::LayoutDescription> Layout::make_layout_description(
    const std::string& layout_name) const {
  Logger<DEBUG> << "Created layout description '" << layout_name << "'";
  auto description         = std::make_shared<LayoutDescription>();
  description->layout_name = layout_name;
  return description;
}

Status Layout::add_frame_to_layout_description(
    const std::shared_ptr<LayoutDescription>& layout_description,
    const std::string& frame_id, std::shared_ptr<Frame> frame) {
  if (!layout_description) {
    Logger<ERROR> << "Cannot add frame '" << frame_id
                  << "' to a null layout description";
    return Status::INVALID_ARGUMENT;
  }

  const Status status =
      layout_description->z_buffer.add(frame_id, std::move(frame));
  if (!is_ok(status)) {
    Logger<ERROR> << "Could not add frame '" << frame_id << "' to layout '"
                  << layout_description->layout_name
                  << "': " << status_message(status);
    return status;
  }

  layout_description->constraints.try_emplace(frame_id);
  ++layout_description->constraint_revision;
  layout_description->cached_resolution_order_valid = false;
  layout_description->cached_absolute_layout.reset();
  Logger<DEBUG> << "Added frame '" << frame_id << "' to layout '"
                << layout_description->layout_name << "'";
  return Status::OK;
}

Status Layout::add_frame(
    const std::shared_ptr<LayoutDescription>& layout_description,
    std::string frame_id, std::shared_ptr<Frame> frame,
    std::initializer_list<Constraint> constraints) {
  if (layout_description == nullptr || frame_id.empty() || frame == nullptr) {
    return Status::INVALID_ARGUMENT;
  }
  for (auto current = constraints.begin(); current != constraints.end();
       ++current) {
    const Status validation = validate_constraint(*current);
    if (!is_ok(validation)) {
      return validation;
    }
    for (auto previous = constraints.begin(); previous != current; ++previous) {
      if (previous->type == current->type ||
          (is_horizontal_placement(previous->type) &&
           is_horizontal_placement(current->type)) ||
          (is_vertical_placement(previous->type) &&
           is_vertical_placement(current->type))) {
        return Status::INVALID_CONSTRAINT;
      }
    }
  }

  Status status = add_frame_to_layout_description(layout_description, frame_id,
                                                  std::move(frame));
  if (!is_ok(status)) {
    return status;
  }
  for (const Constraint& constraint : constraints) {
    status = add_constraint_to_frame(layout_description, frame_id, constraint);
    if (!is_ok(status)) {
      return status;
    }
  }
  return Status::OK;
}

Status Layout::add_constraint_to_frame(
    const std::shared_ptr<LayoutDescription>& layout_description,
    const std::string& frame_id, const Constraint& constraint) {
  if (!layout_description || frame_id.empty()) {
    Logger<ERROR> << "Cannot add layout constraint: "
                  << status_message(Status::INVALID_ARGUMENT);
    return Status::INVALID_ARGUMENT;
  }
  if (!frame_exists(*layout_description, frame_id)) {
    Logger<ERROR> << "Cannot constrain unknown frame '" << frame_id << "'";
    return Status::FRAME_NOT_FOUND;
  }

  const Status validation = validate_constraint(constraint);
  if (!is_ok(validation)) {
    Logger<ERROR> << "Invalid constraint for frame '" << frame_id
                  << "': " << status_message(validation);
    return validation;
  }

  std::vector<Constraint>& constraints =
      layout_description->constraints[frame_id];
  for (const Constraint& existing : constraints) {
    if (existing.type == constraint.type ||
        (is_horizontal_placement(existing.type) &&
         is_horizontal_placement(constraint.type)) ||
        (is_vertical_placement(existing.type) &&
         is_vertical_placement(constraint.type))) {
      Logger<ERROR> << "Conflicting constraint for frame '" << frame_id << "'";
      return Status::INVALID_CONSTRAINT;
    }
  }

  constraints.push_back(constraint);
  ++layout_description->constraint_revision;
  layout_description->cached_resolution_order_valid = false;
  layout_description->cached_absolute_layout.reset();
  Logger<DEBUG> << "Added constraint to frame '" << frame_id << "'";
  return Status::OK;
}

Status Layout::compute_absolute_layout(
    const std::shared_ptr<LayoutDescription>& layout_description,
    size_t screen_width, size_t screen_height,
    const CellDimensions& cell_dimensions,
    AbsoluteLayout& absolute_layout) const {
  absolute_layout.frame_layouts.clear();
  absolute_layout.frame_dependencies.clear();
  if (!layout_description) {
    Logger<ERROR> << "Cannot compute a null layout description";
    return Status::INVALID_ARGUMENT;
  }
  if (cell_dimensions.width == 0 || cell_dimensions.height == 0) {
    Logger<ERROR> << "Cannot compute a layout with zero-sized cells";
    return Status::INVALID_DIMENSIONS;
  }

  const size_t z_buffer_revision = layout_description->z_buffer.revision();
  const std::optional<CachedAbsoluteLayout>& cached =
      layout_description->cached_absolute_layout;
  if (cached.has_value() && cached->screen_width == screen_width &&
      cached->screen_height == screen_height &&
      cached->cell_dimensions == cell_dimensions &&
      cached->z_buffer_revision == z_buffer_revision &&
      cached->constraint_revision == layout_description->constraint_revision) {
    absolute_layout = cached->layout;
    Logger<DEBUG> << "Reused cached absolute layout '"
                  << layout_description->layout_name << "'";
    return Status::OK;
  }

  ParsedLayout parsed_layout;
  Status status = parse_layout(*layout_description, parsed_layout);
  if (!is_ok(status)) {
    return status;
  }

  std::vector<std::string> resolution_order;
  Resolver resolver(*layout_description, parsed_layout, screen_width,
                    screen_height, cell_dimensions, absolute_layout,
                    resolution_order);
  const bool cached_order_valid =
      layout_description->cached_resolution_order_valid &&
      layout_description->cached_order_z_buffer_revision == z_buffer_revision &&
      layout_description->cached_order_constraint_revision ==
          layout_description->constraint_revision;
  status = resolver.resolve_all(
      cached_order_valid ? &layout_description->cached_resolution_order
                         : nullptr);
  if (!is_ok(status)) {
    absolute_layout.frame_layouts.clear();
    absolute_layout.frame_dependencies.clear();
    Logger<ERROR> << "Could not compute layout '"
                  << layout_description->layout_name
                  << "': " << status_message(status);
    return status;
  }

  status = build_frame_dependencies(*layout_description, absolute_layout);
  if (!is_ok(status)) {
    absolute_layout.frame_layouts.clear();
    absolute_layout.frame_dependencies.clear();
    return status;
  }

  if (!cached_order_valid) {
    layout_description->cached_resolution_order = std::move(resolution_order);
    layout_description->cached_order_z_buffer_revision = z_buffer_revision;
    layout_description->cached_order_constraint_revision =
        layout_description->constraint_revision;
    layout_description->cached_resolution_order_valid = true;
  }
  layout_description->cached_absolute_layout = CachedAbsoluteLayout{
      .screen_width        = screen_width,
      .screen_height       = screen_height,
      .cell_dimensions     = cell_dimensions,
      .z_buffer_revision   = z_buffer_revision,
      .constraint_revision = layout_description->constraint_revision,
      .layout              = absolute_layout,
  };

  Logger<DEBUG> << "Computed layout '" << layout_description->layout_name
                << "' for " << screen_width << 'x' << screen_height
                << " screen with " << cell_dimensions.width << ':'
                << cell_dimensions.height << " cell dimensions";
  return Status::OK;
}

Status Layout::compute_minimum_dimensions(
    const std::shared_ptr<LayoutDescription>& layout_description,
    const CellDimensions& cell_dimensions, size_t& minimum_width,
    size_t& minimum_height) const {
  minimum_width  = 0;
  minimum_height = 0;
  if (!layout_description) {
    Logger<ERROR> << "Cannot inspect a null layout description";
    return Status::INVALID_ARGUMENT;
  }
  if (cell_dimensions.width == 0 || cell_dimensions.height == 0) {
    Logger<ERROR> << "Cannot inspect a layout with zero-sized cells";
    return Status::INVALID_DIMENSIONS;
  }

  ParsedLayout parsed_layout;
  Status status = parse_layout(*layout_description, parsed_layout);
  if (!is_ok(status)) {
    return status;
  }

  for (const auto& [frame_id, constraints] : parsed_layout) {
    size_t frame_width  = 0;
    size_t frame_height = 0;
    status = frame_minimum_size(constraints, cell_dimensions, frame_width,
                                frame_height);
    if (!is_ok(status)) {
      Logger<ERROR> << "Could not compute minimum size for frame '" << frame_id
                    << "': " << status_message(status);
      return status;
    }

    size_t required_width = 0;
    status = minimum_axis(constraints, true, frame_width, required_width);
    if (!is_ok(status)) {
      return status;
    }
    size_t required_height = 0;
    status = minimum_axis(constraints, false, frame_height, required_height);
    if (!is_ok(status)) {
      return status;
    }

    minimum_width  = std::max(minimum_width, required_width);
    minimum_height = std::max(minimum_height, required_height);
  }

  Logger<DEBUG> << "Minimum size for layout '"
                << layout_description->layout_name << "' is " << minimum_width
                << 'x' << minimum_height;
  return Status::OK;
}

Status Layout::draw(
    const std::shared_ptr<LayoutDescription>& layout_description,
    const AbsoluteLayout& absolute_layout, const Theme& theme,
    Canvas& canvas) const {
  if (!layout_description) {
    Logger<ERROR> << "Cannot draw a null layout description";
    return Status::INVALID_ARGUMENT;
  }

  const std::vector<ZBuffer::Entry>& frames =
      layout_description->z_buffer.frames();
  for (size_t frame_index = 0U; frame_index < frames.size(); ++frame_index) {
    const ZBuffer::Entry& frame = frames[frame_index];
    const auto rect = absolute_layout.frame_layouts.find(frame.frame_id);
    if (rect == absolute_layout.frame_layouts.end()) {
      Logger<ERROR> << "No absolute rectangle for frame '" << frame.frame_id
                    << "'";
      return Status::FRAME_NOT_FOUND;
    }

    const Status status = frame.frame->draw(theme, canvas, rect->second);
    if (!is_ok(status)) {
      Logger<ERROR> << "Frame '" << frame.frame_id
                    << "' failed to draw: " << status_message(status);
      return status;
    }
    Logger<DEBUG> << "Drew frame '" << frame.frame_id << "'";
  }

  return Status::OK;
}

}  // namespace tui
}  // namespace puc
