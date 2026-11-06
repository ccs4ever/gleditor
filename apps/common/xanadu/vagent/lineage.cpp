// Phase 5 plumbing: the per-agent lineage Store, the dimension-filing
// sweep, and the promote → rebuildManifold → fresh-context re-genesis
// cycle (design/vagent-metaprogramming-implementation.md lines 374-524;
// spec §10.1/§6.6's ordering — seed → mint/file → fold → pin/inject →
// first-resolution, on the spawn lineage and the resume lineage alike).
#include "common/xanadu/vagent/lineage.hpp"

#include <array>
#include <bit>
#include <format>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
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

/// The `+d.owner`/`+d.holder` convention mints every scope arm as
/// Int64(the named cell's position) — scalar content, so promote keeps
/// the bits verbatim while the named cell mints a persistent twin, and
/// the arm stales across a checkpoint exactly the way the ops
/// enumeration's Int64 members do. Restate each promoted record's arm
/// at the twin of the position it names; an arm naming a cell outside
/// the promoted subgraph (or a plain integer) stays verbatim. Without
/// this a scoped record's `owns` check fails closed post-genesis —
/// the read path's dim resolves, the arm names a dead position, and
/// the record is skipped wherever it sits.
[[nodiscard]] xanadu::MicroversionId
restateScopeArms(Store &lineage, xanadu::MicroversionId version,
                 ArenaManifold &arena, const zigzag::Promoted &promoted) {
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> twin;
  twin.reserve(promoted.arena.size());
  for (std::size_t i = 0; i < promoted.arena.size(); ++i)
    twin.emplace(promoted.arena[i], promoted.cells[i]);
  const auto twinOf = [&](zigzag::CellRef ref) {
    if (!zigzag::isEphemeral(ref)) return ref;
    const auto it = twin.find(ref);
    return it == twin.end() ? zigzag::noCell : it->second;
  };
  for (const std::string_view dimName : {"d.owner", "d.holder"}) {
    zigzag::DimRef scopeDim = zigzag::noCell;
    for (const auto dim : arena.dimensions())
      if (arena.textOf(dim) == dimName) {
        scopeDim = dim;
        break;
      }
    if (scopeDim == zigzag::noCell) continue;
    for (const auto arenaPos : promoted.arena) {
      const auto arm =
          arena.linked(arenaPos, scopeDim, zigzag::DimVector::POS);
      if (!arm) continue;
      const auto named = arena.asInt64(*arm);
      if (!named) continue;
      const auto position = twinOf(static_cast<zigzag::CellRef>(*named));
      if (position == zigzag::noCell ||
          position == static_cast<zigzag::CellRef>(*named))
        continue;
      const auto foldArm = twinOf(*arm);
      if (foldArm == zigzag::noCell) continue;
      version = lineage.setValue(version, foldArm, xanadu::PrimediaSpan{},
                                 xanadu::ValueKind::Int64,
                                 std::bit_cast<std::uint64_t>(
                                     static_cast<std::int64_t>(position)));
    }
  }
  return version;
}

