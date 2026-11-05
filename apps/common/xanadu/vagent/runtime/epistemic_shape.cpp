#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"

#include <algorithm>
#include <ranges>

#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

EpistemicStateProbe probeEpistemicState(const zigzag::ArenaManifold &arena,
                                        const zigzag::CellRef agent,
                                        const OwnedGraphDims &dims) {
  EpistemicStateProbe probe;
  if (dims.trust && arena.linked(agent, *dims.trust)) probe.populated = true;
  if (!dims.epistemic) return probe;
  for (const auto cell : zigzag::rankAfter(arena, agent, *dims.epistemic)) {
    if (arena.textOf(cell) != kShapeHeaderName) {
      probe.populated = true;
      continue;
    }
    if (probe.shape || !dims.vars || !dims.values) continue;
    const ValueDims vd{.vars   = *dims.vars,
                       .values = *dims.values,
                       .grab   = dims.grab.value_or(*dims.vars),
                       .step   = dims.step.value_or(*dims.vars)};
    if (const auto version = fieldValue(arena, vd, cell, kShapeVersionField))
      probe.shape = arena.asInt64(*version);
  }
  return probe;
}

bool hasTranslatableEpistemicState(
    const EpistemicStateProbe &probe,
    const TranslatorRegistry &registry) noexcept {
  return !probe.populated || (probe.shape && registry.translates(*probe.shape));
}

std::optional<std::int64_t> programShape(const UpgradeSchema &schema) {
  const bool arbitrated = std::ranges::any_of(
      schema.predicates,
      [](const auto &p) { return p.second.epistemicPolicy.has_value(); });
  return arbitrated ? std::optional{kEpistemicShapeVersion} : std::nullopt;
}

std::optional<TransferRefusal> checkShape(const EpistemicStateProbe &saved,
                                          const std::optional<std::int64_t> current,
                                          const TransferRoute route) {
  if (!saved.populated) return std::nullopt;
  if (saved.shape && (!current || *saved.shape == *current))
    return std::nullopt;
  const auto count = [](const std::optional<std::int64_t> &shape)
      -> std::optional<std::uint64_t> {
    return shape ? std::optional{static_cast<std::uint64_t>(*shape)}
                 : std::nullopt;
  };
  return TransferRefusal::make(
      TransferCode::UnknownShapeVersion, route,
      zigzag::vortex::mesh::ActorError::ExecutionFailed,
      {.subject      = "d.epistemic",
       .savedCount   = count(saved.shape),
       .currentCount = count(current)});
}

} // namespace xanadu::vagent::runtime
