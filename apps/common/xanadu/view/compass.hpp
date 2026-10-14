/**
 * @file compass.hpp
 * @brief What the compass shows: every configured binding point and what is
 *        bound to it (design/view-system.md §7.9; plan §4.6 U5a).
 *
 * The model is device-free so the engine tests read it and any front end draws
 * it: one arm per point of the placement's axes, in order, whether the point
 * is bound or not, each with the cue of the dimension it shows. A group shows
 * its name and how many members it has, and takes the cue of the dimension it
 * resolves to, as a pack does (§7.2), so what it looks like on the compass is
 * what its lane looks like.
 */
#ifndef COMMON_XANADU_VIEW_COMPASS_HPP
#define COMMON_XANADU_VIEW_COMPASS_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/cpp26.hpp>

#include "common/xanadu/view/dimension_colour.hpp"
#include "common/xanadu/view/view_manifold.hpp"

namespace xanadu {
class SpanReader;
} // namespace xanadu

namespace xanadu::view {

/// One binding point on the compass.
struct CompassArm {
  /// The point's name: its label here and its key in the selector.
  std::string point;
  /// The word its role is configured by.
  std::string role;
  /// Whether a view gives the point a direction of its space; a point that is
  /// not is drawn as a petal rather than an arm.
  bool spatial{};
  /// The dimension's name or the group's; empty for an unbound point.
  std::string shows;
  bool group{};
  /// A group's members, so the compass can say how many.
  std::size_t members{};
  /// What the shown dimension is drawn with everywhere else; nothing for an
  /// unbound point, or for a group that resolves to no dimension.
  std::optional<DimensionCue> cue;

  [[nodiscard]] bool bound() const noexcept { return !shows.empty(); }
  /// The words an arm carries, for the screen and for an announcement alike:
  /// "x  d.across", "y  Contact (3)", "z  nothing".
  [[nodiscard]] std::string words() const;
  bool operator==(const CompassArm &) const = default;
};

/// The cue of a dimension by name; the compass asks for every dimension it
/// shows, whether or not one has been minted yet.
using CueOf = gleditor::cpp26::function_ref<DimensionCue(std::string_view)>;

/// One arm for each point of @p space's axes, in order. Dimension names are
/// read from the base through @p reader.
[[nodiscard]] std::vector<CompassArm>
compassOf(const ViewManifold &space, const SpanReader &reader, CueOf cueOf);

} // namespace xanadu::view

#endif // COMMON_XANADU_VIEW_COMPASS_HPP
