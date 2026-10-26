/**
 * @file multi_store.cpp
 * @brief Multi-store connection topology and coordinate manager for Xanadu.
 */
#include "common/xanadu/multi_store.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>

#include <gleditor/logging.hpp>
#include <gleditor/ranges.hpp>

#include "common/xanadu/store_loader.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu {
using zigzag::DimVector;

MultiStoreCoordinator::MultiStoreCoordinator() {
  ownedArena_ = std::make_unique<zigzag::ArenaManifold>();
  ownedCore_  = std::make_unique<zigzag::vortex::VortexCore>(*ownedArena_);
  // core_ aliases the ownedCore_ unique_ptr just constructed above it, so it
  // depends on that body statement rather than only on constructor
  // parameters -- moving it into the member initializer list, as
  // clang-tidy's own -fix does, captures ownedCore_ while it is still the
  // nullptr the initializer list default-constructed it to. See the
  // cppcoreguidelines-prefer-member-initializer note in .clang-tidy.
  // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
  core_ = ownedCore_.get();
  initDimensions();
}

MultiStoreCoordinator::MultiStoreCoordinator(zigzag::vortex::VortexCore &core)
    : core_(&core) {
  initDimensions();
}

void MultiStoreCoordinator::initDimensions() {
  coordinatorHome_ = core_->home();
  dimStores_       = core_->dims().stores;
  dimClone_        = core_->dims().clone;
  dimName_         = core_->dims().name;
  dimRole_         = core_->dims().role;
}

std::optional<CellRef>
MultiStoreCoordinator::derefCloneMaster(CellRef cell) const noexcept {
  if (!core_->arena().contains(cell)) return std::nullopt;
  return core_->arena().cloneMaster(cell, dimClone_).value_or(cell);
}

CellRef MultiStoreCoordinator::linkNewStoreOnRank(CellRef homeCell,
                                                  std::string_view label,
                                                  std::string_view role) {
  auto &arena = core_->arena();

  // 1. Mints representative cell for this store on the d.stores rank
  CellRef storeCell = arena.makeCell();

  // 2. Append storeCell posward along d.stores off coordinatorHome_
  if (!storesTail_) {
    const auto coordHome = coordinatorHome_.value();
    zigzag::expectWritten(
        arena.link(coordHome, dimStores_, DimVector::POS, storeCell));
  } else {
    zigzag::expectWritten(
        arena.link(*storesTail_, dimStores_, DimVector::POS, storeCell));
  }
  storesTail_ = storeCell;

  // 3. Link representative cell to homeCell via d.clone such that homeCell
  // is the clone master (homeCell is leftmost / negward).
  // storeCell links negward to homeCell; homeCell links posward to storeCell.
  zigzag::expectWritten(
      arena.link(storeCell, dimClone_, DimVector::NEG, homeCell));
  zigzag::expectWritten(
      arena.link(homeCell, dimClone_, DimVector::POS, storeCell));

  // 4. Metadata ranks hang directly off slice homeCell (the clone master)
  CellRef nameCell = arena.makeCell(label);
  zigzag::expectWritten(
      arena.link(homeCell, dimName_, DimVector::POS, nameCell));

  CellRef roleCell = arena.makeCell(role);
  zigzag::expectWritten(
      arena.link(homeCell, dimRole_, DimVector::POS, roleCell));

  return storeCell;
}

CellRef MultiStoreCoordinator::addSlice(std::string_view label,
                                        std::string_view role,
                                        CellRef homeCell) {
  if (!core_->arena().contains(homeCell)) {
    throw std::invalid_argument("homeCell must belong to the composite arena");
  }
  const CellRef storeCell = linkNewStoreOnRank(homeCell, label, role);

  StoreInfo info{
      .label     = std::string(label),
      .role      = std::string(role),
      .path      = "",
      .store     = nullptr,
      .manifold  = nullptr,
      .homeCell  = homeCell,
      .storeCell = storeCell,
      .spaceId   = 0,
  };
  stores_.push_back(std::move(info));
  return storeCell;
}

