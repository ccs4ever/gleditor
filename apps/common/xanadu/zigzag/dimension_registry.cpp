/**
 * @file dimension_registry.cpp
 * @brief Unified dimension registry implementation for ZigZag space.
 */
#include "common/xanadu/zigzag/dimension_registry.hpp"

#include <stdexcept>

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

DimRef DimensionRegistry::get(const xanadu::Store &store,
                              const InternedDimName dimName) const {
  if (dimName.empty()) {
    return noCell;
  }
  std::scoped_lock lock(mutex_);
  const auto storeIt = storeDims_.find(&store);
  if (storeIt == storeDims_.end()) {
    return noCell;
  }
  const auto dimIt = storeIt->second.find(dimName.id());
  if (dimIt == storeIt->second.end()) {
    return noCell;
  }
  return dimIt->second;
}

DimRef DimensionRegistry::get(const xanadu::Store &store,
                              const std::string_view name) const {
  const auto dimName = findInterned(name);
  if (dimName.empty()) {
    return noCell;
  }
  return get(store, dimName);
}

DimRef DimensionRegistry::get(const Manifold &manifold,
                              const InternedDimName dimName) const {
  if (dimName.empty()) {
    return noCell;
  }
  if (const auto *const store = manifold.store(); nullptr != store) {
    const auto cell = get(*store, dimName);
    if (noCell != cell && manifold.contains(cell)) {
      return cell;
    }
    // Fall back to manifold scan if cached dim isn't contained in this version
    return manifold.dimensionNamed(dimName.name(), *store);
  }
  return noCell;
}

DimRef DimensionRegistry::get(const Manifold &manifold,
                              const std::string_view name) const {
  const auto dimName = findInterned(name);
  if (!dimName.empty()) {
    return get(manifold, dimName);
  }
  if (const auto *const store = manifold.store(); nullptr != store) {
    return manifold.dimensionNamed(name, *store);
  }
  return noCell;
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

DimRef DimensionRegistry::getOrCreate(Manifold &manifold,
                                      const InternedDimName dimName) {
  if (dimName.empty()) {
    return noCell;
  }
  auto *const store = manifold.store();
  if (nullptr != store) {
    return getOrCreate(*store, manifold, dimName);
  }
  const auto existing = get(manifold, dimName);
  if (noCell != existing) {
    return existing;
  }
  throw std::invalid_argument("Cannot create dimension '" +
                              std::string(dimName.name()) +
                              "' on Manifold without an associated Store");
}

DimRef DimensionRegistry::getOrCreate(Manifold &manifold,
                                      const std::string_view name) {
  return getOrCreate(manifold, intern(name));
}

DimRef DimensionRegistry::getOrCreate(xanadu::Store &store, Manifold &manifold,
                                      const InternedDimName dimName) {
  if (dimName.empty()) {
    return noCell;
  }
  const auto cached = get(store, dimName);
  if (noCell != cached && manifold.contains(cached)) {
    return cached;
  }
  const auto existing = manifold.dimensionNamed(dimName.name(), store);
  if (noCell != existing) {
    registerDim(store, dimName, existing);
    return existing;
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

DimRef DimensionRegistry::getOrCreate(xanadu::Store &store, Manifold &manifold,
                                      const std::string_view name) {
  return getOrCreate(store, manifold, intern(name));
}

DimRef DimensionRegistry::getOrCreate(xanadu::Store &store,
                                      xanadu::MicroversionId &head,
                                      Manifold &manifold,
                                      const InternedDimName dimName) {
  if (dimName.empty()) {
    return noCell;
  }
  const auto cached = get(store, dimName);
  if (noCell != cached && manifold.contains(cached)) {
    return cached;
  }
  const auto existing = manifold.dimensionNamed(dimName.name(), store);
  if (noCell != existing) {
    registerDim(store, dimName, existing);
    return existing;
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

DimRef DimensionRegistry::getOrCreate(xanadu::Store &store,
                                      xanadu::MicroversionId &head,
                                      Manifold &manifold,
                                      const std::string_view name) {
  return getOrCreate(store, head, manifold, intern(name));
}

} // namespace zigzag
