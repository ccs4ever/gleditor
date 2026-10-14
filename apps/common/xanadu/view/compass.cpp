#include "common/xanadu/view/compass.hpp"

#include <format>

#include "common/xanadu/view/view_binding.hpp"

namespace xanadu::view {

std::string CompassArm::words() const {
  if (!bound()) {
    return std::format("{}  nothing", point);
  }
  if (group) {
    return std::format("{}  {} ({})", point, shows, members);
  }
  return std::format("{}  {}", point, shows);
}

std::vector<CompassArm> compassOf(const ViewManifold &space,
                                  const SpanReader &reader, const CueOf cueOf) {
  const auto &axes = space.axes();
  std::vector<CompassArm> arms;
  arms.reserve(axes.axisCount());
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    CompassArm arm;
    arm.point = axes.pointName(axis).value_or(std::string{});
    if (const auto role = axes.role(axis)) {
      arm.role    = std::string(role->id());
      arm.spatial = role->spatial();
    }
    if (const auto shown = axes.shown(axis)) {
      arm.group = axes.isGroup(*shown);
      if (arm.group) {
        arm.shows   = axes.groupName(*shown).value_or(std::string{});
        arm.members = axes.memberCount(*shown);
      } else {
        arm.shows = space.base().textOf(*shown, reader);
      }
      // A group resolves to its first member that resolves, depth first.
      std::string cued;
      axes.forEachLeaf(*shown, [&](const zigzag::DimRef dim) {
        if (cued.empty()) {
          cued = space.base().textOf(dim, reader);
        }
      });
      if (!cued.empty()) {
        arm.cue = cueOf(cued);
      }
    }
    arms.push_back(std::move(arm));
  }
  return arms;
}

} // namespace xanadu::view
