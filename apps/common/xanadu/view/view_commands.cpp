#include "common/xanadu/view/view_commands.hpp"

#include <algorithm>
#include <cctype>

#include "common/xanadu/system_docs.hpp"

namespace xanadu::view {

namespace {

[[nodiscard]] bool blank(const char c) noexcept {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
  while (!text.empty() && blank(text.front())) {
    text.remove_prefix(1);
  }
  while (!text.empty() && blank(text.back())) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] bool sameLetters(const std::string_view a,
                               const std::string_view b) noexcept {
  return std::ranges::equal(a, b, [](const char x, const char y) {
    return std::tolower(static_cast<unsigned char>(x)) ==
           std::tolower(static_cast<unsigned char>(y));
  });
}

/// The point a reader means by @p name: the one spelt exactly so, or else the
/// one that differs only in case ('X' for x), when that is only one.
[[nodiscard]] std::optional<ViewAxisId>
pointNamed(const ViewAxisSet &axes, const std::string_view name) {
  if (const auto exact = axes.axisNamed(name)) {
    return exact;
  }
  std::optional<ViewAxisId> found;
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    if (const auto point = axes.pointName(axis);
        point && sameLetters(*point, name)) {
      if (found) {
        return std::nullopt;
      }
      found = axis;
    }
  }
  return found;
}

} // namespace

std::optional<ViewAxisId> bindingPointNamed(const ViewAxisSet &axes,
                                            const std::string_view name) {
  return pointNamed(axes, name);
}

std::optional<BindCommand> parseBindCommand(const std::string_view text) {
  const auto line = trimmed(text);
  const auto cut  = std::ranges::find_if(line, blank);
  if (cut == line.end()) {
    return std::nullopt;
  }
  const auto point =
      line.substr(0, static_cast<std::size_t>(cut - line.begin()));
  const auto target = trimmed(line.substr(point.size()));
  if (point.empty() || target.empty()) {
    return std::nullopt;
  }
  return BindCommand{.point  = std::string(point),
                     .target = std::string(target)};
}

BindingChoices bindingChoicesOf(const ViewManifold &space,
                                const SpanReader &reader) {
  const auto &axes = space.axes();
  BindingChoices choices;
  for (ViewAxisId axis = 0; axis < axes.axisCount(); ++axis) {
    if (auto point = axes.pointName(axis)) {
      choices.points.push_back(std::move(*point));
    }
  }
  for (const auto dim : space.base().dimensions()) {
    if (auto name = space.base().textOf(dim, reader);
        !name.empty() && name != kDimDims) {
      choices.dimensions.push_back(std::move(name));
    }
  }
  axes.forEachGroup([&](const BindTarget group) {
    if (auto name = axes.groupName(group)) {
      choices.groups.push_back(std::move(*name));
    }
  });
  return choices;
}

std::expected<Bound, ViewError> bindByName(ViewManifold &space,
                                           const SpanReader &reader,
                                           const BindCommand &command) {
  auto &axes      = space.axes();
  const auto axis = pointNamed(axes, command.point);
  if (!axis) {
    return std::unexpected{ViewError::UnknownAxis};
  }
  std::optional<BindTarget> target;
  bool group = false;
  if (command.target != kDimDims) {
    target = space.base().dimensionNamed(command.target, reader);
  }
  if (!target) {
    axes.forEachGroup([&](const BindTarget candidate) {
      if (!target && axes.groupName(candidate) == command.target) {
        target = candidate;
        group  = true;
      }
    });
  }
  if (!target) {
    return std::unexpected{ViewError::UnknownName};
  }
  if (const auto bound = axes.bind(*axis, *target); !bound) {
    return std::unexpected{bound.error()};
  }
  return Bound{.axis  = *axis,
               .point = axes.pointName(*axis).value_or(command.point),
               .shows = command.target,
               .group = group};
}

std::optional<std::uint32_t> steppedSubview(const std::size_t count,
                                            const std::uint32_t current,
                                            const int by) noexcept {
  if (0 == count) {
    return std::nullopt;
  }
  const auto size = static_cast<long long>(count);
  const auto next = (static_cast<long long>(current) + by) % size;
  return static_cast<std::uint32_t>(next < 0 ? next + size : next);
}

std::optional<std::uint32_t> subviewIndexOf(const ViewDescriptor &descriptor,
                                            const std::string_view id) {
  const auto found =
      std::ranges::find(descriptor.subviews, id, &SubviewSpec::id);
  if (id.empty() || found == descriptor.subviews.end()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(found - descriptor.subviews.begin());
}

} // namespace xanadu::view
