// Phase 5 plumbing: the per-agent lineage Store, the dimension-filing
// sweep, and the promote → rebuildManifold → fresh-context re-genesis
// cycle (design/vagent-metaprogramming-implementation.md lines 374-524;
// spec §10.1/§6.6's ordering — seed → mint/file → fold → pin/inject →
// first-resolution, on the spawn lineage and the resume lineage alike).
#include "common/xanadu/vagent/lineage.hpp"

#include <array>
#include <format>
#include <utility>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace xanadu::vagent {
namespace {

/// Refusal-factored wrap: historyFromSeal and bindPublishedLocalScroll
/// throw; the seam reports errors as expected.
[[nodiscard]] std::expected<LineageStore, std::string>
sealToStore(const std::span<const std::string_view> segments,
            const Scroll &from, const std::map<std::string, Scroll> &scrolls,
            const ContentSource *const source,
            const DocumentId *const documentId) {
  try {
    auto store = xanadu::historyFromSeal(segments, from, scrolls, documentId);
    store->setContentSource(source);
    // addScroll dedups on sameContentAs — the history already holds `from`
    // at selfScrollInHistory, so this re-asks for the id rather than
    // filing a second external (store.cpp:1388-1405).
    const auto self = store->addScroll(from);
    store->bindPublishedLocalScroll(self);
    return LineageStore{std::move(store), self};
  } catch (const std::exception &error) {
    return std::unexpected(std::format("seedLineageStore: {}", error.what()));
  }
}

/// The sweep itself: getOrCreate per missing name on the persistent
/// d.dims rank — each mint a permanent rank append, so the walk is
/// idempotent by name and re-entrant per cycle. `head`/`fold` ride the
/// loop because getOrCreate's rebuild IS the injected fold.
[[nodiscard]] std::expected<DimensionFiling, std::string>
fileNameSet(Store &lineage, MicroversionId head,
            std::vector<std::string> names) {
  if (head.isZero()) {
    head = lineage.latest();
  }
  // Genesis up front, not inside the first getOrCreate: the registry
  // runs sliceGenesis then mints unconditionally
  // (dimension_registry.cpp:202-216), so a store that needed genesis
  // inside the call would mint "d.dims" a second time.
  if (!lineage.homeCell()) {
    head = lineage.sliceGenesis(head);
  }
  auto fold = lineage.rebuildManifold(head);
  for (const auto &name : names) {
    auto filed = zigzag::DimensionRegistry::instance().getOrCreate(
        lineage, head, fold, name);
    if (!filed) {
      return std::unexpected(std::format("fileDimensionVocabulary: '{}' — {}",
                                         name,
                                         zigzag::toString(filed.error())));
    }
  }
  return DimensionFiling{head, std::move(fold), std::move(names)};
}

} // namespace

// ---------------------------------------------------------------------------
// seed
// ---------------------------------------------------------------------------

std::expected<LineageStore, std::string>
seedLineageStore(const std::span<const std::string_view> segments,
                 const Scroll &from,
                 const std::map<std::string, Scroll> &scrolls,
                 const ContentSource *const source,
                 const DocumentId *const documentId) {
  return sealToStore(segments, from, scrolls, source, documentId);
}

std::expected<LineageStore, std::string>
seedLineageStore(const std::string_view sealed, const Scroll &from,
                 const std::map<std::string, Scroll> &scrolls,
                 const ContentSource *const source,
                 const DocumentId *const documentId) {
  const std::array<std::string_view, 1> one{sealed};
  return sealToStore(std::span<const std::string_view>{one}, from, scrolls,
                     source, documentId);
}

std::expected<Promoted, std::string>
seedProgramImage(Store &lineage, ArenaManifold &arena,
                 const CellRef programRoot) {
  // Root = the program root, once — the image band the (a) comparand and
  // the (c) programRef bind then name. Every later promote roots at the
  // agent cell alone.
  auto promoted =
      zigzag::promote(lineage, lineage.latest(), arena, programRoot);
  if (!promoted) {
    return std::unexpected(
        "seedProgramImage: promote refused the program image "
        "(root not an arena cell, a relocation cycle, or over budget)");
  }
  return *promoted;
}

// ---------------------------------------------------------------------------
// mint/file
// ---------------------------------------------------------------------------

