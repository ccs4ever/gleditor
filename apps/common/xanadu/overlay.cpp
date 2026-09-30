/**
 * @file overlay.cpp
 * @brief Implementation of plural structure maps and overlay composition
 * (§5.12).
 */
#include "common/xanadu/overlay.hpp"

#include <algorithm>
#include <ranges>
#include <stdexcept>

#include "common/xanadu/torrent.hpp"

namespace xanadu {

namespace {

zigzag::CellRef traceMakeCell(const Store &store,
                              const std::uint32_t startOpIndex) {
  std::uint32_t curr = startOpIndex;
  std::size_t steps  = 0;
  while (curr != 0 && steps < 10000) {
    const auto *node = store.getCompactOp(curr);
    if (nullptr == node || OpKind::Structure != node->kind) {
      break;
    }
    if (structureVerbOf(node->flags) == StructureVerb::MakeCell) {
      return curr;
    }
    if (node->sourceOpIndex >= curr || node->sourceOpIndex == 0) {
      return curr;
    }
    curr = node->sourceOpIndex;
    ++steps;
  }
  return curr != 0 ? curr : zigzag::noCell;
}

} // namespace

OverlayTargetResult declareOverlayTarget(Store &store,
                                         const MicroversionId &parent,
                                         const GlobalDocumentState &targetState,
                                         zigzag::CellRef releaseCell) {
  auto curHead = parent.isZero() ? store.latest() : parent;
  auto fold    = store.rebuildManifold(curHead);

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto dim = fold.dimensionNamed(name, store);
    if (!dim) {
      const auto minted = store.makeDimension(curHead, name, &fold);
      curHead           = minted.version;
      fold              = store.rebuildManifold(curHead);
      return minted.dim;
    }
    return *dim;
  };

  const auto dimOverlayTargets = ensureDim(kDimOverlayTargets);
  const auto dimScrollRefs     = ensureDim("d.scroll-refs");

  if (!store.scrollRegistry().scrollIdForKey(targetState.scroll)) {
    curHead = store.registerScroll(curHead, targetState.scroll, &fold);
    fold    = store.rebuildManifold(curHead);
  }

  if (releaseCell == zigzag::noCell) {
    curHead     = store.makeCell(curHead, "overlay_root");
    releaseCell = store.cellRefOf(curHead);
    fold        = store.rebuildManifold(curHead);
  }

  const auto encodedState = writeGlobalDocumentState(targetState);
  curHead                 = store.makeCell(curHead, encodedState);
  const auto targetCell   = store.cellRefOf(curHead);
  fold                    = store.rebuildManifold(curHead);

  const auto targetsTail =
      zigzag::rankTail(fold, releaseCell, dimOverlayTargets);
  curHead = store.setLink(curHead, targetsTail, dimOverlayTargets,
                          zigzag::DimVector::POS, targetCell, &fold);
  fold    = store.rebuildManifold(curHead);

  auto reg = fold.scrollRegistry(store);
  if (const auto sid = reg.scrollIdForKey(targetState.scroll);
      sid.has_value()) {
    if (const auto rec = reg.findRecord(*sid);
        rec.has_value() && rec->cell != zigzag::noCell) {
      const auto scrollTail = zigzag::rankTail(fold, rec->cell, dimScrollRefs);
      curHead = store.setLink(curHead, scrollTail, dimScrollRefs,
                              zigzag::DimVector::POS, targetCell, &fold);
    }
  }

  return OverlayTargetResult{
      .version     = curHead,
      .targetCell  = targetCell,
      .releaseCell = releaseCell,
  };
}

OverlayTargetResult
declareOverlayTarget(Store &store, const MicroversionId &parent,
                     const zigzag::CellRef releaseCell,
                     const GlobalDocumentState &targetState) {
  return declareOverlayTarget(store, parent, targetState, releaseCell);
}

