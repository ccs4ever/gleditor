#include "common/xanadu/vagent/runtime/epistemic_preflight.hpp"

#include <array>
#include <limits>
#include <unordered_set>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_limits.hpp"
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

std::optional<std::string> savedHash(const zigzag::ArenaManifold &arena,
                                     const ValueDims &vd, CellRef predicate,
                                     const std::optional<DimRef> &epistemic) {
  if (!epistemic) return std::nullopt;
  const auto master = arena.linked(predicate, *epistemic);
  if (!master) return std::nullopt;
  const auto hash = fieldValue(arena, vd, *master, "hash");
  return hash ? std::optional{arena.textOf(*hash)} : std::nullopt;
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
checkResumePolicy(const zigzag::ArenaManifold &arena, CellRef root,
                  const DimTable &table, const UpgradeSchema &into) {
  const auto dims = OwnedGraphDims::byName(table);
  if (!dims.vars || !dims.values || !dims.epistemic) return std::nullopt;
  const ValueDims vd{.vars   = *dims.vars,
                     .values = *dims.values,
                     .grab   = dims.grab.value_or(*dims.vars),
                     .step   = dims.step.value_or(*dims.vars)};
  for (const auto predicate : ranked(arena, root, dims.belief)) {
    const auto name = arena.textOf(predicate);
    const auto was  = savedHash(arena, vd, predicate, dims.epistemic);
    const auto next = into.predicates.find(name);
    if (next == into.predicates.end()) {
      // A plain predicate is dropped as it always was; one with a policy
      // master holds sources' histories scored under it.
      if (was)
        return TransferRefusal::make(
            TransferCode::PredicateRemoved, kTransferRoute,
            zigzag::vortex::mesh::ActorError::ExecutionFailed,
            {.subject = name, .saved = was});
      continue;
    }
    const auto now = next->second.epistemicPolicy
                         ? std::optional{next->second.epistemicPolicy->hash}
                         : std::nullopt;
    // A source's history was judged under its policy; a resume under another
    // would score it by rules it was never checked against.
    if (was != now)
      return TransferRefusal::make(
          TransferCode::PolicyChanged, kTransferRoute,
          zigzag::vortex::mesh::ActorError::ExecutionFailed,
          {.subject = name, .saved = was, .current = now});
  }
  return std::nullopt;
}

std::optional<TransferRefusal>
checkResumeBudget(const zigzag::ArenaManifold &arena, CellRef root,
                  const DimTable &table, const ResumeAllowance &allowance) {
  const auto dims  = OwnedGraphDims::byName(table);
  const auto cells = countOwnedCells(
      arena, root, dims, std::numeric_limits<std::size_t>::max() - 1);
  const auto ceiling = allowance.epistemicCells;
  // The same sixteenth of headroom that lets an isolate land on a full arena.
  const auto ceilingWithHeadroom = ceiling + ceiling / kHeadroomDivisor;
  const auto peak                = [&](std::uint64_t allowed) {
    return TransferRefusal::make(
        TransferCode::PeakCopyBudget, kTransferRoute,
        zigzag::vortex::mesh::ActorError::ResourceLimit,
        {.savedCount = cells, .limit = allowed});
  };
  if (cells > ceilingWithHeadroom) return peak(ceilingWithHeadroom);
  if (allowance.grant) {
    if (cells > allowance.grant->maxCells)
      return peak(allowance.grant->maxCells);
    const std::uint64_t work = cells * kTransferWorkPerCell;
    if (work > allowance.grant->instructions)
      return TransferRefusal::make(
          TransferCode::WorkBudget, kTransferRoute,
          zigzag::vortex::mesh::ActorError::ResourceLimit,
          {.savedCount = work, .limit = allowance.grant->instructions});
  }
  return std::nullopt;
}

} // namespace xanadu::vagent::runtime