std::set<std::string> dimensionVocabulary(const ArenaManifold &arena) {
  std::set<std::string> names;
  for (const auto name : kGenesisDimensionNames) {
    names.emplace(name);
  }
  for (const auto name : kMetaDimensionNames) {
    names.emplace(name);
  }
  for (const auto name : kAgentDimensionNames) {
    names.emplace(name);
  }
  // The base∪arenaDims_ surface — never the arena-local d.dims rank.
  // Names live as interned dim-cell content; an unreadable one simply
  // has no name to file.
  for (const auto dim : arena.dimensions()) {
    if (const auto name = arena.textOf(dim); !name.empty()) {
      names.emplace(name);
    }
  }
  return names;
}

std::expected<void, std::string> fileDimensionVocabulary(Store &lineage,
                                                         ArenaManifold &arena) {
  auto filing = fileDimensionVocabulary(lineage, arena, MicroversionId{});
  if (!filing) {
    return std::unexpected(std::move(filing.error()));
  }
  return {};
}

std::expected<DimensionFiling, std::string>
fileDimensionVocabulary(Store &lineage, ArenaManifold &arena,
                        MicroversionId head) {
  auto names = dimensionVocabulary(arena);
  return fileNameSet(lineage, head, {names.begin(), names.end()});
}

std::expected<DimensionFiling, std::string>
fileDimensionNames(Store &lineage, MicroversionId head,
                   const std::span<const std::string_view> names) {
  std::vector<std::string> deduped;
  deduped.reserve(names.size());
  for (const auto name : names) {
    if (!name.empty() && std::ranges::find(deduped, name) == deduped.end()) {
      deduped.emplace_back(name);
    }
  }
  return fileNameSet(lineage, head, std::move(deduped));
}

// ---------------------------------------------------------------------------
// fold → pin/inject
// ---------------------------------------------------------------------------

std::expected<RegenesisResult, std::string>
regenesisCycle(Store &lineage, ArenaManifold &arena, const CellRef agentRoot) {
  // The promote destination is arena.base()->store() by contract — a
  // lineage mix would silently graft one document's state onto another,
  // so a base that names a different store is a refusal, not a warning.
  if (arena.base() != nullptr) {
    const auto *const baseStore = arena.base()->store();
    if (baseStore != nullptr && baseStore != &lineage) {
      return std::unexpected(
          "regenesisCycle: the arena's base fold names a different Store");
    }
  }

  // 1. The session's promote-visible mutations ride the agent cell's
  //    reachable subgraph — ephemeral cells mint persistent twins,
  //    holdsOwn cells' link-writes become SetLink ops.
  auto promoted = zigzag::promote(lineage, lineage.latest(), arena, agentRoot);
  if (!promoted) {
    return std::unexpected(
        "regenesisCycle: promote refused the agent cell's subgraph "
        "(root not an arena cell, a relocation cycle, or over budget)");
  }

  // 2. Filing BEFORE the rebuild: the sweep enumerates the still-live
  //    session arena — its session mints and any genesis re-mints from
  //    the last cycle file now — and each makeDimension lands inside the
  //    fold about to be sealed, not outside it.
  auto filing = fileDimensionVocabulary(lineage, arena, promoted->version);
  if (!filing) {
    return std::unexpected(std::move(filing.error()));
  }

  // 3–4. Pin and inject: the fold is heap-held because the fresh
  //    context's arena carries a raw `base_` pointer into it, and the
  //    bundle is non-movable by design (worker_context.hpp:27-30).
  auto fold    = std::make_unique<Manifold>(std::move(filing->fold));
  auto context = std::make_unique<WorkerContext>(fold.get(), &lineage);

  // The agent cell's persistent name for the next cycle: promote mints
  // one twin per ephemeral, in discovery order — root is order[0], so a
  // minted root is cells.front(); a base-cell root already named itself.
  // isEphemeral, not denseOf: a shadowed base cell also answers denseOf
  // (the overlay owns a slot for it) but keeps its persistent name.
  auto persistent = agentRoot;
  if (zigzag::isEphemeral(agentRoot) && !promoted->cells.empty()) {
    persistent = promoted->cells.front();
  }

  return RegenesisResult{filing->head, persistent, std::move(fold),
                         std::move(context)};
}

} // namespace xanadu::vagent