OverlayClaimResult
authorOverlayClaim(Store &store, const MicroversionId &parent,
                   const zigzag::CellRef from, const zigzag::DimRef dim,
                   const zigzag::DimVector dir, const zigzag::CellRef to,
                   zigzag::CellRef releaseCell, const std::string_view label) {
  auto curHead = parent.isZero() ? store.latest() : parent;
  auto fold    = store.rebuildManifold(curHead);

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto d = fold.dimensionNamed(name, store);
    if (!d) {
      const auto minted = store.makeDimension(curHead, name, &fold);
      curHead           = minted.version;
      fold              = store.rebuildManifold(curHead);
      return minted.dim;
    }
    return *d;
  };

  const auto dimOverlayClaims = ensureDim(kDimOverlayClaims);

  if (releaseCell == zigzag::noCell) {
    curHead     = store.makeCell(curHead, "overlay_root");
    releaseCell = store.cellRefOf(curHead);
    fold        = store.rebuildManifold(curHead);
  }

  curHead              = store.setLink(curHead, from, dim, dir, to, &fold);
  const auto setLinkOp = store.segmentedOps().indexOf(curHead);
  fold                 = store.rebuildManifold(curHead);

  curHead               = store.makeOpHandle(curHead, setLinkOp, label);
  const auto handleCell = store.cellRefOf(curHead);
  fold                  = store.rebuildManifold(curHead);

  const auto claimsTail = zigzag::rankTail(fold, releaseCell, dimOverlayClaims);
  curHead = store.setLink(curHead, claimsTail, dimOverlayClaims,
                          zigzag::DimVector::POS, handleCell, &fold);

  return OverlayClaimResult{
      .version = curHead, .handleCell = handleCell, .releaseCell = releaseCell};
}

OverlayClaimResult
authorOverlayClaim(Store &store, const MicroversionId &parent,
                   const zigzag::CellRef releaseCell,
                   const zigzag::CellRef from, const zigzag::DimRef dim,
                   const zigzag::DimVector dir, const zigzag::CellRef to,
                   const std::string_view label) {
  return authorOverlayClaim(store, parent, from, dim, dir, to, releaseCell,
                            label);
}

OverlayClaimResult authorOverlayBreak(
    Store &store, const MicroversionId &parent, const zigzag::CellRef from,
    const zigzag::DimRef dim, const zigzag::DimVector dir,
    const zigzag::CellRef releaseCell, const std::string_view label) {
  return authorOverlayClaim(store, parent, from, dim, dir, zigzag::noCell,
                            releaseCell, label);
}

OverlayClaimResult
authorOverlayBreak(Store &store, const MicroversionId &parent,
                   const zigzag::CellRef releaseCell,
                   const zigzag::CellRef from, const zigzag::DimRef dim,
                   const zigzag::DimVector dir, const std::string_view label) {
  return authorOverlayClaim(store, parent, from, dim, dir, zigzag::noCell,
                            releaseCell, label);
}

OverlayReleaseResult sealOverlayRelease(Store &store,
                                        const MicroversionId &parent,
                                        zigzag::CellRef releaseCell,
                                        const std::string_view label) {
  auto curHead = parent.isZero() ? store.latest() : parent;
  auto fold    = store.rebuildManifold(curHead);
  if (releaseCell == zigzag::noCell) {
    curHead     = store.makeCell(curHead, label.empty() ? "release" : label);
    releaseCell = store.cellRefOf(curHead);
  } else {
    curHead = store.setCellText(curHead, releaseCell,
                                label.empty() ? "release" : label, &fold);
  }
  return OverlayReleaseResult{.version = curHead, .releaseCell = releaseCell};
}

