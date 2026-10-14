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

} // namespace

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

std::expected<Bound, ViewError> bindByName(ViewManifold &space,
                                           const SpanReader &reader,
                                           const BindCommand &command) {
  auto &axes      = space.axes();
  const auto axis = axes.axisNamed(command.point);
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
