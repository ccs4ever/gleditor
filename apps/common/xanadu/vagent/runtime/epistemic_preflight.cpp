#include "common/xanadu/vagent/runtime/epistemic_preflight.hpp"

#include <array>
#include <limits>
#include <ranges>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"
#include "common/xanadu/vagent/runtime/wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vagent::runtime {

namespace {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::DimVector;

constexpr auto kTransferRoute = TransferRoute::Resume;

std::vector<CellRef> ranked(const zigzag::ArenaManifold &arena, CellRef owner,
                            const std::optional<DimRef> &dim) {
  return dim ? zigzag::rankAfter(arena, owner, *dim) |
                   std::ranges::to<std::vector<CellRef>>()
             : std::vector<CellRef>{};
}

} // namespace

DimTable discoverDimensions(const zigzag::ArenaManifold &arena, CellRef root) {
  const auto inBase = [](CellRef ref) {
    return (ref & zigzag::ephemeralBit) == 0;
  };
  DimTable found;
  std::unordered_set<CellRef> seen{root};
  std::vector<CellRef> frontier{root};
  for (std::size_t i = 0; i < frontier.size(); ++i) {
    for (const auto &link : arena.dimensionsOf(frontier[i])) {
      if (!inBase(link.dim)) continue;
      found.try_emplace(arena.textOf(link.dim), link.dim);
      for (const auto dir : {DimVector::POS, DimVector::NEG}) {
        const auto next = link.neighbor(dir);
        if (next && inBase(*next) && seen.insert(*next).second)
          frontier.push_back(*next);
      }
    }
  }
  return found;
}

OwnedGraphDims OwnedGraphDims::byName(const DimTable &table) {
  const auto find = [&](const char *name) -> std::optional<DimRef> {
    const auto it = table.find(name);
    return it == table.end() ? std::nullopt : std::optional{it->second};
  };
  return {.vars      = find("d.vars"),
          .values    = find("d.values"),
          .grab      = find("d.grab"),
          .step      = find("d.step"),
          .clone     = find("d.clone"),
          .clause    = find("d.clause"),
          .source    = find("d.source"),
          .belief    = find("d.belief"),
          .trust     = find("d.trust"),
          .epistemic = find("d.epistemic")};
}

std::size_t countOwnedCells(const zigzag::ArenaManifold &arena, CellRef agent,
                            const OwnedGraphDims &dims, std::size_t ceiling) {
  std::unordered_set<CellRef> seen;
  std::vector<CellRef> stack;
  const auto admit = [&](CellRef cell) {
    if (seen.size() <= ceiling && seen.insert(cell).second)
      stack.push_back(cell);
  };
  // Host admission against a host setting, so C++: the walk is bounded by the
  // ceiling and runs before any actor exists.
  for (const auto profile : ranked(arena, agent, dims.trust)) {
    admit(profile);
    for (const auto record : ranked(arena, profile, dims.epistemic))
      admit(record);
  }
  // The agent's own records (the audience); the header is the program's.
  for (const auto record : ranked(arena, agent, dims.epistemic))
    if (arena.textOf(record) != kShapeHeaderName) admit(record);
  // Every support's history on an arbitrated predicate: the predicate heads
  // its policy master on `d.epistemic`, which only arbitration mints.
  for (const auto predicate : ranked(arena, agent, dims.belief)) {
    if (!dims.epistemic || !arena.linked(predicate, *dims.epistemic)) continue;
    for (const auto clause : ranked(arena, predicate, dims.clause))
      for (const auto support : ranked(arena, clause, dims.source))
        for (const auto record : ranked(arena, support, dims.epistemic))
          admit(record);
  }
  const std::array data{dims.vars, dims.values, dims.grab, dims.step,
                        dims.clone};
  while (!stack.empty() && seen.size() <= ceiling) {
    const auto cell = stack.back();
    stack.pop_back();
    for (const auto &dim : data)
      if (dim)
        if (const auto next = arena.linked(cell, *dim)) admit(*next);
  }
  return seen.size();
}

std::optional<TransferRefusal>
checkResumeState(const zigzag::ArenaManifold &arena, CellRef root,
                 const DimTable &table, const UpgradeSchema &into) {
  const auto dims    = OwnedGraphDims::byName(table);
  const auto probe   = probeEpistemicState(arena, root, dims);
  const auto current = programShape(into);
  if (probe.shape || current) {
    const auto intention  = table.find("d.intention");
    const auto intentions = intention == table.end()
                                ? std::size_t{0}
                                : std::ranges::distance(zigzag::rankAfter(
                                      arena, root, intention->second));
    if (intentions != 0)
      return TransferRefusal::make(
          TransferCode::ActiveIntention, kTransferRoute,
          zigzag::vortex::mesh::ActorError::ExecutionFailed,
          {.savedCount = static_cast<std::uint64_t>(intentions)});
  }
  return checkShape(probe, current, kTransferRoute);
}

} // namespace xanadu::vagent::runtime