RebaseResult rebaseOverlay(Store &overlayStore, const MicroversionId &parent,
                           const zigzag::CellRef releaseCell,
                           const Store &targetStore,
                           const GlobalDocumentState &newTargetState,
                           const std::string_view newReleaseLabel) {
  auto curHead = parent.isZero() ? overlayStore.latest() : parent;
  auto fold    = overlayStore.rebuildManifold(curHead);

  auto ensureDim = [&](const std::string_view name) -> zigzag::DimRef {
    auto d = fold.dimensionNamed(name, overlayStore);
    if (!d) {
      const auto minted = overlayStore.makeDimension(curHead, name, &fold);
      curHead           = minted.version;
      fold              = overlayStore.rebuildManifold(curHead);
      return minted.dim;
    }
    return *d;
  };

  const auto dimTargets = ensureDim(kDimOverlayTargets);
  const auto dimClaims  = ensureDim(kDimOverlayClaims);

  // Mint new release cell to preserve hypertime history of old release
  const auto newRelOp = overlayStore.makeCell(
      curHead, newReleaseLabel.empty() ? "rebased" : newReleaseLabel);
  const auto newRelCell = overlayStore.cellRefOf(newRelOp);
  curHead               = newRelOp;
  fold                  = overlayStore.rebuildManifold(curHead);

  // Declare new target state on d.overlay-targets posward from new release cell
  const auto targetRes =
      declareOverlayTarget(overlayStore, curHead, newTargetState, newRelCell);
  curHead = targetRes.version;
  fold    = overlayStore.rebuildManifold(curHead);

  // Link existing claims posward from new release cell
  const auto firstClaim =
      fold.linked(releaseCell, dimClaims, zigzag::DimVector::POS);
  if (firstClaim != zigzag::noCell) {
    curHead = overlayStore.setLink(curHead, newRelCell, dimClaims,
                                   zigzag::DimVector::POS, firstClaim, &fold);
    fold    = overlayStore.rebuildManifold(curHead);
  }

  // Validate existing claims against newTargetState
  std::vector<OverlayCandidate> conflicts;
  for (const auto claimHandle :
       zigzag::rankAfter(fold, newRelCell, dimClaims)) {
    const auto opIndex = fold.handleTarget(claimHandle);
    if (!opIndex.has_value()) {
      continue;
    }
    const auto *node = overlayStore.getCompactOp(*opIndex);
    if (!node || node->kind != OpKind::Structure) {
      continue;
    }

    const auto from = traceMakeCell(overlayStore, node->sourceOpIndex);
    if (const auto extFrom = overlayStore.externTarget(from);
        extFrom.has_value()) {
      if (extFrom->produces != newTargetState.version &&
          !extFrom->produces.isAncestorOf(newTargetState.version)) {
        conflicts.push_back(OverlayCandidate{
            .claimHandle  = claimHandle,
            .from         = from,
            .dimension    = node->linkId,
            .direction    = (node->flags & structureNegward)
                                ? zigzag::DimVector::NEG
                                : zigzag::DimVector::POS,
            .to           = node->to,
            .state        = OverlayClaimState::OutsideSnapshot,
            .claimOpIndex = *opIndex,
        });
      }
    }
    if (node->to != zigzag::noCell) {
      if (const auto extTo = overlayStore.externTarget(node->to);
          extTo.has_value()) {
        if (extTo->produces != newTargetState.version &&
            !extTo->produces.isAncestorOf(newTargetState.version)) {
          conflicts.push_back(OverlayCandidate{
              .claimHandle  = claimHandle,
              .from         = from,
              .dimension    = node->linkId,
              .direction    = (node->flags & structureNegward)
                                  ? zigzag::DimVector::NEG
                                  : zigzag::DimVector::POS,
              .to           = node->to,
              .state        = OverlayClaimState::OutsideSnapshot,
              .claimOpIndex = *opIndex,
          });
        }
      }
    }
  }

  // Touch new release cell last to pin publication state (§5.12 §8)
  const auto sealRes =
      sealOverlayRelease(overlayStore, curHead, newRelCell,
                         newReleaseLabel.empty() ? "rebased" : newReleaseLabel);

  return RebaseResult{
      .version           = sealRes.version,
      .newReleaseCell    = newRelCell,
      .conflictingClaims = std::move(conflicts),
  };
}

DhtTarget overlayRendezvousTarget(const std::string_view targetScrollKey) {
  return DhtTarget{sha1("structure-overlays:" + std::string(targetScrollKey))};
}

// -- OverlayComposition ------------------------------------------------------

OverlayComposition &OverlayComposition::withTarget(TargetSpec spec) & {
  targets_.push_back(std::move(spec));
  return *this;
}

OverlayComposition &&OverlayComposition::withTarget(TargetSpec spec) && {
  targets_.push_back(std::move(spec));
  return std::move(*this);
}

