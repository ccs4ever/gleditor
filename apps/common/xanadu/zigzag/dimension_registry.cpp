/**
 * @file dimension_registry.cpp
 * @brief Unified dimension registry implementation for ZigZag space.
 */
#include "common/xanadu/zigzag/dimension_registry.hpp"

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace zigzag {

DimensionRegistry &DimensionRegistry::instance() noexcept {
  static DimensionRegistry registry;
  return registry;
}

InternedDimName DimensionRegistry::intern(const std::string_view name) {
  if (name.empty()) {
    return InternedDimName{0, ""};
  }
  std::scoped_lock lock(mutex_);
  if (const auto it = nameToId_.find(name); it != nameToId_.end()) {
    const auto id = it->second;
    return InternedDimName{id, internPool_[id - 1]};
  }

  internPool_.emplace_back(name);
  const auto id = static_cast<std::uint32_t>(internPool_.size());
  std::string_view pooled(internPool_.back());
  nameToId_[pooled] = id;
  return InternedDimName{id, pooled};
}

InternedDimName
DimensionRegistry::findInterned(const std::string_view name) const {
  if (name.empty()) {
    return InternedDimName{0, ""};
  }
  std::scoped_lock lock(mutex_);
  if (const auto it = nameToId_.find(name); it != nameToId_.end()) {
    const auto id = it->second;
    return InternedDimName{id, internPool_[id - 1]};
  }
  return InternedDimName{0, ""};
}

std::optional<DimRef>
DimensionRegistry::get(const xanadu::Store &store,
                       const InternedDimName dimName) const {
  if (dimName.empty()) {
    return std::nullopt;
  }
  std::scoped_lock lock(mutex_);
  const auto storeIt = storeDims_.find(&store);
  if (storeIt == storeDims_.end()) {
    return std::nullopt;
  }
  const auto dimIt = storeIt->second.find(dimName.id());
  if (dimIt == storeIt->second.end()) {
    return std::nullopt;
  }
  return dimIt->second;
}

std::optional<DimRef>
DimensionRegistry::get(const xanadu::Store &store,
                       const std::string_view name) const {
  return get(store, findInterned(name));
}

std::optional<DimRef>
DimensionRegistry::get(const Manifold &manifold,
                       const InternedDimName dimName) const {
  const auto *const store = manifold.store();
  if (dimName.empty() || nullptr == store) {
    return std::nullopt;
  }
  // The cache first; a cached dim this version does not hold falls back to a
  // scan of the manifold's own d.dims rank.
  return get(*store, dimName)
      .and_then([&](const DimRef cell) {
        return manifold.contains(cell) ? std::optional{cell} : std::nullopt;
      })
      .or_else([&] { return manifold.dimensionNamed(dimName.name(), *store); });
}

std::optional<DimRef>
DimensionRegistry::get(const Manifold &manifold,
                       const std::string_view name) const {
  const auto dimName = findInterned(name);
  if (!dimName.empty()) {
    return get(manifold, dimName);
  }
  if (const auto *const store = manifold.store(); nullptr != store) {
    return manifold.dimensionNamed(name, *store);
  }
  return std::nullopt;
}

void DimensionRegistry::registerDim(const xanadu::Store &store,
                                    const InternedDimName dimName,
                                    const DimRef cell) {
  if (dimName.empty() || noCell == cell) {
    return;
  }
  std::scoped_lock lock(mutex_);
  storeDims_[&store][dimName.id()] = cell;
}

void DimensionRegistry::registerDim(const xanadu::Store &store,
                                    const std::string_view name,
                                    const DimRef cell) {
  registerDim(store, intern(name), cell);
}

void DimensionRegistry::unregisterStore(
    const xanadu::Store *const store) noexcept {
  if (nullptr == store) {
    return;
  }
  std::scoped_lock lock(mutex_);
  storeDims_.erase(store);
}

void DimensionRegistry::clear() noexcept {
  std::scoped_lock lock(mutex_);
  storeDims_.clear();
  nameToId_.clear();
  internPool_.clear();
}

DimensionResult DimensionRegistry::getOrCreate(Manifold &manifold,
                                               const InternedDimName dimName) {
  if (dimName.empty()) {
    return std::unexpected{DimensionError::EmptyName};
  }
  auto *const store = manifold.store();
  if (nullptr != store) {
    return getOrCreate(*store, manifold, dimName);
  }
  if (const auto existing = get(manifold, dimName)) {
    return *existing;
  }
  // Minting a dimension is recording an operation, and a manifold with no
  // store has nowhere to record one.
  return std::unexpected{DimensionError::NoStore};
}

std::optional<DimRef> DimensionRegistry::held(const xanadu::Store &store,
                                              const Manifold &manifold,
                                              const InternedDimName dimName) {
  const auto cached = get(store, dimName).and_then([&](const DimRef cell) {
    return manifold.contains(cell) ? std::optional{cell} : std::nullopt;
  });
  if (cached) {
    return cached;
  }
  return manifold.dimensionNamed(dimName.name(), store)
      .transform([&](const DimRef found) {
        registerDim(store, dimName, found);
        return found;
      });
}

DimensionResult DimensionRegistry::getOrCreate(Manifold &manifold,
                                               const std::string_view name) {
  return getOrCreate(manifold, intern(name));
}

DimensionResult DimensionRegistry::getOrCreate(xanadu::Store &store,
                                               Manifold &manifold,
                                               const InternedDimName dimName) {
  if (dimName.empty()) {
    return std::unexpected{DimensionError::EmptyName};
  }
  if (const auto known = held(store, manifold, dimName)) {
    return *known;
  }

  if (noCell == store.homeCell()) {
    auto rootParent = store.latest();
    store.sliceGenesis(rootParent);
  }
  auto head = store.primaryCurrentVersion();
  if (head.isZero()) {
    head = store.latest();
  }
  return getOrCreate(store, head, manifold, dimName);
}

DimensionResult DimensionRegistry::getOrCreate(xanadu::Store &store,
                                               Manifold &manifold,
                                               const std::string_view name) {
  return getOrCreate(store, manifold, intern(name));
}

DimensionResult DimensionRegistry::getOrCreate(xanadu::Store &store,
                                               xanadu::MicroversionId &head,
                                               Manifold &manifold,
                                               const InternedDimName dimName) {
  if (dimName.empty()) {
    return std::unexpected{DimensionError::EmptyName};
  }
  if (const auto known = held(store, manifold, dimName)) {
    return *known;
  }

  if (noCell == store.homeCell()) {
    head = store.sliceGenesis(head);
  }
  const auto minted = store.makeDimension(head, dimName.name(), &manifold);
  head              = minted.version;
  store.repointCurrentVersion(head);
  auto newManifold = store.rebuildManifold(head);
  newManifold.setStore(&store);
  for (const auto &c : manifold.cells()) {
    if (c.formatFlags != 0) {
      newManifold.setFormatFlags(c.birthOp, c.formatFlags);
    }
  }
  manifold = std::move(newManifold);
  registerDim(store, dimName, minted.dim);
  return minted.dim;
}

DimensionResult DimensionRegistry::getOrCreate(xanadu::Store &store,
                                               xanadu::MicroversionId &head,
                                               Manifold &manifold,
                                               const std::string_view name) {
  return getOrCreate(store, head, manifold, intern(name));
}

} // namespace zigzag
