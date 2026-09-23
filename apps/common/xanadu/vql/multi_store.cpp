/**
 * @file multi_store.cpp
 * @brief Multi-store connection topology and coordinate manager for VQL.
 */
#include "common/xanadu/vql/multi_store.hpp"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>

#include "common/xanadu/zigzag/cell_views.hpp"

namespace xanadu::vql {
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

CellRef MultiStoreCoordinator::derefCloneMaster(CellRef cell) const noexcept {
  if (cell == noCell) {
    return noCell;
  }
  return core_->arena().cloneMaster(cell, dimClone_).value_or(cell);
}

CellRef MultiStoreCoordinator::linkNewStoreOnRank(CellRef homeCell,
                                                  std::string_view label,
                                                  std::string_view role) {
  auto &arena = core_->arena();

  // 1. Mints representative cell for this store on the d.stores rank
  CellRef storeCell = arena.makeCell();

  // 2. Append storeCell posward along d.stores off coordinatorHome_
  if (storesTail_ == noCell) {
    zigzag::expectWritten(
        arena.link(coordinatorHome_, dimStores_, DimVector::POS, storeCell));
  } else {
    zigzag::expectWritten(
        arena.link(storesTail_, dimStores_, DimVector::POS, storeCell));
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
  if (homeCell == noCell) {
    throw std::invalid_argument("homeCell must not be noCell");
  }
  CellRef storeCell = linkNewStoreOnRank(homeCell, label, role);

  StoreInfo info{
      .label     = std::string(label),
      .role      = std::string(role),
      .path      = "",
      .store     = nullptr,
      .homeCell  = homeCell,
      .storeCell = storeCell,
  };
  stores_.push_back(std::move(info));
  return storeCell;
}

CellRef MultiStoreCoordinator::importManifold(const zigzag::Manifold &source,
                                              zigzag::ArenaManifold &dest) {
  std::unordered_map<CellRef, CellRef> sourceToDest;
  sourceToDest.reserve(source.cellCount());

  // Pass 1: Mint corresponding cells and preserve scalar bits / content
  for (const auto &cell : source.cells()) {
    CellRef srcRef = cell.birthOp;
    CellRef dstRef = noCell;

    const auto kind = static_cast<xanadu::ValueKind>(cell.valueKind);
    if (kind != xanadu::ValueKind::None) {
      dstRef = dest.makeCell();
      zigzag::expectWritten(dest.setValueBits(dstRef, kind, cell.valueBits));
    } else {
      const auto spans = source.contentOf(srcRef);
      if (!spans.empty()) {
        dstRef = dest.makeCell();
        zigzag::expectWritten(dest.setContent(dstRef, spans));
      } else {
        dstRef = dest.makeCell();
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

      if (dimLink.pos != noCell) {
        auto itPos = sourceToDest.find(dimLink.pos);
        if (itPos != sourceToDest.end()) {
          zigzag::expectWritten(
              dest.link(dstRef, dstDim, DimVector::POS, itPos->second));
        }
      }
      if (dimLink.neg != noCell) {
        auto itNeg = sourceToDest.find(dimLink.neg);
        if (itNeg != sourceToDest.end()) {
          zigzag::expectWritten(
              dest.link(dstRef, dstDim, DimVector::NEG, itNeg->second));
        }
      }
    }
  }

  if (source.home() != noCell) {
    auto itHome = sourceToDest.find(source.home());
    if (itHome != sourceToDest.end()) {
      return itHome->second;
    }
  }
  return sourceToDest.empty() ? noCell : sourceToDest.begin()->second;
}

CellRef
MultiStoreCoordinator::addStore(std::string_view label, std::string_view role,
                                const std::shared_ptr<xanadu::Store> &store,
                                const xanadu::MicroversionId &version) {
  if (!store) {
    throw std::invalid_argument("store must not be null");
  }
  xanadu::MicroversionId v = version;
  if (v.isZero()) {
    v = store->primaryCurrentVersion();
    if (v.isZero()) {
      v = store->latest();
    }
  }
  zigzag::Manifold m = store->rebuildManifold(v);

  CellRef importedHome = importManifold(m, core_->arena());
  if (importedHome == noCell) {
    // For xanadocs without zigzag structure ops or empty slices, mint a home
    // cell
    importedHome = core_->arena().makeCell(label);
  }
  CellRef storeCell = linkNewStoreOnRank(importedHome, label, role);

  StoreInfo info{
      .label     = std::string(label),
      .role      = std::string(role),
      .path      = "",
      .store     = store,
      .homeCell  = importedHome,
      .storeCell = storeCell,
  };
  stores_.push_back(std::move(info));
  return storeCell;
}

CellRef MultiStoreCoordinator::loadAndAddStore(
    std::string_view label, std::string_view role, const std::string &path,
    const std::shared_ptr<xanadu::UserPermascroll> &userPermascroll) {
  auto loadedStore = std::make_shared<xanadu::Store>(userPermascroll);
  loadedStore->load(path);
  CellRef storeCell   = addStore(label, role, loadedStore);
  stores_.back().path = path;
  return storeCell;
}

CellRef MultiStoreCoordinator::homeAnchor() const noexcept {
  if (stores_.size() == 1) {
    return stores_.front().homeCell;
  }
  return coordinatorHome_;
}

std::optional<CellRef>
MultiStoreCoordinator::resolveNamedStore(std::string_view name) const {
  // Walks ##/d.stores>[d.name = "NAME"] structurally along the d.stores rank
  const auto &arena = core_->arena();
  const auto named  = [&](const CellRef master) {
    return zigzag::step(arena, master, dimName_)
        .transform(
            [&](const CellRef label) { return arena.textOf(label) == name; })
        .value_or(false);
  };
  const auto structural =
      zigzag::firstOf(zigzag::rankAfter(arena, coordinatorHome_, dimStores_) |
                      std::views::transform([this](const CellRef entry) {
                        return derefCloneMaster(entry);
                      }) |
                      std::views::filter(named));
  if (structural) {
    return structural;
  }
  // Fallback: the registration list.
  return zigzag::firstOf(stores_ |
                         std::views::filter([&](const StoreInfo &info) {
                           return info.label == name;
                         }) |
                         std::views::transform(&StoreInfo::homeCell));
}

const StoreInfo *
MultiStoreCoordinator::findStore(std::string_view label) const noexcept {
  for (const auto &info : stores_) {
    if (info.label == label) {
      return &info;
    }
  }
  return nullptr;
}

const StoreInfo *MultiStoreCoordinator::primaryStore() const noexcept {
  for (const auto &info : stores_) {
    if (info.role == "primary") {
      return &info;
    }
  }
  if (!stores_.empty()) {
    return &stores_.front();
  }
  return nullptr;
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

  return core_->findOrMintDimension(name);
}

} // namespace xanadu::vql