/// The emitter's recipe records the operand slots: `+d.refs` members
/// mark their slot `+d.ref`, `+d.recipe` ref-capture members mark it
/// `+d.refFor`. Each marked slot's Int64 is an arena position authored
/// at emission — verbatim through promote, stale on the fold — and
/// genesis (e) rebinds only by name on the `+d.code` heads it sweeps.
/// Clause spines and unregistered heads have no (e) arm at all, so the
/// publish restates every marked slot at the twin of the position it
/// names — the name-records' labels still outrank the slot value when
/// (e) does run.
[[nodiscard]] xanadu::MicroversionId
restateRefSlots(Store &lineage, xanadu::MicroversionId version,
                ArenaManifold &arena, const zigzag::Promoted &promoted) {
  std::unordered_map<zigzag::CellRef, zigzag::CellRef> twin;
  twin.reserve(promoted.arena.size());
  for (std::size_t i = 0; i < promoted.arena.size(); ++i)
    twin.emplace(promoted.arena[i], promoted.cells[i]);
  const auto twinOf = [&](zigzag::CellRef ref) {
    if (!zigzag::isEphemeral(ref)) return ref;
    const auto it = twin.find(ref);
    return it == twin.end() ? zigzag::noCell : it->second;
  };
  const auto dimOf = [&](std::string_view name) {
    for (const auto dim : arena.dimensions())
      if (arena.textOf(dim) == name) return dim;
    return zigzag::DimRef{zigzag::noCell};
  };
  const auto refsDim   = dimOf("d.refs");
  const auto recipeDim = dimOf("d.recipe");
  const auto refDim    = dimOf("d.ref");
  const auto refForDim = dimOf("d.refFor");
  const auto stepDim   = dimOf("d.step");
  if (refsDim == zigzag::noCell || recipeDim == zigzag::noCell ||
      refDim == zigzag::noCell || refForDim == zigzag::noCell ||
      stepDim == zigzag::noCell)
    return version;
  const auto restate = [&](zigzag::CellRef slot) {
    const auto named = arena.asInt64(slot);
    if (!named) return;
    const auto position = twinOf(static_cast<zigzag::CellRef>(*named));
    if (position == zigzag::noCell ||
        position == static_cast<zigzag::CellRef>(*named))
      return;
    const auto foldSlot = twinOf(slot);
    if (foldSlot == zigzag::noCell) return;
    version = lineage.setValue(version, foldSlot, xanadu::PrimediaSpan{},
                               xanadu::ValueKind::Int64,
                               std::bit_cast<std::uint64_t>(
                                   static_cast<std::int64_t>(position)));
  };
  for (const auto head : promoted.arena) {
    for (auto record = arena.linked(head, refsDim, zigzag::DimVector::POS);
         record;
         record = arena.linked(*record, stepDim, zigzag::DimVector::POS))
      if (const auto slot =
              arena.linked(*record, refDim, zigzag::DimVector::POS))
        restate(*slot);
    for (auto member = arena.linked(head, recipeDim, zigzag::DimVector::POS);
         member;
         member = arena.linked(*member, stepDim, zigzag::DimVector::POS))
      if (const auto slot =
              arena.linked(*member, refForDim, zigzag::DimVector::POS)) {
        restate(*slot);
        restate(*member);
      }
  }
  // The dense half of the relocation contract: a `d.mesh.relocation`
  // mark on a slot cell is itself the manifest — vars-record member
  // refs, record fields and staged-outbox refs ride `refCell` with no
  // recipe chain to find them by. The rewrite is idempotent beside the
  // chain arm's (a slot either walk reaches names the same twin).
  if (const auto relocDim = dimOf("d.mesh.relocation");
      relocDim != zigzag::noCell)
    for (std::size_t dense = 0; dense < arena.cellCount(); ++dense) {
      const auto cell =
          static_cast<zigzag::CellRef>(zigzag::ephemeralBit | dense);
      if (!arena.holdsOwn(cell)) continue;
      if (!arena.linked(cell, relocDim, zigzag::DimVector::POS))
        continue;
      restate(cell);
    }
  return version;
}

/// Every slot cell carrying the `+d.mesh.relocation → "cell"` mark —
/// the dense half of the manifest walk, slot-level so the caller
/// decides which slots count (a regenesis cycle seeds only
/// session-minted slots' referents; the recipe's band cells are
/// replay-minted and must stay out).
[[nodiscard]] std::vector<zigzag::CellRef>
markedRelocationSlots(ArenaManifold &arena) {
  zigzag::DimRef marker = zigzag::noCell;
  for (const auto dim : arena.dimensions())
    if (arena.textOf(dim) == "d.mesh.relocation") {
      marker = dim;
      break;
    }
  std::vector<zigzag::CellRef> slots;
  if (marker == zigzag::noCell) return slots;
  for (std::size_t dense = 0; dense < arena.cellCount(); ++dense) {
    const auto cell =
        static_cast<zigzag::CellRef>(zigzag::ephemeralBit | dense);
    if (!arena.holdsOwn(cell)) continue;
    if (arena.linked(cell, marker, zigzag::DimVector::POS))
      slots.push_back(cell);
  }
  return slots;
}

/// A non-name ref slot carries `+d.mesh.relocation → "cell"` — the
/// emission's own "this Int64 is a position" mark (fragment.cpp's
/// refsRecord). Promote's walk sees edges only, so a referent named
/// solely by that scalar never reaches the band: harvest every marked
/// slot's position as a promote root, and restateRefSlots then has a
/// twin to write back. Name-record slots re-resolve by label at
/// genesis (e), so only the marked slots need seeding.
[[nodiscard]] std::vector<zigzag::CellRef>
harvestRelocationTargets(ArenaManifold &arena) {
  std::vector<zigzag::CellRef> targets;
  for (const auto slot : markedRelocationSlots(arena)) {
    const auto named = arena.asInt64(slot);
    if (named && zigzag::isEphemeral(
                     static_cast<zigzag::CellRef>(*named)))
      targets.push_back(static_cast<zigzag::CellRef>(*named));
  }
  return targets;
}

} // namespace

// ---------------------------------------------------------------------------
// seed
// ---------------------------------------------------------------------------