OverlayComposition &
OverlayComposition::withOverlay(OverlayAttachment overlay) & {
  for (const auto &link : overlay.classicLinks) {
    classicLinks_.push_back(link);
  }
  overlays_.push_back(std::move(overlay));
  return *this;
}

OverlayComposition &&
OverlayComposition::withOverlay(OverlayAttachment overlay) && {
  for (const auto &link : overlay.classicLinks) {
    classicLinks_.push_back(link);
  }
  overlays_.push_back(std::move(overlay));
  return std::move(*this);
}

OverlayComposition &
OverlayComposition::withCurators(std::set<PublicKey> followedCurators) & {
  followedCurators_ = std::move(followedCurators);
  return *this;
}

OverlayComposition &&
OverlayComposition::withCurators(std::set<PublicKey> followedCurators) && {
  followedCurators_ = std::move(followedCurators);
  return std::move(*this);
}

OverlayComposition &
OverlayComposition::withMaxPublicOverlays(const std::size_t maxCount) & {
  maxPublicOverlays_ = maxCount;
  return *this;
}

OverlayComposition &&
OverlayComposition::withMaxPublicOverlays(const std::size_t maxCount) && {
  maxPublicOverlays_ = maxCount;
  return std::move(*this);
}

OverlayComposition &OverlayComposition::withClassicLink(GlobalLink link) & {
  classicLinks_.push_back(std::move(link));
  return *this;
}

OverlayComposition &&OverlayComposition::withClassicLink(GlobalLink link) && {
  classicLinks_.push_back(std::move(link));
  return std::move(*this);
}

OverlayComposition &OverlayComposition::merge(const OverlayComposition &other) {
  for (const auto &t : other.targets_) {
    withTarget(t);
  }
  for (const auto &o : other.overlays_) {
    withOverlay(o);
  }
  for (const auto &c : other.followedCurators_) {
    followedCurators_.insert(c);
  }
  for (const auto &l : other.classicLinks_) {
    withClassicLink(l);
  }
  return compose();
}