std::optional<CellRef>
MultiStoreCoordinator::importManifold(const zigzag::Manifold &source,
                                      zigzag::ArenaManifold &dest) {
  std::unordered_map<CellRef, CellRef> sourceToDest;
  sourceToDest.reserve(source.cellCount());

  // Pass 1: Mint corresponding cells and preserve scalar bits / content
  for (const auto &cell : source.cells()) {
    CellRef srcRef       = cell.birthOp;
    const CellRef dstRef = dest.makeCell();

    const auto kind = static_cast<xanadu::ValueKind>(cell.valueKind);
    if (kind != xanadu::ValueKind::None) {
      zigzag::expectWritten(dest.setValueBits(dstRef, kind, cell.valueBits));
    } else {
      const auto spans = source.contentOf(srcRef);
      if (!spans.empty()) {
        zigzag::expectWritten(dest.setContent(dstRef, spans));
      }
    }
    sourceToDest[srcRef] = dstRef;
  }

  // Pass 2: Re-link all dimensional neighbours in the destination arena
  for (const auto &cell : source.cells()) {
    CellRef srcRef = cell.birthOp;
    CellRef dstRef = sourceToDest[srcRef];

    for (const auto &dimLink : source.dimensionsOf(srcRef)) {
      auto itDim = sourceToDest.find(dimLink.dim);
      if (itDim == sourceToDest.end()) {
        continue;
      }
      DimRef dstDim = itDim->second;

      if (const auto pos = dimLink.neighbor(DimVector::POS)) {
        auto itPos = sourceToDest.find(*pos);
        if (itPos != sourceToDest.end()) {
          zigzag::expectWritten(
              dest.link(dstRef, dstDim, DimVector::POS, itPos->second));
        }
      }
      if (const auto neg = dimLink.neighbor(DimVector::NEG)) {
        auto itNeg = sourceToDest.find(*neg);
        if (itNeg != sourceToDest.end()) {
          zigzag::expectWritten(
              dest.link(dstRef, dstDim, DimVector::NEG, itNeg->second));
        }
      }
    }
  }

  if (const auto home = source.home()) {
    auto itHome = sourceToDest.find(*home);
    if (itHome != sourceToDest.end()) {
      return itHome->second;
    }
  }
  if (sourceToDest.empty()) return std::nullopt;
  return sourceToDest.begin()->second;
}

CellRef MultiStoreCoordinator::addStore(
    std::string_view label, std::string_view role,
    const std::shared_ptr<xanadu::Store> &store,
    const std::optional<xanadu::MicroversionId> version) {
  if (!store) {
    throw std::invalid_argument("store must not be null");
  }
  xanadu::MicroversionId v = version.value_or(store->primaryCurrentVersion());
  if (!version) {
    v = store->primaryCurrentVersion();
    // The current version is a document's designation and lags edits made
    // to a slice in the same store; the latest state descending from it is
    // what the reader last saw, and what a query should read.
    if (v.isZero() || v.isAncestorOf(store->latest())) {
      v = store->latest();
    }
  }
  auto foldedManifold =
      std::make_shared<zigzag::Manifold>(store->rebuildManifold(v));
  // latest() names the greatest branch, and a branch forked for a document
  // before the slice existed outranks every operation that built it: folded
  // there, a store with a perfectly good home answers `##` with nothing. A
  // version named by the caller is taken as asked; a guessed one falls back
  // to a structure head, preferring one that descends from the guess.
  if (!version && !foldedManifold->home()) {
    const auto heads = store->structureHeads();
    const auto descends =
        std::ranges::find_if(heads, [&v](const xanadu::MicroversionId &head) {
          return head == v || v.isAncestorOf(head);
        });
    const auto head =
        descends != heads.end() ? *descends : store->structureHead();
    if (head) {
      GLEDITOR_LOG_DEBUG("vql.stores",
                         "{}: no home at {}, folding at structure head {}",
                         label, v.str(), head->str());
      v = *head;
      foldedManifold =
          std::make_shared<zigzag::Manifold>(store->rebuildManifold(v));
    }
  }

  zigzag::Space space{
      .manifold = foldedManifold.get(),
      .store    = store.get(),
      .reader   = store.get(),
      .label    = std::string(label),
  };
  const auto spaceId = core_->arena().attach(std::move(space));

  const CellRef importedHome = [&] {
    if (const auto home = foldedManifold->home()) {
      return core_->arena().proxyFor(spaceId, *home);
    }
    // Text-only stores still need a representative in the composite view.
    return core_->arena().makeCell(label);
  }();
  CellRef storeCell = linkNewStoreOnRank(importedHome, label, role);

  StoreInfo info{
      .label     = std::string(label),
      .role      = std::string(role),
      .path      = "",
      .store     = store,
      .manifold  = foldedManifold,
      .homeCell  = importedHome,
      .storeCell = storeCell,
      .spaceId   = spaceId,
  };
  stores_.push_back(std::move(info));
  return storeCell;
}

