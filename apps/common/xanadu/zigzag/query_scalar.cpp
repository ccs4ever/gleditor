#include "common/xanadu/zigzag/query_scalar.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/query_clone_master.hpp"

namespace zigzag {
namespace {

constexpr std::string_view closureDescriptorTag = "VORTEX_CLOSURE_DESCRIPTOR/1";
constexpr std::string_view closureValueTag      = "VORTEX_CLOSURE_VALUE/1";

struct RuntimeObjectSource {
  const Manifold *manifold{};
  const xanadu::SpanReader *reader{};
  CellRef root;
  CellRef arenaRoot;
  DimRef claimDimension;
};

enum class TextReadResult { Read, LimitExceeded, Unreadable };

bool spendInspection(const ArenaManifold &arena, std::size_t &remaining,
                     const std::size_t amount = 1) {
  if (amount > remaining) return false;
  remaining -= amount;
  try {
    for (std::size_t i = 0; i < amount; ++i) arena.consumeWork();
  } catch (...) {
    return false;
  }
  return true;
}

TextReadResult readBoundedText(const ArenaManifold &arena,
                               const RuntimeObjectSource &source,
                               const CellRef cell, std::size_t &remaining,
                               std::string &text) {
  const auto spans = source.manifold != nullptr
                         ? source.manifold->contentOf(cell)
                         : arena.contentOf(cell);
  if (spans.size() > remaining) return TextReadResult::LimitExceeded;
  std::size_t bytes = 0;
  for (const auto &span : spans) {
    if (!spendInspection(arena, remaining))
      return TextReadResult::LimitExceeded;
    if (span.length > remaining ||
        span.length > std::numeric_limits<std::size_t>::max() - bytes)
      return TextReadResult::LimitExceeded;
    bytes += static_cast<std::size_t>(span.length);
  }
  if (bytes > remaining) return TextReadResult::LimitExceeded;
  if (!spendInspection(arena, remaining, bytes))
    return TextReadResult::LimitExceeded;
  try {
    if (source.manifold != nullptr) {
      if (source.reader != nullptr) {
        text = source.manifold->textOf(cell, *source.reader);
      } else if (cell == source.root) {
        text = arena.textOf(source.arenaRoot);
      } else {
        return TextReadResult::Unreadable;
      }
    } else {
      text = arena.textOf(cell);
    }
  } catch (...) {
    return TextReadResult::Unreadable;
  }
  if (text.size() != bytes) return TextReadResult::Unreadable;
  return TextReadResult::Read;
}

} // namespace

RuntimeObjectClaimResult
classifyRuntimeObjectRoot(const ArenaManifold &arena, const CellRef root,
                          const DimRef claimDimension,
                          const RuntimeObjectReadLimits limits) {
  if (!arena.contains(root) || claimDimension == noCell)
    return RuntimeObjectClaimResult::Unreadable;

  std::size_t workRemaining = limits.maxMappingWork;
  const auto chargeWork     = [&](const std::size_t amount) {
    try {
      for (std::size_t i = 0; i < amount; ++i) arena.consumeWork();
    } catch (...) {
      return false;
    }
    return true;
  };
  RuntimeObjectSource source{
      .root = root, .arenaRoot = root, .claimDimension = claimDimension};
  if (const auto foreign = arena.foreignOf(root)) {
    const auto space = arena.spaceAt(foreign->space);
    if (!space || !space->manifold ||
        !space->manifold->contains(foreign->index))
      return RuntimeObjectClaimResult::Unreadable;
    source.manifold = space->manifold;
    source.reader   = space->reader != nullptr
                          ? space->reader
                          : (space->store != nullptr ? space->store
                                                     : space->manifold->store());
    source.root     = foreign->index;
  }

  const auto rootKind = source.manifold != nullptr
                            ? source.manifold->valueKindOf(source.root)
                            : arena.valueKindOf(source.root);
  if (rootKind != xanadu::ValueKind::None)
    return RuntimeObjectClaimResult::NoClaim;

  std::string rootText;
  const auto rootTextResult =
      readBoundedText(arena, source, source.root, workRemaining, rootText);
  if (rootTextResult == TextReadResult::LimitExceeded)
    return RuntimeObjectClaimResult::ProbeLimitExceeded;
  if (rootTextResult == TextReadResult::Unreadable)
    return RuntimeObjectClaimResult::Unreadable;
  if (rootText != runtimeObjectProbeText)
    return RuntimeObjectClaimResult::NoClaim;

  if (const auto foreign = arena.foreignOf(root)) {
    auto mappingWork  = workRemaining;
    const auto mapped = arena.checkedDimensionMapping(
        foreign->space, claimDimension, mappingWork, chargeWork);
    if (!mapped) return RuntimeObjectClaimResult::ProbeLimitExceeded;
    workRemaining = mappingWork;
    if (*mapped) {
      source.claimDimension = **mapped;
    } else {
      if (source.reader == nullptr) return RuntimeObjectClaimResult::Unreadable;
      auto lookupLimits = arena.dimensionLookupLimits();
      lookupLimits.work = std::min(lookupLimits.work, workRemaining);
      DimensionLookupBudget budget{lookupLimits};
      const auto named = source.manifold->dimensionNamedBounded(
          runtimeObjectClaimDimensionName, *source.reader, budget, chargeWork);
      if (!named) return RuntimeObjectClaimResult::ProbeLimitExceeded;
      if (!*named) return RuntimeObjectClaimResult::NoClaim;
      source.claimDimension = **named;
      workRemaining         = budget.workRemaining;
    }
  }

  const auto relations = source.manifold != nullptr
                             ? source.manifold->dimensionsOf(source.root)
                             : arena.dimensionsOf(source.root);
  if (relations.size() > limits.maxRootRelations)
    return RuntimeObjectClaimResult::ProbeLimitExceeded;

  std::optional<CellRef> candidate;
  for (const auto &relation : relations) {
    if (!spendInspection(arena, workRemaining))
      return RuntimeObjectClaimResult::ProbeLimitExceeded;
    if (relation.dim != source.claimDimension) continue;
    if (candidate) return RuntimeObjectClaimResult::Unreadable;
    const auto positive = relation.neighbor(DimVector::POS);
    if (!positive) return RuntimeObjectClaimResult::NoClaim;
    candidate = *positive;
  }
  if (!candidate) return RuntimeObjectClaimResult::NoClaim;

  const bool candidateExists = source.manifold != nullptr
                                   ? source.manifold->contains(*candidate)
                                   : arena.contains(*candidate);
  if (!candidateExists) return RuntimeObjectClaimResult::Unreadable;
  const auto candidateRelations =
      source.manifold != nullptr ? source.manifold->dimensionsOf(*candidate)
                                 : arena.dimensionsOf(*candidate);
  if (candidateRelations.size() > limits.maxRootRelations)
    return RuntimeObjectClaimResult::ProbeLimitExceeded;
  bool sawClaimDimension = false;
  bool hasBacklink       = false;
  for (const auto &relation : candidateRelations) {
    if (!spendInspection(arena, workRemaining))
      return RuntimeObjectClaimResult::ProbeLimitExceeded;
    if (relation.dim != source.claimDimension) continue;
    if (sawClaimDimension) return RuntimeObjectClaimResult::Unreadable;
    sawClaimDimension = true;
    hasBacklink       = relation.neighbor(DimVector::NEG) == source.root;
  }
  if (!sawClaimDimension || !hasBacklink)
    return RuntimeObjectClaimResult::Unreadable;

  const auto targetKind = source.manifold != nullptr
                              ? source.manifold->valueKindOf(*candidate)
                              : arena.valueKindOf(*candidate);
  if (targetKind != xanadu::ValueKind::None)
    return RuntimeObjectClaimResult::Unreadable;
  std::string targetText;
  const auto targetTextResult =
      readBoundedText(arena, source, *candidate, workRemaining, targetText);
  if (targetTextResult == TextReadResult::LimitExceeded)
    return RuntimeObjectClaimResult::ProbeLimitExceeded;
  if (targetTextResult == TextReadResult::Unreadable)
    return RuntimeObjectClaimResult::Unreadable;
  if (targetText == closureDescriptorTag || targetText == closureValueTag ||
      targetText == vortexBindingsDescriptorTag)
    return RuntimeObjectClaimResult::Claimed;
  return RuntimeObjectClaimResult::Unreadable;
}

std::expected<QueryScalar, QueryScalarReadError>
readQueryScalar(ArenaManifold &arena, const CellRef cell,
                const DimRef cloneDimension,
                const DimRef runtimeObjectClaimDimension,
                const RuntimeObjectReadLimits limits) {
  if (!arena.contains(cell))
    return std::unexpected(QueryScalarReadError::Missing);

  const CellRef master =
      queryCloneMaster(arena, cell, cloneDimension).value_or(cell);
  if (!arena.contains(master))
    return std::unexpected(QueryScalarReadError::Missing);

  auto kind = arena.valueKindOf(master);
  if (const auto foreign = arena.foreignOf(master)) {
    const auto space = arena.spaceAt(foreign->space);
    if (!space || !space->manifold ||
        !space->manifold->contains(foreign->index)) {
      return std::unexpected(QueryScalarReadError::InvalidForeign);
    }
    kind = space->manifold->valueKindOf(foreign->index);
  }

  if (kind == xanadu::ValueKind::OpHandle ||
      kind == xanadu::ValueKind::ExternRef ||
      kind == xanadu::ValueKind::ForeignOpHandle) {
    return std::unexpected(QueryScalarReadError::UnsupportedHandle);
  }
  if (kind == xanadu::ValueKind::Int64) {
    if (const auto value = arena.asInt64(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }
  if (kind == xanadu::ValueKind::Double) {
    if (const auto value = arena.asDouble(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }
  if (kind == xanadu::ValueKind::Bool) {
    if (const auto value = arena.asBool(master)) return QueryScalar{*value};
    return std::unexpected(QueryScalarReadError::Unreadable);
  }

  if (kind != xanadu::ValueKind::None && kind != xanadu::ValueKind::Timestamp)
    return std::unexpected(QueryScalarReadError::Unreadable);

  const auto spans = arena.contentOf(master);
  for (const auto &span : spans) {
    for (std::uint64_t byte = 0; byte < span.length; ++byte)
      arena.consumeWork();
  }
  auto text = arena.textOf(master);
  if (text == runtimeObjectProbeText) {
    switch (classifyRuntimeObjectRoot(arena, master,
                                      runtimeObjectClaimDimension, limits)) {
    case RuntimeObjectClaimResult::NoClaim:
      break;
    case RuntimeObjectClaimResult::Claimed:
      return std::unexpected(QueryScalarReadError::UnsupportedHandle);
    case RuntimeObjectClaimResult::ProbeLimitExceeded:
    case RuntimeObjectClaimResult::Unreadable:
      return std::unexpected(QueryScalarReadError::Unreadable);
    }
  }
  return QueryScalar{std::move(text)};
}

} // namespace zigzag