OverlayComposition &OverlayComposition::compose() & {
  candidateMap_.clear();
  allCandidates_.clear();
  occupiedSlots_.clear();

  // 1. Attach target manifolds to arena
  for (auto &target : targets_) {
    zigzag::Space sp{
        .manifold = target.manifold,
        .store    = target.store,
        .label =
            !target.scrollKey.empty() ? target.scrollKey : target.state.scroll,
    };
    target.spaceId = arena_.attach(std::move(sp));
    if (target.manifold != nullptr) {
      for (const auto &slot : target.manifold->cells()) {
        if (slot.birthOp != zigzag::noCell) {
          arena_.proxyFor(target.spaceId, slot.birthOp);
        }
      }
    }
  }

  // 2. Attach overlay spaces to arena
  struct AttachedOverlayInfo {
    std::size_t overlayIndex{0};
    std::uint32_t spaceId{0};
    ProminenceTier tier{ProminenceTier::Public};
    std::int64_t score{0};
    std::shared_ptr<zigzag::Manifold> ownedFold;
  };
  std::vector<AttachedOverlayInfo> activeOverlays;

  for (std::size_t i = 0; i < overlays_.size(); ++i) {
    const auto &overlay = overlays_[i];
    std::shared_ptr<zigzag::Manifold> owned;
    const zigzag::Manifold *overlayFoldPtr = nullptr;
    if (overlay.store != nullptr) {
      owned = std::make_shared<zigzag::Manifold>(
          overlay.store->rebuildManifold(overlay.store->latest()));
      overlayFoldPtr = owned.get();
    }
    zigzag::Space sp{
        .manifold      = overlayFoldPtr,
        .ownedManifold = owned,
        .store         = overlay.store,
        .sealedAs      = overlay.sealedAs,
        .label         = overlay.publicationKey,
    };
    const auto spId = arena_.attach(std::move(sp));

    // Determine prominence tier: verified publisher against target author
    // (§5.12 §6)
    bool isAuthor = false;
    for (const auto &t : targets_) {
      if (t.author == overlay.publisher && !overlay.publisher.isZero()) {
        isAuthor = true;
        break;
      }
    }

    ProminenceTier tier = ProminenceTier::Public;
    std::int64_t score  = overlay.sequence;
    if (isAuthor) {
      tier  = ProminenceTier::Author;
      score = 2000 + overlay.sequence;
    } else if (followedCurators_.contains(overlay.publisher)) {
      tier  = ProminenceTier::Curated;
      score = 1000 + overlay.sequence;
    }

    activeOverlays.push_back(AttachedOverlayInfo{
        .overlayIndex = i,
        .spaceId      = spId,
        .tier         = tier,
        .score        = score,
        .ownedFold    = owned,
    });
  }

  // Filter public overlays according to maxPublicOverlays_ bound (§5.12 §6, §9)
  std::vector<AttachedOverlayInfo> nonPublic;
  std::vector<AttachedOverlayInfo> publicList;
  for (auto &ao : activeOverlays) {
    if (ao.tier == ProminenceTier::Public) {
      publicList.push_back(std::move(ao));
    } else {
      nonPublic.push_back(std::move(ao));
    }
  }

  std::ranges::sort(publicList, [&](const AttachedOverlayInfo &a,
                                    const AttachedOverlayInfo &b) {
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return overlays_[a.overlayIndex].publicationKey <
           overlays_[b.overlayIndex].publicationKey;
  });

  if (publicList.size() > maxPublicOverlays_) {
    publicList.resize(maxPublicOverlays_);
  }

  activeOverlays.clear();
  activeOverlays.reserve(nonPublic.size() + publicList.size());
  for (auto &ao : nonPublic) {
    activeOverlays.push_back(std::move(ao));
  }
  for (auto &ao : publicList) {
    activeOverlays.push_back(std::move(ao));
  }

  // 3. Bind published dimensions across all spaces (§5.11)
  arena_.bindSharedIdentities();

  // Bind standard system dimensions starting with "d." across spaces
  std::map<std::string, std::vector<std::pair<std::uint32_t, zigzag::DimRef>>>
      bySystemDimName;
  for (std::uint32_t spaceId = 1; spaceId <= arena_.spaceCount(); ++spaceId) {
    const auto s = arena_.spaceAt(spaceId);
    if (!s || !s->manifold) {
      continue;
    }
    for (const auto dim : s->manifold->dimensions()) {
      std::string name;
      if (s->reader) {
        name = s->manifold->textOf(dim, *s->reader);
      } else if (s->store) {
        name = s->manifold->textOf(dim, *s->store);
      }
      if (!name.empty() && name.starts_with("d.")) {
        bySystemDimName[name].push_back({spaceId, dim});
      }
    }
  }

  for (const auto &[name, members] : bySystemDimName) {
    if (members.size() <= 1) {
      continue;
    }
    zigzag::DimRef arenaDim = zigzag::noCell;
    for (const auto &[spaceId, dim] : members) {
      const auto existing = arena_.arenaDimFor(spaceId, dim);
      if (existing != zigzag::noCell) {
        arenaDim = existing;
        break;
      }
    }
    if (arenaDim == zigzag::noCell) {
      arenaDim = arena_.ensureDimension(name);
    }
    for (const auto &[spaceId, dim] : members) {
      arena_.bindDimension(arenaDim, spaceId, dim,
                           zigzag::DimensionBindingMode::Explicit);
    }
  }

  // 4. Mark intrinsic target manifold links in occupiedSlots_ (§5.12 §5)
  for (const auto &target : targets_) {
    if (target.manifold == nullptr) {
      continue;
    }
    for (const auto &slot : target.manifold->cells()) {
      const auto cellRef = slot.birthOp;
      if (cellRef == zigzag::noCell) {
        continue;
      }
      const auto proxyFrom = arena_.proxyFor(target.spaceId, cellRef);

      for (const auto &dLink : target.manifold->dimensionsOf(cellRef)) {
        const auto foreignDim = dLink.dim;
        zigzag::DimRef arenaDim =
            arena_.arenaDimFor(target.spaceId, foreignDim);
        if (arenaDim == zigzag::noCell) {
          std::string dName;
          if (target.store != nullptr) {
            dName = target.manifold->textOf(foreignDim, *target.store);
          }
          if (dName.empty()) {
            dName = "dim_" + std::to_string(foreignDim);
          }
          arenaDim = arena_.ensureDimension(dName);
          arena_.bindDimension(arenaDim, target.spaceId, foreignDim,
                               zigzag::DimensionBindingMode::Explicit);
        }

        for (const auto dir :
             {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
          const auto neighbor = dLink.neighbor(dir);
          if (neighbor != zigzag::noCell) {
            const auto proxyTo = arena_.proxyFor(target.spaceId, neighbor);
            occupiedSlots_.insert(SlotKey{proxyFrom, arenaDim, dir});
            occupiedSlots_.insert(SlotKey{proxyTo, arenaDim, -dir});
          }
        }
      }
    }
  }

  // 5. Parse and collect candidate claims from overlays
  std::vector<OverlayCandidate> candidatesToArbitrate;

  for (const auto &ao : activeOverlays) {
    const auto &overlay = overlays_[ao.overlayIndex];
    if (overlay.store == nullptr || ao.ownedFold == nullptr) {
      continue;
    }
    const auto &overlayFold = *ao.ownedFold;

    const auto dimClaimsOpt =
        overlayFold.dimensionNamed(kDimOverlayClaims, *overlay.store);
    if (!dimClaimsOpt.has_value()) {
      continue;
    }
    const auto dimClaims = *dimClaimsOpt;

    std::vector<zigzag::CellRef> claimCells;
    if (overlay.releaseCell != zigzag::noCell) {
      if (overlayFold.handleTarget(overlay.releaseCell).has_value()) {
        claimCells.push_back(overlay.releaseCell);
      }
      for (const auto h :
           zigzag::rankAfter(overlayFold, overlay.releaseCell, dimClaims)) {
        claimCells.push_back(h);
      }
    } else {
      for (const auto &slot : overlayFold.cells()) {
        const auto c = slot.birthOp;
        if (c == zigzag::noCell) {
          continue;
        }
        if (overlayFold.linked(c, dimClaims, zigzag::DimVector::NEG) ==
                zigzag::noCell &&
            overlayFold.linked(c, dimClaims, zigzag::DimVector::POS) !=
                zigzag::noCell) {
          if (overlayFold.handleTarget(c).has_value()) {
            claimCells.push_back(c);
          }
          for (const auto h : zigzag::rankAfter(overlayFold, c, dimClaims)) {
            claimCells.push_back(h);
          }
        }
      }
    }

    for (const auto claimHandle : claimCells) {
      const auto opIndexOpt = overlayFold.handleTarget(claimHandle);
      if (!opIndexOpt.has_value()) {
        candidatesToArbitrate.push_back(OverlayCandidate{
            .claimHandle    = claimHandle,
            .tier           = ao.tier,
            .score          = ao.score,
            .state          = OverlayClaimState::Unintelligible,
            .publicationKey = overlay.publicationKey,
        });
        continue;
      }
      const auto opIndex = *opIndexOpt;
      const auto *node   = overlay.store->getCompactOp(opIndex);
      if (nullptr == node || OpKind::Structure != node->kind ||
          structureVerbOf(node->flags) != StructureVerb::SetLink) {
        candidatesToArbitrate.push_back(OverlayCandidate{
            .claimHandle    = claimHandle,
            .tier           = ao.tier,
            .score          = ao.score,
            .state          = OverlayClaimState::Unintelligible,
            .claimOpIndex   = opIndex,
            .publicationKey = overlay.publicationKey,
        });
        continue;
      }

      const zigzag::DimVector authoredDir = (node->flags & structureNegward)
                                                ? zigzag::DimVector::NEG
                                                : zigzag::DimVector::POS;
      const auto overlayDim               = node->linkId;
      const auto overlayTo                = node->to;
      const auto overlayFrom =
          traceMakeCell(*overlay.store, node->sourceOpIndex);

      OverlayClaimState claimState =
          OverlayClaimState::Shadowed; // Default pending arbitration

      // Resolve from cell
      zigzag::CellRef fromProxy = zigzag::noCell;
      if (const auto extFrom = overlay.store->externTarget(overlayFrom);
          extFrom.has_value()) {
        const auto rec =
            overlay.store->scrollRegistry().findRecord(extFrom->scroll);
        std::string targetScroll = rec ? rec->globalKey : "";

        const TargetSpec *matchedTarget = nullptr;
        for (const auto &t : targets_) {
          if ((!t.scrollKey.empty() && t.scrollKey == targetScroll) ||
              t.state.scroll == targetScroll) {
            matchedTarget = &t;
            break;
          }
        }

        if (nullptr == matchedTarget) {
          claimState = OverlayClaimState::TargetNotFetched;
        } else {
          const auto targetOp =
              matchedTarget->store != nullptr
                  ? matchedTarget->store->segmentedOps().indexOf(
                        extFrom->produces)
                  : 0;
          if (targetOp == 0) {
            claimState = OverlayClaimState::TargetAbsent;
          } else if (extFrom->produces != matchedTarget->state.version &&
                     !extFrom->produces.isAncestorOf(
                         matchedTarget->state.version)) {
            claimState = OverlayClaimState::OutsideSnapshot;
          } else {
            const auto targetCellRef =
                traceMakeCell(*matchedTarget->store, targetOp);
            fromProxy = arena_.proxyFor(matchedTarget->spaceId, targetCellRef);
          }
        }
      } else {
        fromProxy = arena_.proxyFor(ao.spaceId, overlayFrom);
      }

      // Resolve to cell
      zigzag::CellRef toProxy = zigzag::noCell;
      if (overlayTo != zigzag::noCell) {
        if (const auto extTo = overlay.store->externTarget(overlayTo);
            extTo.has_value()) {
          const auto rec =
              overlay.store->scrollRegistry().findRecord(extTo->scroll);
          std::string targetScroll = rec ? rec->globalKey : "";

          const TargetSpec *matchedTarget = nullptr;
          for (const auto &t : targets_) {
            if ((!t.scrollKey.empty() && t.scrollKey == targetScroll) ||
                t.state.scroll == targetScroll) {
              matchedTarget = &t;
              break;
            }
          }

          if (nullptr == matchedTarget) {
            claimState = OverlayClaimState::TargetNotFetched;
          } else {
            const auto targetOp =
                matchedTarget->store != nullptr
                    ? matchedTarget->store->segmentedOps().indexOf(
                          extTo->produces)
                    : 0;
            if (targetOp == 0) {
              claimState = OverlayClaimState::TargetAbsent;
            } else if (extTo->produces != matchedTarget->state.version &&
                       !extTo->produces.isAncestorOf(
                           matchedTarget->state.version)) {
              claimState = OverlayClaimState::OutsideSnapshot;
            } else {
              const auto targetCellRef =
                  traceMakeCell(*matchedTarget->store, targetOp);
              toProxy = arena_.proxyFor(matchedTarget->spaceId, targetCellRef);
            }
          }
        } else {
          toProxy = arena_.proxyFor(ao.spaceId, overlayTo);
        }
      }

      // Resolve dimension: if bound through bindSharedIdentities or system dim,
      // use bound dimension; otherwise, keep private to this overlay (§5.12 §7,
      // Test 11).
      zigzag::DimRef arenaDim = arena_.arenaDimFor(ao.spaceId, overlayDim);
      if (arenaDim == zigzag::noCell) {
        std::string dName = overlayFold.textOf(overlayDim, *overlay.store);
        if (dName.empty()) {
          dName = "dim_" + std::to_string(overlayDim);
        }
        arenaDim = arena_.ensureDimension(
            "overlay_" + std::to_string(ao.spaceId) + ":" + dName);
        arena_.bindDimension(arenaDim, ao.spaceId, overlayDim,
                             zigzag::DimensionBindingMode::Explicit);
      }

      candidatesToArbitrate.push_back(OverlayCandidate{
          .claimHandle    = claimHandle,
          .from           = fromProxy,
          .dimension      = arenaDim,
          .direction      = authoredDir,
          .to             = toProxy,
          .tier           = ao.tier,
          .score          = ao.score,
          .state          = claimState,
          .claimOpIndex   = opIndex,
          .publicationKey = overlay.publicationKey,
          .claimVersion   = overlay.store->segmentedOps().idOf(opIndex),
      });
    }
  }

  // 6. Deterministic Two-Slot Arbitration (§5.12 §5)
  std::ranges::sort(candidatesToArbitrate,
                    [](const OverlayCandidate &a, const OverlayCandidate &b) {
                      if (a.tier != b.tier) {
                        return static_cast<std::uint8_t>(a.tier) <
                               static_cast<std::uint8_t>(b.tier);
                      }
                      if (a.score != b.score) {
                        return a.score > b.score;
                      }
                      if (a.publicationKey != b.publicationKey) {
                        return a.publicationKey < b.publicationKey;
                      }
                      const auto vA = a.claimVersion.str();
                      const auto vB = b.claimVersion.str();
                      if (vA != vB) {
                        return vA < vB;
                      }
                      return a.claimOpIndex < b.claimOpIndex;
                    });

  for (auto &cand : candidatesToArbitrate) {
    if (cand.state != OverlayClaimState::Shadowed) {
      // Retain errors like OutsideSnapshot, TargetAbsent, TargetNotFetched,
      // Unintelligible
      continue;
    }

    const SlotKey slot1{cand.from, cand.dimension, cand.direction};

    if (cand.isBreak()) { // Explicit break to == noCell
      if (occupiedSlots_.contains(slot1)) {
        cand.state = OverlayClaimState::Shadowed;
      } else {
        cand.state = OverlayClaimState::Applied;
        occupiedSlots_.insert(slot1);
        // Materialise no break; the occupied-slot set already expresses its
        // result (§5.12 §5)
      }
    } else { // Positive claim
      const SlotKey slot2{cand.to, cand.dimension, -cand.direction};
      if (occupiedSlots_.contains(slot1)) {
        cand.state = OverlayClaimState::Shadowed;
      } else if (occupiedSlots_.contains(slot2)) {
        cand.state = OverlayClaimState::SlotConflict;
      } else {
        cand.state = OverlayClaimState::Applied;
        occupiedSlots_.insert(slot1);
        occupiedSlots_.insert(slot2);
        zigzag::expectWritten(
            arena_.link(cand.from, cand.dimension, cand.direction, cand.to));
      }
    }
  }

  // 7. Store candidates in side index for provenance inspection (§5.12 §4)
  for (const auto &cand : candidatesToArbitrate) {
    const SlotKey slot1{cand.from, cand.dimension, cand.direction};
    candidateMap_[slot1].push_back(cand);
    if (!cand.isBreak()) {
      const SlotKey slot2{cand.to, cand.dimension, -cand.direction};
      candidateMap_[slot2].push_back(cand);
    }
    allCandidates_.push_back(cand);
  }

  return *this;
}

OverlayComposition &&OverlayComposition::compose() && {
  compose();
  return std::move(*this);
}

std::span<const OverlayCandidate>
OverlayComposition::candidates(const zigzag::CellRef from,
                               const zigzag::DimRef dim,
                               const zigzag::DimVector dir) const noexcept {
  const SlotKey key{from, dim, dir};
  const auto it = candidateMap_.find(key);
  if (it == candidateMap_.end()) {
    return {};
  }
  return it->second;
}

std::optional<zigzag::CellRef>
OverlayComposition::effectiveLink(const zigzag::CellRef from,
                                  const zigzag::DimRef dim,
                                  const zigzag::DimVector dir) const noexcept {
  const auto res = arena_.linked(from, dim, dir);
  return zigzag::present(res);
}

std::vector<GlobalLink>
OverlayComposition::classicLinksTouching(const GlobalSpan &span) const {
  std::vector<GlobalLink> matching;
  for (const auto &link : classicLinks_) {
    if (link.touches(span)) {
      matching.push_back(link);
    }
  }
  return matching;
}

} // namespace xanadu