CellRef MultiStoreCoordinator::loadAndAddStore(
    std::string_view label, std::string_view role, const std::string &path,
    const std::shared_ptr<xanadu::UserPermascroll> &userPermascroll) {
  auto loadedStore                        = loadStore(path, userPermascroll);
  std::shared_ptr<xanadu::Store> storePtr = std::move(loadedStore);
  CellRef storeCell                       = addStore(label, role, storePtr);
  stores_.back().path                     = path;
  return storeCell;
}

std::optional<CellRef> MultiStoreCoordinator::homeAnchor() const noexcept {
  if (stores_.size() == 1 && stores_.front().homeCell.has_value()) {
    return stores_.front().homeCell;
  }
  return coordinatorHome_;
}

std::optional<CellRef>
MultiStoreCoordinator::resolveNamedStore(std::string_view name) const {
  if (!coordinatorHome_) {
    return std::nullopt;
  }
  // Walks ##/d.stores>[d.name = "NAME"] structurally along the d.stores rank
  const auto &arena = core_->arena();
  const auto named  = [&](const CellRef master) {
    return zigzag::step(arena, master, dimName_)
        .transform(
            [&](const CellRef label) { return arena.textOf(label) == name; })
        .value_or(false);
  };
  const auto structural =
      zigzag::firstOf(zigzag::rankAfter(arena, *coordinatorHome_, dimStores_) |
                      std::views::transform([this](const CellRef entry) {
                        return derefCloneMaster(entry).value_or(entry);
                      }) |
                      std::views::filter(named));
  if (structural) {
    return structural;
  }
  // Fallback: the registration list.
  for (const auto &info : stores_) {
    if (info.label == name && info.homeCell) {
      return info.homeCell;
    }
  }
  return std::nullopt;
}

gleditor::cpp26::optional<const StoreInfo &>
MultiStoreCoordinator::findStore(std::string_view label) const noexcept {
  return gleditor::findRef(
      stores_, [label](const StoreInfo &info) { return info.label == label; });
}

gleditor::cpp26::optional<const StoreInfo &>
MultiStoreCoordinator::primaryStore() const noexcept {
  return gleditor::findRef(
             stores_,
             [](const StoreInfo &info) { return info.role == "primary"; })
      .or_else([this]() -> gleditor::cpp26::optional<const StoreInfo &> {
        if (stores_.empty()) {
          return gleditor::cpp26::nullopt;
        }
        return stores_.front();
      });
}

DimRef MultiStoreCoordinator::resolveDimension(std::string_view name) {
  const auto &sysDims = core_->dims();
  if (name == "d.stores") return sysDims.stores;
  if (name == "d.name") return sysDims.name;
  if (name == "d.role") return sysDims.role;
  if (name == "d.clone") return sysDims.clone;
  if (name == "d.dims") return sysDims.dims;
  if (name == "d.grab") return sysDims.grab;
  if (name == "d.step") return sysDims.step;
  if (name == "d.spin") return sysDims.spin;
  if (name == "d.stack") return sysDims.stack;
  if (name == "d.contract") return sysDims.contract;
  if (name == "d.cursors") return sysDims.cursors;
  if (name == "d.cache") return sysDims.cache;
  if (name == "d.vars") return sysDims.vars;
  if (name == "d.values") return sysDims.values;
  if (name == "d.pinning-cursors") return sysDims.pinningCursors;
  if (name == "d.stdlib") return sysDims.stdlib;
  if (name == "d.clause") return sysDims.clause;

  // Canonical generic lens dimensions (design/anything-to-zigzag-lens.md
  // §4.3.1) -- d.contains/d.seq/d.ref/d.mime -- are not privileged ranks in
  // core_->dims(); d.clone is the only structural generic that is (handled
  // above). They resolve through the mint-on-demand fallthrough below, which is
  // what projection and a query over it both call, so both name the same rank.
  return core_->findOrMintDimension(name);
}

} // namespace xanadu