std::expected<LineageStore, std::string> seedLineageStore(
    const std::span<const std::string_view> segments, const Scroll &from,
    const std::map<std::string, Scroll> &scrolls,
    const ContentSource *const source, const DocumentId *const documentId) {
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

namespace {

/// Find @p owner's +d.vars member whose content names @p key — the
/// varsRecord shape's own walk, written against whichever manifold the
/// caller is standing on (the compile arena for the authored side, the
/// rebuilt fold for the promoted side). Template so both spell one
/// lookup.
template <typename M>
[[nodiscard]] std::optional<zigzag::CellRef>
varsMember(M &m, const xanadu::SpanReader *reader, zigzag::CellRef owner,
           zigzag::DimRef vars, zigzag::DimRef values,
           std::string_view key) {
  const auto textOf = [&](zigzag::CellRef cell) {
    if constexpr (std::is_same_v<std::decay_t<M>, zigzag::ArenaManifold>)
      return m.textOf(cell, reader);
    else
      return m.textOf(cell, *reader);
  };
  for (const auto record : zigzag::rankAfter(m, owner, vars))
    if (textOf(record) == key)
      if (const auto v = m.linked(record, values, zigzag::DimVector::POS))
        return std::optional<zigzag::CellRef>{*v};
  return std::nullopt;
}

} // namespace

std::expected<Promoted, std::string>
seedProgramImage(Store &lineage, ArenaManifold &arena,
                 const CellRef programRoot) {
  // Root = the program root, once — the image band the (a) comparand and
  // the (c) programRef bind then name. Every later promote roots at the
  // agent cell alone. extraRoots carries the d.dims dim before anything
  // else discovered: R12's fiat reads home and dimsDim off the band's
  // first two positions, and the d.dims rank is the one rank promote's
  // discovery walk skips outright.
  const auto dimNamed = [&](std::string_view name) {
    for (const auto dim : arena.dimensions())
      if (arena.textOf(dim) == name) return dim;
    return zigzag::DimRef{zigzag::noCell};
  };
  const auto varsDim   = dimNamed("d.vars");
  const auto valuesDim = dimNamed("d.values");
  const auto stepDim   = dimNamed("d.step");
  const auto dimsDim   = dimNamed("d.dims");

  // The image record's "ops" enumeration holds Int64 arena positions —
  // scalar bits promote cannot read, so an op reachable only through
  // the enumeration would not promote at all. They join extraRoots:
  // each becomes a promote root and each gets a persistent twin for
  // the enumeration's restatement below.
  std::vector<zigzag::CellRef> extra;
  if (dimsDim != zigzag::noCell) extra.push_back(dimsDim);
  for (const auto target : harvestRelocationTargets(arena))
    extra.push_back(target);
  std::vector<zigzag::CellRef> arenaPositions;
  if (varsDim != zigzag::noCell && valuesDim != zigzag::noCell &&
      stepDim != zigzag::noCell)
    if (const auto imageRecord =
            varsMember(arena, nullptr, programRoot, varsDim, valuesDim,
                       "imageRecord"))
      if (const auto opsHead =
              varsMember(arena, nullptr, *imageRecord, varsDim,
                         valuesDim, "ops"))
        for (const auto member :
             zigzag::rankAfter(arena, *opsHead, stepDim))
          if (const auto position = arena.asInt64(member)) {
            arenaPositions.push_back(
                static_cast<zigzag::CellRef>(*position));
            if (zigzag::isEphemeral(*position))
              extra.push_back(static_cast<zigzag::CellRef>(*position));
          }

  auto promoted = zigzag::promote(lineage, lineage.latest(), arena,
                                  programRoot, {}, extra);
  if (!promoted) {
    return std::unexpected(
        "seedProgramImage: promote refused the program image "
        "(root not an arena cell, a relocation cycle, or over budget)");
  }
  if (arenaPositions.empty()) {
    promoted->version =
        restateScopeArms(lineage, promoted->version, arena, *promoted);
    promoted->version =
        restateRefSlots(lineage, promoted->version, arena, *promoted);
    return *promoted;
  }

  std::unordered_map<zigzag::CellRef, zigzag::CellRef> twin;
  twin.reserve(promoted->arena.size());
  for (std::size_t i = 0; i < promoted->arena.size(); ++i)
    twin.emplace(promoted->arena[i], promoted->cells[i]);

  // The fold walk mirrors the arena walk on the promoted twins — the
  // rank members sit in the same order both sides of the seed. A
  // lineage-backed compile arena's root is a base cell: promote maps
  // those to themselves, so the identity is the lookup's fallback.
  auto fold = lineage.rebuildManifold(promoted->version);
  const auto twinOf = [&](zigzag::CellRef ref) {
    if (!zigzag::isEphemeral(ref)) return ref;
    const auto it = twin.find(ref);
    return it == twin.end() ? zigzag::noCell : it->second;
  };
  const auto varsTwin   = twinOf(varsDim);
  const auto valuesTwin = twinOf(valuesDim);
  const auto stepTwin   = twinOf(stepDim);
  if (varsTwin == zigzag::noCell || valuesTwin == zigzag::noCell ||
      stepTwin == zigzag::noCell)
    return std::unexpected(
        "seedProgramImage: the record dims did not reach the band");
  const auto recordTwin =
      varsMember(fold, &lineage, twinOf(programRoot), varsTwin,
                 valuesTwin, "imageRecord");
  if (!recordTwin)
    return std::unexpected(
        "seedProgramImage: the image record did not reach the band");
  const auto opsTwin = varsMember(fold, &lineage, *recordTwin, varsTwin,
                                  valuesTwin, "ops");
  if (!opsTwin)
    return std::unexpected(
        "seedProgramImage: the ops enumeration did not reach the band");

  auto version = promoted->version;
  std::size_t index = 0;
  for (const auto member : zigzag::rankAfter(fold, *opsTwin, stepTwin)) {
    if (index >= arenaPositions.size())
      return std::unexpected(
          "seedProgramImage: the promoted ops rank outruns the authored "
          "enumeration");
    const auto position = twinOf(arenaPositions[index++]);
    if (position == zigzag::noCell)
      return std::unexpected(
          "seedProgramImage: a recorded op names no promoted cell");
    version = lineage.setValue(version, member, xanadu::PrimediaSpan{},
                               xanadu::ValueKind::Int64,
                               std::bit_cast<std::uint64_t>(
                                   static_cast<std::int64_t>(position)));
  }
  if (index != arenaPositions.size())
    return std::unexpected(
        "seedProgramImage: the promoted ops rank undercuts the authored "
        "enumeration");
  promoted->version = restateScopeArms(lineage, version, arena, *promoted);
  promoted->version =
      restateRefSlots(lineage, promoted->version, arena, *promoted);
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
regenesisCycle(Store &lineage, ArenaManifold &arena, const CellRef agentRoot,
               const std::uint32_t imageExtent) {
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
  //    holdsOwn cells' link-writes become SetLink ops. The state-side
  //    shadows join as extraRoots: a rank append at depth ≥2 sits on a
  //    shadowed cell behind unshadowed base records, which the root
  //    walk deliberately never traverses. Program-side shadows — refs
  //    at or below the image band — are verb-replayed writes genesis
  //    re-derives; seeding them would promote the installed copy the
  //    replay then re-mints.
  std::vector<CellRef> durable;
  for (const CellRef ref : arena.shadowOrder()) {
    if (!zigzag::isEphemeral(ref) && ref > imageExtent) {
      durable.push_back(ref);
    }
  }
  // The marked-ref harvest, convention-filtered: a state-side
  // `d.mesh.relocation` slot's Int64-carried referent never reaches
  // promote through the edge walk — a `var child` member or a staged
  // entry's payload lives only behind that scalar. The recipe's own
  // marks pair their slots through +d.ref/+d.refFor (fragment.cpp's
  // refsRecord), and genesis replay re-mints those referents —
  // seeding one re-installs the clause beside the replayed copy.
  const auto refDim    = arena.dimensionNamed("d.ref");
  const auto refForDim = arena.dimensionNamed("d.refFor");
  for (const auto slot : markedRelocationSlots(arena)) {
    if (!zigzag::isEphemeral(slot)) continue;
    if ((refDim &&
         arena.linked(slot, *refDim, zigzag::DimVector::NEG)) ||
        (refForDim &&
         arena.linked(slot, *refForDim, zigzag::DimVector::NEG)))
      continue;
    const auto named = arena.asInt64(slot);
    if (!named) continue;
    const auto target = static_cast<CellRef>(*named);
    if (zigzag::isEphemeral(target)) durable.push_back(target);
  }
  // Opt the same state-side shadows into committing edge-clears: the
  // `d.patch`/`d.code` rank unlinks unlearn/de-registration drop can now be
  // durable removals, not only tombstone reads. The sideness test is the same
  // one the extraRoots above use; session-only value slots get Clears but
  // never Values and carry no edge-drops, so they stay additive.
  const auto commitPolicy = zigzag::commitStateSide(imageExtent);
  zigzag::PromotionBudget budget{};
  budget.commitShadowed = commitPolicy;
  auto promoted = zigzag::promote(lineage, lineage.latest(), arena, agentRoot,
                                  budget, durable);
  if (!promoted) {
    return std::unexpected(
        "regenesisCycle: promote refused the agent cell's subgraph "
        "(root not an arena cell, a relocation cycle, or over budget)");
  }
  promoted->version =
      restateScopeArms(lineage, promoted->version, arena, *promoted);
  promoted->version =
      restateRefSlots(lineage, promoted->version, arena, *promoted);

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
