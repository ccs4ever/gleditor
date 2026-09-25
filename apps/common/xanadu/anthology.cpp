/**
 * @file anthology.cpp
 * @brief Cross-store ranks: the anthology (§5.9).
 */
#include "common/xanadu/anthology.hpp"

#include "common/xanadu/extern_ref.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu {

std::optional<AnthologyMember>
readAnthologyMember(const zigzag::Manifold &manifold, const Store &store,
                    const zigzag::CellRef cell) {
  if (zigzag::noCell == cell || !manifold.contains(cell)) {
    return std::nullopt;
  }

  AnthologyMember res;
  res.cell  = cell;
  res.label = manifold.textOf(cell, store);

  const auto dimMemberOpt     = manifold.dimensionNamed("d.member", store);
  zigzag::CellRef placeholder = zigzag::noCell;
  if (dimMemberOpt.has_value()) {
    for (const auto c : zigzag::rankAfter(manifold, cell, *dimMemberOpt,
                                          zigzag::DimVector::POS)) {
      if (manifold.valueKindOf(c) == ValueKind::ExternRef ||
          store.externTarget(c).has_value()) {
        placeholder = c;
        break;
      }
      placeholder = c;
    }
  }

  if (zigzag::noCell != placeholder) {
    res.kind            = AnthologyMemberKind::ForeignEntry;
    res.placeholderCell = placeholder;
    res.externRef       = store.externTarget(placeholder);

    const auto dimMemberStateOpt =
        manifold.dimensionNamed("d.member-state", store);
    const auto stateCellOpt =
        dimMemberStateOpt ? zigzag::step(manifold, cell, *dimMemberStateOpt)
                          : std::nullopt;
    if (stateCellOpt.has_value() && *stateCellOpt != zigzag::noCell) {
      res.stateCell   = *stateCellOpt;
      const auto text = manifold.textOf(*stateCellOpt, store);
      res.pinnedState = readGlobalDocumentState(text);
    }
  } else {
    res.kind = AnthologyMemberKind::Local;
  }

  return res;
}

void forEachAnthologyMember(
    const zigzag::Manifold &manifold, const Store &store,
    const zigzag::CellRef root,
    gleditor::cpp26::function_ref<bool(const AnthologyMember &)> visitor) {
  if (zigzag::noCell == root || !manifold.contains(root)) {
    return;
  }

  const auto dimAnthologyOpt = manifold.dimensionNamed("d.anthology", store);
  if (!dimAnthologyOpt.has_value()) {
    return;
  }
  const auto dimAnthology = *dimAnthologyOpt;

  zigzag::CellRef start = root;
  if (root == store.homeCell()) {
    const auto first = zigzag::step(manifold, root, dimAnthology);
    if (!first.has_value() || *first == zigzag::noCell) {
      return;
    }
    start = *first;
  }

  for (const auto cell : zigzag::rank(manifold, start, dimAnthology)) {
    if (const auto m = readAnthologyMember(manifold, store, cell);
        m.has_value()) {
      if (!visitor(*m)) {
        break;
      }
    }
  }
}

std::vector<AnthologyMember> anthologyMembers(const zigzag::Manifold &manifold,
                                              const Store &store,
                                              const zigzag::CellRef root) {
  std::vector<AnthologyMember> members;
  forEachAnthologyMember(manifold, store, root, [&](const AnthologyMember &m) {
    members.push_back(m);
    return true;
  });
  return members;
}

ResolvedAnthologyMember
resolveAnthologyMember(const Store &localStore, const AnthologyMember &member,
                       const Store &foreignStore, const Scroll &sealedAs,
                       const zigzag::Manifold *const foreignFold) {
  ResolvedAnthologyMember res;
  res.member = member;

  if (member.isLocal()) {
    res.status              = AnthologyDisplayStatus::Local;
    res.resolvedForeignCell = member.cell;
    return res;
  }

  if (zigzag::noCell == member.placeholderCell || !member.externRef) {
    res.status = AnthologyDisplayStatus::Unintelligible;
    return res;
  }

  const auto extRes = resolveExternCell(localStore, member.placeholderCell,
                                        foreignStore, sealedAs, foreignFold);
  switch (extRes.status) {
  case ExternResolutionStatus::Resolved:
    res.status              = AnthologyDisplayStatus::Resolved;
    res.resolvedForeignCell = extRes.cell;
    break;
  case ExternResolutionStatus::NotFetched:
    res.status = AnthologyDisplayStatus::NotFetched;
    break;
  case ExternResolutionStatus::Absent:
    res.status = AnthologyDisplayStatus::Absent;
    break;
  case ExternResolutionStatus::Unintelligible:
    res.status = AnthologyDisplayStatus::Unintelligible;
    break;
  }
  return res;
}

ResolvedAnthologyMember
resolveAnthologyInArena(zigzag::ArenaManifold &arena, const Store &localStore,
                        const AnthologyMember &member,
                        const std::uint32_t foreignSpaceId,
                        const Scroll &sealedAs) {
  ResolvedAnthologyMember res;
  res.member = member;

  if (member.isLocal()) {
    res.status              = AnthologyDisplayStatus::Local;
    res.resolvedForeignCell = member.cell;
    return res;
  }

  const auto spaceOpt = arena.spaceAt(foreignSpaceId);
  if (!spaceOpt.has_value() || nullptr == spaceOpt->store) {
    res.status = AnthologyDisplayStatus::NotFetched;
    return res;
  }

  const auto &space = *spaceOpt;
  res = resolveAnthologyMember(localStore, member, *space.store, sealedAs,
                               space.manifold);
  if (res.status == AnthologyDisplayStatus::Resolved &&
      zigzag::noCell != res.resolvedForeignCell) {
    res.arenaProxy = arena.proxyFor(foreignSpaceId, res.resolvedForeignCell);
  }
  return res;
}

} // namespace xanadu
