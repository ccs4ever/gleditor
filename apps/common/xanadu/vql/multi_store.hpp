/**
 * @file multi_store.hpp
 * @brief Multi-store connection topology and coordinate manager for VQL.
 *
 * Implements Stage 0 of the VQL architecture:
 * - Standard connection points along system dimension `d.stores` off home.
 * - Slices and xanadocs joined to representative cells via `d.clone` ranks
 *   where the slice's home cell is the authoritative clone master.
 * - Metadata ranks `d.name` (label) and `d.role` hanging directly off slice
 *   home.
 * - Universal `>` clone master dereference (sugar for /d.clone::head).
 * - Direct resolution of `##NAME` shorthand (##/d.stores>[d.name = "NAME"]).
 */
#ifndef COMMON_XANADU_VQL_MULTI_STORE_HPP
#define COMMON_XANADU_VQL_MULTI_STORE_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/vortex_core.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vql {

using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::noCell;

/**
 * @struct StoreInfo
 * @brief Metadata and coordinates for a loaded slice or xanadoc store.
 */
struct StoreInfo {
  std::string label;
  std::string role;
  std::string path;
  std::shared_ptr<xanadu::Store> store{nullptr};
  CellRef homeCell{
      noCell}; ///< Slice's home cell (clone master carrying metadata)
  CellRef storeCell{
      noCell}; ///< Representative cell on coordinator's d.stores rank
};

/**
 * @class MultiStoreCoordinator
 * @brief Manages composite manifold topologies across multiple stores.
 */
class MultiStoreCoordinator {
public:
  MultiStoreCoordinator();
  explicit MultiStoreCoordinator(zigzag::vortex::VortexCore &core);
  ~MultiStoreCoordinator() = default;

  MultiStoreCoordinator(const MultiStoreCoordinator &)                = delete;
  MultiStoreCoordinator &operator=(const MultiStoreCoordinator &)     = delete;
  MultiStoreCoordinator(MultiStoreCoordinator &&) noexcept            = default;
  MultiStoreCoordinator &operator=(MultiStoreCoordinator &&) noexcept = default;

  // -- Store Registration ----------------------------------------------------

  /// Registers an existing slice in the composite arena with its home cell.
  CellRef addSlice(std::string_view label, std::string_view role,
                   CellRef homeCell);

  /// Registers an on-disk or in-memory xanadu::Store.
  /// Folds the store's manifold and imports it into the composite arena.
  CellRef addStore(std::string_view label, std::string_view role,
                   std::shared_ptr<xanadu::Store> store,
                   const xanadu::MicroversionId &version = {});

  /// Loads a store from directory path and registers it.
  CellRef loadAndAddStore(
      std::string_view label, std::string_view role, const std::string &path,
      std::shared_ptr<xanadu::UserPermascroll> userPermascroll = nullptr);

  // -- Navigation & Resolution ------------------------------------------------

  /// Resolves default home anchor '##'.
  /// In single-store mode, returns that store's home cell; in multi-store mode,
  /// returns the coordinator origin home cell.
  [[nodiscard]] CellRef homeAnchor() const noexcept;

  /// The universal '>' clone master dereference (cell/d.clone::head).
  /// Walks negward along d.clone until reaching the clone master.
  [[nodiscard]] CellRef derefCloneMaster(CellRef cell) const noexcept;

  /// Resolves the '##NAME' shorthand: ##/d.stores>[d.name = "NAME"].
  /// Returns the named slice's home cell (the clone master), or noCell if not
  /// found.
  [[nodiscard]] CellRef resolveNamedStore(std::string_view name) const;

  /// Multi-store coordinator genesis origin cell.
  [[nodiscard]] CellRef coordinatorHome() const noexcept {
    return coordinatorHome_;
  }

  // -- Introspection ---------------------------------------------------------

  [[nodiscard]] std::size_t storeCount() const noexcept {
    return stores_.size();
  }

  [[nodiscard]] std::span<const StoreInfo> stores() const noexcept {
    return stores_;
  }

  [[nodiscard]] const StoreInfo *
  findStore(std::string_view label) const noexcept;

  [[nodiscard]] const StoreInfo *primaryStore() const noexcept;

  // -- Runtime Accessors -----------------------------------------------------

  [[nodiscard]] zigzag::ArenaManifold &arena() noexcept {
    return core_->arena();
  }
  [[nodiscard]] const zigzag::ArenaManifold &arena() const noexcept {
    return core_->arena();
  }

  [[nodiscard]] zigzag::vortex::VortexCore &core() noexcept { return *core_; }
  [[nodiscard]] const zigzag::vortex::VortexCore &core() const noexcept {
    return *core_;
  }

  [[nodiscard]] DimRef dimStores() const noexcept { return dimStores_; }
  [[nodiscard]] DimRef dimClone() const noexcept { return dimClone_; }
  [[nodiscard]] DimRef dimName() const noexcept { return dimName_; }
  [[nodiscard]] DimRef dimRole() const noexcept { return dimRole_; }

  /// Resolves a dimension by name from system dimensions or arena d.dims rank.
  /// Mints the dimension if not found.
  DimRef resolveDimension(std::string_view name);

  // -- Manifold Import Helper ------------------------------------------------

  /// Imports an entire zigzag::Manifold into an ArenaManifold, preserving
  /// all cells, dimensions, links, text spans, and canonical scalar bits.
  static CellRef importManifold(const zigzag::Manifold &source,
                                zigzag::ArenaManifold &dest);

private:
  void initDimensions();
  CellRef linkNewStoreOnRank(CellRef homeCell, std::string_view label,
                             std::string_view role);

  std::unique_ptr<zigzag::ArenaManifold> ownedArena_{nullptr};
  std::unique_ptr<zigzag::vortex::VortexCore> ownedCore_{nullptr};
  zigzag::vortex::VortexCore *core_{nullptr};

  CellRef coordinatorHome_{noCell};
  CellRef storesTail_{noCell};

  DimRef dimStores_{noCell};
  DimRef dimClone_{noCell};
  DimRef dimName_{noCell};
  DimRef dimRole_{noCell};

  std::vector<StoreInfo> stores_;
};

} // namespace xanadu::vql

namespace zigzag::vql {
using namespace ::xanadu::vql;
} // namespace zigzag::vql

#endif // COMMON_XANADU_VQL_MULTI_STORE_HPP
