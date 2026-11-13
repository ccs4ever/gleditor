#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "common/xanadu/foreign_op_handle.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/publication.hpp"
#include "common/xanadu/scalar.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_prefix_wire.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include <gleditor/logging.hpp>

namespace zigzag {

ArenaManifold::ArenaManifold(const Manifold *base, const xanadu::Store *store)
    : base_(base), store_(store) {
  if (store_ != nullptr) {
    projectProvenance(*store_);
  }
}

std::optional<CellRef> ArenaManifold::home() const noexcept {
  return base_ ? base_->home() : std::nullopt;
}

std::vector<DimRef> ArenaManifold::dimensions() const {
  std::vector<DimRef> dims;
  if (base_ != nullptr) {
    const auto bDims = base_->dimensions();
    dims.assign(bDims.begin(), bDims.end());
  }
  for (const auto &[name, dim] : arenaDims_) {
    if (std::ranges::find(dims, dim) == dims.end()) {
      dims.push_back(dim);
    }
  }
  return dims;
}

std::optional<DimRef>
ArenaManifold::dimensionNamed(const std::string_view name,
                              const xanadu::SpanReader &reader) const {
  return dimensionNamed(name, &reader);
}

std::optional<DimRef>
ArenaManifold::dimensionNamed(const std::string_view name,
                              const xanadu::SpanReader *reader) const {
  if (!reader) reader = defaultReader_;
  if (base_ != nullptr) {
    std::optional<DimRef> baseDim;
    if (reader != nullptr) {
      baseDim = base_->dimensionNamed(name, *reader);
    } else if (store_ != nullptr) {
      baseDim = base_->dimensionNamed(name, *store_);
    } else if (base_->store() != nullptr) {
      baseDim = base_->dimensionNamed(name);
    }
    if (baseDim.has_value()) {
      return *baseDim;
    }
  }
  const auto it = arenaDims_.find(std::string(name));
  if (it != arenaDims_.end()) {
    return it->second;
  }
  return std::nullopt;
}

DimRef ArenaManifold::ensureDimension(const std::string_view name) {
  const auto existing = dimensionNamed(name);
  if (existing) {
    return *existing;
  }
  const auto it = arenaDims_.find(std::string(name));
  if (it != arenaDims_.end()) {
    return it->second;
  }
  const auto span                   = intern(name);
  const auto dim                    = makeCell(span);
  const auto key                    = std::string(name);
  const auto [insertedAt, inserted] = arenaDims_.emplace(key, dim);
  if (!inserted) {
    return insertedAt->second;
  }
  if (outstandingMarks_ > 0) {
    try {
      dimensionOrder_.push_back(DimensionEntry{.name = key, .dim = dim});
    } catch (...) {
      arenaDims_.erase(insertedAt);
      throw;
    }
  }

  const auto dimDims = dimensionNamed("d.dims");
  const auto home    = homeCell();
  if (dimDims && home) {
    CellRef tail = *home;
    while (true) {
      const auto nxt = linked(tail, *dimDims, DimVector::POS);
      if (!nxt) {
        break;
      }
      tail = *nxt;
    }
    link(tail, *dimDims, DimVector::POS, dim).value();
  }
  return dim;
}

ArenaManifold *ArenaManifold::projectProvenance(const xanadu::Store &store) {
  if (!store.provenance().has_value()) {
    return this;
  }
  const auto &signedProv = *store.provenance();
  if (signedProv.signature.empty() || signedProv.tsv.empty()) {
    return this;
  }
  const auto check = xanadu::verifyProvenance(signedProv);
  if (!check.signatureValid) {
    return this;
  }
  const auto prov = xanadu::parseProvenance(signedProv.tsv);
  if (!prov) {
    return this;
  }

  const auto home = homeCell();
  if (!home) {
    return this;
  }

  const DimRef dimAuthorship = ensureDimension("d.authorship");
  provenanceCells_.insert(dimAuthorship);
  authorshipRoot_ = makeCell(intern("AUTHORSHIP.tsv"));
  provenanceCells_.insert(*authorshipRoot_);
  link(*home, dimAuthorship, DimVector::POS, *authorshipRoot_).value();

  const DimRef dimSource = ensureDimension("d.source");
  provenanceCells_.insert(dimSource);
  CellRef bootstrapCell    = noCell;
  const auto &bootstrapKey = store.bootstrapPermascrollKey();
  if (bootstrapKey) {
    const auto &registry = store.scrollRegistry();
    for (const auto &rec : registry.scrolls) {
      if (rec.globalKey == *bootstrapKey && rec.cell != noCell) {
        bootstrapCell = rec.cell;
        break;
      }
    }
    if (noCell == bootstrapCell) {
      const auto dimScrolls = dimensionNamed("d.scrolls");
      if (dimScrolls) {
        CellRef cur = *home;
        while (true) {
          const auto next = linked(cur, *dimScrolls, DimVector::POS);
          if (!next) {
            break;
          }
          cur = *next;
          if (textOf(cur) == *bootstrapKey) {
            bootstrapCell = cur;
            break;
          }
        }
      }
    }
    if (noCell == bootstrapCell) {
      bootstrapCell = makeCell(intern(*bootstrapKey));
      provenanceCells_.insert(bootstrapCell);
    }
  }
  if (noCell != bootstrapCell) {
    link(*authorshipRoot_, dimSource, DimVector::POS, bootstrapCell).value();
  }

  auto appendRank = [this](std::string_view dimName, std::string_view value) {
    const DimRef dim = ensureDimension(dimName);
    provenanceCells_.insert(dim);
    const CellRef cell = makeCell(intern(value));
    provenanceCells_.insert(cell);
    CellRef tail = *authorshipRoot_;
    while (true) {
      const auto nxt = linked(tail, dim, DimVector::POS);
      if (!nxt) {
        break;
      }
      tail = *nxt;
    }
    link(tail, dim, DimVector::POS, cell).value();
    return cell;
  };

  std::istringstream lines{signedProv.tsv};
  std::string line;
  while (std::getline(lines, line)) {
    if (line.empty()) {
      continue;
    }
    const auto tab = line.find('\t');
    if (std::string::npos == tab || 0 == tab) {
      continue;
    }
    const auto key    = std::string_view{line}.substr(0, tab);
    const auto valRaw = std::string_view{line}.substr(tab + 1);

    std::string val;
    val.reserve(valRaw.size());
    for (std::size_t i = 0; i < valRaw.size(); ++i) {
      if ('\\' != valRaw[i]) {
        val.push_back(valRaw[i]);
        continue;
      }
      if (++i == valRaw.size()) {
        break;
      }
      switch (valRaw[i]) {
      case 't':
        val.push_back('\t');
        break;
      case 'n':
        val.push_back('\n');
        break;
      case 'r':
        val.push_back('\r');
        break;
      case '\\':
        val.push_back('\\');
        break;
      default:
        val.push_back(valRaw[i]);
        break;
      }
    }

    std::string dimName;
    if (key.starts_with("d.")) {
      dimName = std::string(key);
    } else {
      dimName = "d." + std::string(key);
    }
    appendRank(dimName, val);
  }

  if (!signedProv.signature.empty()) {
    appendRank("d.signature", signedProv.signature);
  }
  return this;
}

void expectWritten(const ArenaResult &result,
                   const std::source_location where) noexcept {
  if (!result) {
    GLEDITOR_LOG_WARN("zigzag.arena", "arena write refused ({}) at {}:{}",
                      toString(result.error()), where.file_name(),
                      where.line());
  }
}
std::optional<std::uint32_t>
ArenaManifold::denseOf(const CellRef ref) const noexcept {
  if (!isEphemeral(ref)) {
    // A base cell, which this arena holds only once it has been shadowed.
    // Until then denseOf() says "not mine" and every read falls through.
    const auto found = overlay_.find(ref);
    return found == overlay_.end()
               ? std::nullopt
               : std::optional<std::uint32_t>{found->second};
  }
  const auto dense = ref & ~ephemeralBit;
  return dense < slots_.size() ? std::optional<std::uint32_t>{dense}
                               : std::nullopt;
}

bool ArenaManifold::holdsOwn(const CellRef ref) const noexcept {
  return denseOf(ref).has_value();
}

std::uint32_t ArenaManifold::attach(Space space) {
  const auto id = static_cast<std::uint32_t>(spaces_.size() + 1);
  if (space.manifold == nullptr && space.ownedManifold != nullptr) {
    space.manifold = space.ownedManifold.get();
  }
  SpaceAdoption adoption;
  if (base_ == nullptr && space.manifold != nullptr) {
    base_         = space.manifold;
    adoption.base = true;
  }
  if (store_ == nullptr && space.store != nullptr) {
    store_         = space.store;
    adoption.store = true;
  }
  if (space.reader == nullptr && space.store != nullptr) {
    space.reader = space.store;
  }

  const auto dimStores    = ensureDimension("d.stores");
  const auto dimStoreRefs = ensureDimension("d.store-refs");

  if (space.storeCell && *space.storeCell == noCell) {
    throw std::invalid_argument("a space store cell cannot be noCell");
  }
  if (!space.storeCell) {
    const std::string label =
        space.label.empty() ? ("space_" + std::to_string(id)) : space.label;
    space.storeCell = makeCell(label);
  }

  auto root = homeCell();
  if (!root) {
    root = makeCell("home");
  }

  if (*space.storeCell != *root &&
      !linked(*space.storeCell, dimStores, DimVector::NEG) &&
      !linked(*space.storeCell, dimStores, DimVector::POS)) {
    if (!storesRankTail_) {
      CellRef cur = *root;
      while (true) {
        const auto nxt = linked(cur, dimStores, DimVector::POS);
        if (!nxt) {
          break;
        }
        cur = *nxt;
      }
      zigzag::expectWritten(
          link(cur, dimStores, DimVector::POS, *space.storeCell));
    } else {
      zigzag::expectWritten(
          link(*storesRankTail_, dimStores, DimVector::POS, *space.storeCell));
    }
  }
  storesRankTail_ = *space.storeCell;

  spaceStoreRefsTail_.push_back(*space.storeCell);
  spaceStoreRefsDimensions_.push_back(dimStoreRefs);
  spaceProxies_.emplace_back();
  spaceAdoptions_.push_back(adoption);
  spaces_.push_back(space);
  return id;
}

gleditor::cpp26::optional<const Space &>
ArenaManifold::spaceAt(const std::uint32_t id) const noexcept {
  if (id == 0 || id > spaces_.size()) {
    return gleditor::cpp26::nullopt;
  }
  return spaces_[id - 1];
}

gleditor::cpp26::optional<Space &>
ArenaManifold::spaceAt(const std::uint32_t id) noexcept {
  if (id == 0 || id > spaces_.size()) {
    return gleditor::cpp26::nullopt;
  }
  return spaces_[id - 1];
}

std::optional<DimRef>
ArenaManifold::storeRefsDimension(const std::uint32_t space) const noexcept {
  if (space == 0 || space > spaces_.size() ||
      space > spaceStoreRefsDimensions_.size()) {
    return std::nullopt;
  }
  const auto cachedDimension = spaceStoreRefsDimensions_[space - 1];
  if (!cachedDimension) {
    return std::nullopt;
  }
  const DimRef dimension = *cachedDimension;
  return contains(dimension) ? std::optional<DimRef>{dimension} : std::nullopt;
}

CellRef ArenaManifold::proxyFor(const std::uint32_t space,
                                const CellRef foreignIndex) {
  if (space == 0 || space > spaces_.size()) {
    throw std::out_of_range("invalid space id in proxyFor");
  }
  if (foreignIndex == noCell) {
    throw std::invalid_argument("a proxy needs a foreign cell reference");
  }
  auto &map = spaceProxies_[space - 1];
  if (const auto it = map.find(foreignIndex); it != map.end()) {
    return it->second;
  }

  const Mark savepoint = mark();
  try {
    const auto chargeLookupWork = [&](const std::size_t amount) {
      try {
        for (std::size_t i = 0; i < amount; ++i) {
          consumeWork();
        }
      } catch (const std::runtime_error &) {
        return false;
      }
      return true;
    };
    DimensionLookupBudget lookupBudget{dimensionLookupLimits_};
    const auto chargeProbe = [&](const Manifold &manifold,
                                 const CellRef owner) {
      const std::size_t amount = 1 + manifold.dimensionsOf(owner).size();
      if (amount > lookupBudget.workRemaining || !chargeLookupWork(amount)) {
        throw std::runtime_error(
            "proxy collision edge probe exhausted its configured budget");
      }
      lookupBudget.workRemaining -= amount;
    };
    const auto dimStoreRefs = storeRefsDimension(space);
    if (!dimStoreRefs) {
      throw std::logic_error("proxy store-ref index dimension is unavailable");
    }
    const auto &source = spaces_[space - 1];
    const auto sourceDimension =
        [&](const std::uint32_t sourceId) -> std::optional<DimRef> {
      const auto mapping =
          checkedDimensionMapping(sourceId, *dimStoreRefs,
                                  lookupBudget.workRemaining, chargeLookupWork);
      if (!mapping) {
        throw std::runtime_error(
            mapping.error() == DimensionMappingRefusal::Ambiguous
                ? "proxy collision lookup found ambiguous dimension mapping"
                : "proxy collision lookup exhausted its configured budget");
      }
      if (mapping->has_value()) {
        return **mapping;
      }
      const auto sourceSpace = spaceAt(sourceId);
      if (!sourceSpace || !sourceSpace->manifold) {
        return std::nullopt;
      }
      const auto *reader = sourceSpace->reader;
      if (reader == nullptr) {
        reader = sourceSpace->store != nullptr ? sourceSpace->store
                                               : sourceSpace->manifold->store();
      }
      if (reader == nullptr) {
        return std::nullopt;
      }
      const auto named = sourceSpace->manifold->dimensionNamedBounded(
          "d.store-refs", *reader, lookupBudget, chargeLookupWork);
      if (!named) {
        throw std::runtime_error("proxy collision dimension lookup refused by "
                                 "its configured budget");
      }
      return *named;
    };
    const auto sourceDim = sourceDimension(space);
    if (sourceDim && source.manifold) {
      chargeProbe(*source.manifold, foreignIndex);
      if (source.manifold->linked(foreignIndex, *sourceDim, DimVector::POS)) {
        throw std::logic_error(
            "proxy store-ref append conflicts with a source-axis neighbor");
      }
      chargeProbe(*source.manifold, foreignIndex);
      if (source.manifold->linked(foreignIndex, *sourceDim, DimVector::NEG)) {
        throw std::logic_error(
            "proxy store-ref append conflicts with a source-axis neighbor");
      }
    }

    const CellRef tail = spaceStoreRefsTail_[space - 1];
    if (isProxy(tail)) {
      const auto dense = denseOf(tail);
      const bool posShadowed =
          dense.has_value() &&
          proxyShadowedEdges_.contains(ShadowEdgeKey{
              .dense = *dense, .dim = *dimStoreRefs, .dir = DimVector::POS});
      if (!posShadowed) {
        if (const auto origin = foreignOf(tail); origin.has_value()) {
          const auto originSpace = spaceAt(origin->space);
          const auto originDim   = sourceDimension(origin->space);
          if (originSpace && originSpace->manifold && originDim) {
            chargeProbe(*originSpace->manifold, origin->index);
            if (originSpace->manifold->linked(origin->index, *originDim,
                                              DimVector::POS)) {
              throw std::logic_error(
                  "proxy store-ref tail has a source POS neighbor");
            }
          }
        }
      }
    }

    const auto bits = (static_cast<std::uint64_t>(space) << 32) |
                      static_cast<std::uint64_t>(foreignIndex);
    const CellRef proxy = mintSlot(xanadu::ValueKind::ExternRef, bits, {});

    appendLocalLink(tail, *dimStoreRefs, DimVector::POS, proxy);

    // Record the undo key before publishing into the map. If node allocation
    // fails, release() can erase the absent key and remove the minted slot.
    proxyOrder_.push_back(ProxyEntry{
        .space = space, .foreignIndex = foreignIndex, .proxy = proxy});
    const auto [it, inserted] = map.emplace(foreignIndex, proxy);
    if (!inserted) {
      const CellRef existing = it->second;
      proxyOrder_.pop_back();
      release(savepoint);
      return existing;
    }
    spaceStoreRefsTail_[space - 1] = proxy;
    discard(savepoint);
  } catch (...) {
    release(savepoint);
    throw;
  }
  const auto found = map.find(foreignIndex);
  return found->second;
}

std::optional<CellRef>
ArenaManifold::findExistingProxy(const std::uint32_t space,
                                 const CellRef foreignIndex) const noexcept {
  if (space == 0 || space > spaceProxies_.size() || foreignIndex == noCell) {
    return std::nullopt;
  }
  const auto &map = spaceProxies_[space - 1];
  const auto it   = map.find(foreignIndex);
  if (it != map.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool ArenaManifold::isProxy(const CellRef ref) const noexcept {
  if (!isEphemeral(ref)) {
    return false;
  }
  if (isQuoteOccurrence(ref)) {
    return false;
  }
  const auto dense = denseOf(ref);
  if (!dense.has_value() || *dense >= slots_.size()) {
    return false;
  }
  return slots_[*dense].valueKind ==
         static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef);
}

CellRef ArenaManifold::quoteOccurrence(const QuoteViewId view,
                                       const CellRef canonicalProxy) {
  if (!contains(canonicalProxy)) {
    throw std::invalid_argument("canonicalProxy does not exist");
  }
  const auto key = std::make_pair(view, canonicalProxy);
  if (const auto it = viewCanonicalToOccurrence_.find(key);
      it != viewCanonicalToOccurrence_.end()) {
    return it->second;
  }

  const CellRef occurrence           = mintSlot(xanadu::ValueKind::None, 0, {});
  occurrenceToCanonical_[occurrence] = canonicalProxy;
  occurrenceToView_[occurrence]      = view;
  viewCanonicalToOccurrence_[key]    = occurrence;
  quoteOccurrenceOrder_.push_back(occurrence);

  const auto dimQuoteOrigin = ensureDimension("d.quote-origin");
  CellRef cur               = canonicalProxy;
  while (true) {
    const auto nxt = linked(cur, dimQuoteOrigin, DimVector::POS);
    if (!nxt) {
      break;
    }
    cur = *nxt;
  }
  zigzag::expectWritten(link(cur, dimQuoteOrigin, DimVector::POS, occurrence));
  return occurrence;
}

bool ArenaManifold::isQuoteOccurrence(const CellRef ref) const noexcept {
  return occurrenceToCanonical_.contains(ref);
}

std::optional<CellRef>
ArenaManifold::canonicalProxyOf(const CellRef occurrence) const noexcept {
  const auto it = occurrenceToCanonical_.find(occurrence);
  if (it == occurrenceToCanonical_.end()) return std::nullopt;
  return it->second;
}

gleditor::cpp26::optional<ForeignRef>
ArenaManifold::foreignOf(const CellRef ref) const noexcept {
  if (!isEphemeral(ref)) {
    return gleditor::cpp26::nullopt;
  }
  CellRef target = ref;
  if (const auto it = occurrenceToCanonical_.find(ref);
      it != occurrenceToCanonical_.end()) {
    target = it->second;
  }
  const auto dense = denseOf(target);
  if (!dense.has_value() || *dense >= slots_.size()) {
    return gleditor::cpp26::nullopt;
  }
  const auto &cell = slots_[*dense];
  if (cell.valueKind ==
      static_cast<std::uint8_t>(xanadu::ValueKind::ExternRef)) {
    const auto sp  = static_cast<std::uint32_t>(cell.valueBits >> 32);
    const auto idx = static_cast<CellRef>(cell.valueBits & 0xFFFFFFFFULL);
    if (sp > 0 && sp <= spaces_.size()) {
      return ForeignRef{.space = sp, .index = idx};
    }
  }
  return gleditor::cpp26::nullopt;
}

gleditor::cpp26::optional<std::pair<const xanadu::Store *, CellRef>>
ArenaManifold::resolveForeign(const CellRef ref) const noexcept {
  const auto target =
      isQuoteOccurrence(ref) ? canonicalProxyOf(ref).value() : ref;
  const auto foreign = foreignOf(target);
  if (!foreign) {
    return gleditor::cpp26::nullopt;
  }
  const auto s = spaceAt(foreign->space);
  if (!s || !s->store) {
    return gleditor::cpp26::nullopt;
  }
  return std::make_pair(s->store, foreign->index);
}

gleditor::cpp26::optional<std::pair<const xanadu::Store *, CellRef>>
ArenaManifold::resolveSourceCell(const CellRef ref) const noexcept {
  if (const auto foreign = resolveForeign(ref)) return foreign;
  if (!isEphemeral(ref) && base_ && base_->contains(ref) && store())
    return std::make_pair(store(), ref);
  return gleditor::cpp26::nullopt;
}

std::optional<DimRef> ArenaManifold::dimIn(const std::uint32_t space,
                                           const DimRef dim) const noexcept {
  if (space == 0 || space > spaces_.size() || dim == noCell) {
    return std::nullopt;
  }
  auto mappingWork   = std::numeric_limits<std::size_t>::max();
  const auto mapping = checkedDimensionMapping(space, dim, mappingWork);
  if (!mapping.has_value()) {
    return std::nullopt;
  }
  if (mapping->has_value()) {
    return **mapping;
  }
  const auto s = spaceAt(space);
  if (!s || !s->manifold) {
    return std::nullopt;
  }

  if (spaces_.size() == 1) {
    std::string_view name;
    for (const auto &[k, v] : arenaDims_) {
      if (v == dim) {
        name = k;
        break;
      }
    }
    std::string nameBuf;
    if (name.empty()) {
      nameBuf = textOf(dim);
      name    = nameBuf;
    }
    if (!name.empty()) {
      std::optional<DimRef> fDim;
      if (s->reader != nullptr) {
        fDim = s->manifold->dimensionNamed(name, *s->reader);
      } else if (s->store != nullptr) {
        fDim = s->manifold->dimensionNamed(name, *s->store);
      } else if (s->manifold->store() != nullptr) {
        fDim = s->manifold->dimensionNamed(name);
      }
      if (fDim.has_value()) {
        return *fDim;
      }
    }
  }

  return std::nullopt;
}

std::expected<std::optional<DimRef>, DimensionMappingRefusal>
ArenaManifold::checkedDimensionMapping(
    const std::uint32_t space, const DimRef dim, std::size_t &workRemaining,
    const std::optional<gleditor::cpp26::function_ref<bool(std::size_t)>>
        charge) const noexcept {
  if (space == 0 || space > spaces_.size() || dim == noCell) {
    return std::nullopt;
  }
  bool fuelFailed    = false;
  const auto refusal = [&]() {
    return fuelFailed ? DimensionMappingRefusal::FuelLimitExceeded
                      : DimensionMappingRefusal::WorkLimitExceeded;
  };
  const auto spend = [&](const std::size_t amount = 1) {
    if (amount > workRemaining) {
      return false;
    }
    if (charge) {
      try {
        if (!(*charge)(amount)) {
          fuelFailed = true;
          return false;
        }
      } catch (...) {
        fuelFailed = true;
        return false;
      }
    }
    workRemaining -= amount;
    return true;
  };

  if (!spend()) {
    return std::unexpected{refusal()};
  }
  if (const auto exact = boundDimensions_.find(dim);
      exact != boundDimensions_.end()) {
    for (const auto &member : exact->second.members) {
      if (!spend()) {
        return std::unexpected{refusal()};
      }
      if (member.space == space) {
        return std::optional<DimRef>{member.dim};
      }
    }
    return std::nullopt;
  }

  std::optional<DimRef> alias;
  for (const auto &[arenaDim, set] : boundDimensions_) {
    std::ignore = arenaDim;
    if (!spend()) {
      return std::unexpected{refusal()};
    }
    bool inputMember = false;
    std::optional<DimRef> targetMember;
    for (const auto &member : set.members) {
      if (!spend()) {
        return std::unexpected{refusal()};
      }
      inputMember = inputMember || member.dim == dim;
      if (member.space == space) {
        targetMember = member.dim;
      }
    }
    if (!inputMember || !targetMember) {
      continue;
    }
    if (alias && *alias != *targetMember) {
      return std::unexpected{DimensionMappingRefusal::Ambiguous};
    }
    alias = targetMember;
  }
  if (alias) {
    return alias;
  }

  if (spaces_.size() != 1) {
    return std::nullopt;
  }
  const auto source = spaceAt(space);
  if (!source || !source->manifold) {
    return std::nullopt;
  }
  const auto home = source->manifold->home();
  const auto dims = source->manifold->dimsDimension();
  if (!home || !dims) {
    return std::nullopt;
  }
  CellRef current       = *home;
  std::size_t remaining = source->manifold->traversalBound();
  while (remaining-- > 0) {
    const auto rowSize = source->manifold->dimensionsOf(current).size();
    if (!spend(rowSize + 1)) {
      return std::unexpected{refusal()};
    }
    const auto next = source->manifold->linked(current, *dims, DimVector::POS);
    if (!next || *next == current || *next == *home) {
      return std::nullopt;
    }
    if (*next == dim) {
      return std::optional<DimRef>{dim};
    }
    current = *next;
  }
  return std::nullopt;
}

std::expected<std::vector<EffectiveLinkRow>, EffectiveLinkRefusal>
ArenaManifold::snapshotLinks(
    const CellRef owner, const EffectiveLinkLimits &limits,
    const std::optional<DimensionDirectory> directory) const {
  if (limits.maxCandidates == 0 || limits.maxLinks == 0 ||
      limits.maxWork == 0) {
    return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
  }
  if (!contains(owner)) {
    return std::unexpected{EffectiveLinkRefusal::UnknownOwner};
  }
  const auto foreignOwner = isProxy(owner) ? foreignOf(owner) : std::nullopt;

  std::size_t workRemaining     = limits.maxWork;
  std::size_t candidateAttempts = 0;
  DimensionLookupBudget nameBudget{dimensionLookupLimits_};
  std::vector<DimRef> candidates;
  auto spend = [&](const std::size_t amount = 1) {
    if (amount > workRemaining) {
      return false;
    }
    workRemaining -= amount;
    try {
      for (std::size_t i = 0; i < amount; i++) {
        consumeWork();
      }
    } catch (const std::runtime_error &) {
      return false;
    }
    return true;
  };
  const auto chargeNameWork = [&](const std::size_t amount) {
    return spend(amount);
  };
  const auto spendNameWork = [&](const std::size_t amount = 1) {
    if (amount > nameBudget.workRemaining) {
      return false;
    }
    if (!chargeNameWork(amount)) {
      return false;
    }
    nameBudget.workRemaining -= amount;
    return true;
  };
  const auto resolveName = [&](const DimRef arenaDim, const Space &sourceSpace)
      -> std::expected<std::optional<DimRef>, DimensionLookupRefusal> {
    std::string_view name;
    for (const auto &[key, dim] : arenaDims_) {
      if (!spendNameWork()) {
        return std::unexpected{DimensionLookupRefusal::FuelLimitExceeded};
      }
      if (dim == arenaDim) {
        name = key;
        break;
      }
    }
    std::string extractedName;
    if (name.empty()) {
      auto text = boundedTextOf(arenaDim, nameBudget, chargeNameWork);
      if (!text) {
        return std::unexpected{text.error()};
      }
      extractedName = std::move(*text);
      name          = extractedName;
    }
    if (name.empty()) {
      return std::optional<DimRef>{};
    }
    const xanadu::SpanReader *reader = sourceSpace.reader;
    if (reader == nullptr) {
      reader = sourceSpace.store != nullptr ? sourceSpace.store
                                            : sourceSpace.manifold->store();
    }
    if (reader == nullptr) {
      return std::optional<DimRef>{};
    }
    return sourceSpace.manifold->dimensionNamedBounded(
        name, *reader, nameBudget, chargeNameWork);
  };
  auto addCandidate = [&](const DimRef dim) {
    if (dim == noCell) {
      return true;
    }
    if (candidateAttempts >= limits.maxCandidates || !spend()) {
      return false;
    }
    candidateAttempts++;
    for (const DimRef current : candidates) {
      if (!spend()) {
        return false;
      }
      if (current == dim) {
        return true;
      }
    }
    candidates.push_back(dim);
    return true;
  };

  // Manifold::dimensions() fills a cache by walking the whole rank. Walk its
  // public rank endpoints directly so the caller's limits apply before any
  // candidate vector grows.
  if (base_ != nullptr) {
    const auto home = base_->home();
    const auto dims = base_->dimsDimension();
    if (home && dims) {
      CellRef current       = *home;
      std::size_t remaining = base_->traversalBound();
      while (remaining-- > 0) {
        const auto rowSize = base_->dimensionsOf(current).size();
        if (!spend(rowSize + 1)) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
        const auto next = base_->linked(current, *dims, DimVector::POS);
        if (!next || *next == current || *next == *home) {
          break;
        }
        if (!addCandidate(*next)) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
        current = *next;
      }
    }
  }
  if (foreignOwner && directory) {
    const CellRef head = directory->head;
    if (head == noCell || directory->axis == noCell || !contains(head) ||
        !contains(directory->axis) || isProxy(head) ||
        isQuoteOccurrence(head) || isProxy(directory->axis) ||
        isQuoteOccurrence(directory->axis)) {
      return std::unexpected{EffectiveLinkRefusal::InvalidDimensionDirectory};
    }
    CellRef current = head;
    while (true) {
      if (!contains(current) || isProxy(current) ||
          isQuoteOccurrence(current)) {
        return std::unexpected{EffectiveLinkRefusal::InvalidDimensionDirectory};
      }
      if (!addCandidate(current)) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      const auto directoryRows = dimensionsOf(current);
      if (!spend(1 + directoryRows.size())) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      std::optional<CellRef> next;
      bool axisRowFound = false;
      for (const auto &edge : directoryRows) {
        if (edge.dim != directory->axis) {
          continue;
        }
        if (axisRowFound) {
          return std::unexpected{
              EffectiveLinkRefusal::InvalidDimensionDirectory};
        }
        axisRowFound = true;
        if (edge.pos != noCell) {
          next = edge.pos;
        }
      }
      if (!next || *next == current || *next == head) {
        break;
      }
      current = *next;
    }
  }
  for (const auto &[name, dim] : arenaDims_) {
    std::ignore = name;
    if (!spend()) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
    if (contains(dim) && !addCandidate(dim)) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
  }
  for (const auto &[dim, set] : boundDimensions_) {
    std::ignore = set;
    if (!spend()) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
    if (contains(dim) && !addCandidate(dim)) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
  }
  for (const auto &[arenaDim, set] : boundDimensions_) {
    std::ignore = arenaDim;
    for (const auto &member : set.members) {
      if (!spend()) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      if (contains(member.dim) && !addCandidate(member.dim)) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
    }
  }

  std::span<const DimLink> ownerRows;
  if (const auto dense = denseOf(owner); dense.has_value()) {
    const auto &slot = slots_[*dense];
    ownerRows        = std::span<const DimLink>{links_.data() + slot.linkOffset,
                                                slot.linkCount};
  } else if (base_ != nullptr) {
    ownerRows = base_->dimensionsOf(owner);
  }
  for (const auto &edge : ownerRows) {
    if (!spend()) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
    if (!contains(edge.dim)) {
      return std::unexpected{EffectiveLinkRefusal::UnmappedDimension};
    }
    if (!addCandidate(edge.dim)) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
  }

  // A deterministic row order is part of the snapshot. Insertion sort keeps
  // its comparison work visible to the same budget and needs no second buffer.
  for (std::size_t i = 1; i < candidates.size(); i++) {
    const DimRef value = candidates[i];
    std::size_t at     = i;
    while (at > 0) {
      if (!spend()) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      if (candidates[at - 1] <= value) {
        break;
      }
      candidates[at] = candidates[at - 1];
      at--;
    }
    candidates[at] = value;
  }

  const Space *foreignSpace = nullptr;
  if (isProxy(owner)) {
    if (!foreignOwner) {
      return std::unexpected{EffectiveLinkRefusal::UnknownOwner};
    }
    const auto attached = spaceAt(foreignOwner->space);
    if (!attached || !attached->manifold) {
      return std::unexpected{EffectiveLinkRefusal::UnknownOwner};
    }
    foreignSpace = &*attached;
  }

  std::vector<EffectiveLinkRow> rows;
  rows.reserve(candidates.size());
  std::vector<std::optional<DimRef>> sourceDims(candidates.size());
  std::vector<bool> posShadowedRows(candidates.size());
  std::vector<bool> negShadowedRows(candidates.size());
  std::size_t linkedCount = 0;
  auto accountLink        = [&](std::optional<EffectiveLinkNeighbor> &side,
                         const std::optional<CellRef> neighbor) {
    if (!neighbor) {
      return true;
    }
    if (linkedCount >= limits.maxLinks) {
      return false;
    }
    linkedCount++;
    side = EffectiveLinkNeighbor{*neighbor};
    return true;
  };

  if (foreignOwner) {
    const auto dense = denseOf(owner);
    for (std::size_t i = 0; i < candidates.size(); i++) {
      const DimRef dim = candidates[i];
      if (!spend(2)) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      posShadowedRows[i] =
          dense.has_value() &&
          proxyShadowedEdges_.contains(ShadowEdgeKey{
              .dense = *dense, .dim = dim, .dir = DimVector::POS});
      negShadowedRows[i] =
          dense.has_value() &&
          proxyShadowedEdges_.contains(ShadowEdgeKey{
              .dense = *dense, .dim = dim, .dir = DimVector::NEG});
      std::size_t mappingWork = workRemaining;
      const auto mapped =
          checkedDimensionMapping(foreignOwner->space, dim, mappingWork);
      const std::size_t usedWork = workRemaining - mappingWork;
      if (!spend(usedWork)) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      if (!mapped.has_value()) {
        return std::unexpected{mapped.error() ==
                                       DimensionMappingRefusal::Ambiguous
                                   ? EffectiveLinkRefusal::AmbiguousDimension
                                   : EffectiveLinkRefusal::LimitExceeded};
      }
      sourceDims[i] = *mapped;
      if (!sourceDims[i] && spaces_.size() == 1) {
        const auto named = resolveName(dim, *foreignSpace);
        if (!named) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
        sourceDims[i] = *named;
      }
    }
  }

  for (std::size_t i = 0; i < candidates.size(); i++) {
    const DimRef dim = candidates[i];
    EffectiveLinkRow row{.dim = dim};
    std::optional<DimLink> local;
    for (const auto &edge : ownerRows) {
      if (!spend()) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      if (edge.dim == dim) {
        local = edge;
        break;
      }
    }
    if (!foreignOwner) {
      if (local.has_value()) {
        if (!spend(2)) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
        if (!accountLink(row.pos, local->neighbor(DimVector::POS)) ||
            !accountLink(row.neg, local->neighbor(DimVector::NEG))) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
      }
      rows.push_back(std::move(row));
      continue;
    }

    const bool posShadowed = posShadowedRows[i];
    const bool negShadowed = negShadowedRows[i];
    if (posShadowed &&
        !accountLink(row.pos,
                     local ? local->neighbor(DimVector::POS) : std::nullopt)) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
    if (negShadowed &&
        !accountLink(row.neg,
                     local ? local->neighbor(DimVector::NEG) : std::nullopt)) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }

    if (sourceDims[i].has_value()) {
      const auto sourceRows =
          foreignSpace->manifold->dimensionsOf(foreignOwner->index);
      if (!spend(sourceRows.size())) {
        return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
      }
      for (const auto &edge : sourceRows) {
        if (edge.dim != *sourceDims[i]) {
          continue;
        }
        if (!posShadowed && edge.pos != noCell) {
          if (linkedCount >= limits.maxLinks) {
            return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
          }
          row.pos = EffectiveLinkNeighbor{
              ForeignRef{.space = foreignOwner->space, .index = edge.pos}};
          linkedCount++;
        }
        if (!negShadowed && edge.neg != noCell) {
          if (linkedCount >= limits.maxLinks) {
            return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
          }
          row.neg = EffectiveLinkNeighbor{
              ForeignRef{.space = foreignOwner->space, .index = edge.neg}};
          linkedCount++;
        }
        break;
      }
    }
    rows.push_back(std::move(row));
  }

  // Every nonempty source CSR axis must be represented by at least one
  // effective row; otherwise the snapshot would silently omit a foreign edge.
  if (foreignOwner) {
    const auto sourceRows =
        foreignSpace->manifold->dimensionsOf(foreignOwner->index);
    if (!spend(sourceRows.size())) {
      return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
    }
    for (const auto &edge : sourceRows) {
      if (edge.pos == noCell && edge.neg == noCell) {
        continue;
      }
      bool represented = false;
      for (std::size_t i = 0; i < candidates.size(); i++) {
        if (!spend()) {
          return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
        }
        if (sourceDims[i] && *sourceDims[i] == edge.dim) {
          represented = true;
          break;
        }
      }
      if (!represented) {
        return std::unexpected{
            spaces_.size() == 1 ? EffectiveLinkRefusal::NameResolutionRequired
                                : EffectiveLinkRefusal::UnmappedDimension};
      }
    }
  }
  if (!spend(rows.size())) {
    return std::unexpected{EffectiveLinkRefusal::LimitExceeded};
  }
  std::erase_if(rows, [](const EffectiveLinkRow &row) {
    return !row.pos.has_value() && !row.neg.has_value();
  });
  return rows;
}

std::optional<DimRef>
ArenaManifold::arenaDimFor(const std::optional<std::uint32_t> space,
                           const DimRef foreignDim) const noexcept {
  if (foreignDim == noCell ||
      (space && (*space == 0 || *space > spaces_.size()))) {
    return std::nullopt;
  }
  for (const auto &[arenaDim, bSet] : boundDimensions_) {
    for (const auto &member : bSet.members) {
      if ((!space || member.space == *space) && member.dim == foreignDim) {
        return arenaDim;
      }
    }
  }
  if (!space && boundDimensions_.contains(foreignDim)) {
    return foreignDim;
  }
  return std::nullopt;
}

ArenaManifold *ArenaManifold::bindDimension(const DimRef arenaDim,
                                            const std::uint32_t space,
                                            const DimRef foreignDim,
                                            const DimensionBindingMode mode) {
  auto &set    = boundDimensions_[arenaDim];
  set.arenaDim = arenaDim;
  if (set.name.empty()) {
    set.name = textOf(arenaDim);
  }
  for (auto &member : set.members) {
    if (member.space == space) {
      member.dim  = foreignDim;
      member.mode = mode;
      return this;
    }
  }
  set.members.push_back(
      BoundDimensionMember{.space = space, .dim = foreignDim, .mode = mode});
  return this;
}

ArenaManifold *ArenaManifold::bindSharedIdentities() {
  std::map<xanadu::GlobalOpRef, std::vector<std::pair<std::uint32_t, DimRef>>>
      groups;

  for (std::uint32_t spaceId = 1; spaceId <= spaces_.size(); ++spaceId) {
    const auto s = spaceAt(spaceId);
    if (!s || !s->manifold) {
      continue;
    }
    for (const auto dim : s->manifold->dimensions()) {
      std::optional<xanadu::GlobalOpRef> gRef;
      if (s->store != nullptr) {
        if (const auto ext = s->store->externTarget(dim); ext.has_value()) {
          const auto rec = s->store->scrollRegistry().findRecord(ext->scroll);
          if (rec.has_value()) {
            gRef = xanadu::GlobalOpRef{.scroll   = rec->globalKey,
                                       .produces = ext->produces};
          }
        } else {
          const auto slot = s->manifold->slot(dim);
          if (slot.has_value() && slot->birthOp != 0) {
            const auto birth = s->store->segmentedOps().idOf(slot->birthOp);
            auto sKey        = s->sealedAs != nullptr
                                   ? scrollKey(*s->sealedAs)
                                   : s->store->bootstrapPermascrollKey();
            if (!sKey && !s->label.empty()) {
              sKey = s->label;
            }
            if (sKey && birth && !birth->isZero()) {
              gRef = xanadu::GlobalOpRef{.scroll = *sKey, .produces = *birth};
            }
          }
        }
      }
      if (gRef.has_value()) {
        groups[*gRef].push_back({spaceId, dim});
      }
    }
  }

  for (const auto &[gRef, memberList] : groups) {
    if (memberList.size() <= 1) {
      continue;
    }
    std::optional<DimRef> targetArenaDim;
    for (const auto &[sp, d] : memberList) {
      for (const auto &[aDim, bSet] : boundDimensions_) {
        for (const auto &m : bSet.members) {
          if (m.space == sp && m.dim == d) {
            targetArenaDim = aDim;
            break;
          }
        }
        if (targetArenaDim) {
          break;
        }
      }
      if (targetArenaDim) {
        break;
      }
    }

    if (!targetArenaDim) {
      std::string name;
      const auto firstSp = spaceAt(memberList.front().first);
      if (firstSp && firstSp->manifold) {
        if (firstSp->reader) {
          name = firstSp->manifold->textOf(memberList.front().second,
                                           *firstSp->reader);
        } else if (firstSp->store) {
          name = firstSp->manifold->textOf(memberList.front().second,
                                           *firstSp->store);
        }
      }
      if (name.empty()) {
        name = "d.shared_" + gRef.produces.str();
      }
      targetArenaDim = ensureDimension(name);
    }

    for (const auto &[sp, d] : memberList) {
      bindDimension(*targetArenaDim, sp, d,
                    DimensionBindingMode::SharedIdentity);
    }
  }
  return this;
}

ArenaManifold *ArenaManifold::bindDimensionsByNameMatch() {
  std::map<std::string, std::vector<std::pair<std::uint32_t, DimRef>>> byName;
  for (std::uint32_t spaceId = 1; spaceId <= spaces_.size(); ++spaceId) {
    const auto s = spaceAt(spaceId);
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
      if (!name.empty()) {
        byName[name].push_back({spaceId, dim});
      }
    }
  }

  for (const auto &[name, members] : byName) {
    if (members.size() <= 1) {
      continue;
    }
    const auto arenaDim = ensureDimension(name);
    for (const auto &[spaceId, dim] : members) {
      bindDimension(arenaDim, spaceId, dim, DimensionBindingMode::NameMatch);
    }
  }
  return this;
}

gleditor::cpp26::optional<const BoundDimensionSet &>
ArenaManifold::boundDimensionSet(const DimRef dim) const noexcept {
  const auto it = boundDimensions_.find(dim);
  if (it != boundDimensions_.end()) {
    return it->second;
  }
  return gleditor::cpp26::nullopt;
}

ArenaManifold *ArenaManifold::materializeFrontier(const std::uint32_t space,
                                                  const CellRef foreignHead,
                                                  const DimRef foreignDim,
                                                  const DimVector dir,
                                                  const std::size_t maxSteps) {
  const auto s = spaceAt(space);
  if (!s || !s->manifold) {
    return this;
  }
  std::optional<CellRef> cur = gleditor::fromSentinel<noCell>(foreignHead);
  if (isProxy(foreignHead)) {
    const auto foreign = foreignOf(foreignHead);
    if (foreign && foreign->space == space) {
      cur = foreign->index;
    }
  }
  DimRef fDim = foreignDim;
  if (contains(foreignDim)) {
    const auto resolved = dimIn(space, foreignDim);
    if (resolved) {
      fDim = *resolved;
    }
  }
  std::size_t steps = 0;
  while (cur && steps < maxSteps) {
    proxyFor(space, *cur);
    cur = s->manifold->linked(*cur, fDim, dir);
    steps++;
  }
  return this;
}

void ArenaManifold::forEachSpace(
    const gleditor::cpp26::function_ref<void(std::uint32_t, const Space &)>
        visitor) const {
  for (std::size_t i = 0; i < spaces_.size(); ++i) {
    visitor(static_cast<std::uint32_t>(i + 1), spaces_[i]);
  }
}

void ArenaManifold::forEachProxy(
    const std::uint32_t space,
    const gleditor::cpp26::function_ref<void(CellRef, CellRef)> visitor) const {
  if (space == 0 || space > spaceProxies_.size()) {
    return;
  }
  for (const auto &[fIdx, proxy] : spaceProxies_[space - 1]) {
    visitor(proxy, fIdx);
  }
}

void ArenaManifold::forEachQuoteOccurrence(
    const QuoteViewId view,
    const gleditor::cpp26::function_ref<void(CellRef, CellRef)> visitor) const {
  for (const auto &[key, occ] : viewCanonicalToOccurrence_) {
    if (key.first == view) {
      visitor(occ, key.second);
    }
  }
}

SlotRef ArenaManifold::slot(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  if (dense.has_value()) {
    return SlotRef{slots_[*dense]};
  }
  return nullptr == base_ ? SlotRef{} : base_->slot(ref);
}

DimLink *ArenaManifold::existingLink(const std::uint32_t dense,
                                     const DimRef dim) noexcept {
  const auto &cell = slots_[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    if (links_[static_cast<std::size_t>(cell.linkOffset) + i].dim == dim) {
      return &links_[static_cast<std::size_t>(cell.linkOffset) + i];
    }
  }
  return nullptr;
}

DimLink *ArenaManifold::linkFor(const std::uint32_t dense, const DimRef dim) {
  if (DimLink *const found = existingLink(dense, dim); nullptr != found) {
    return found;
  }
  // Manifold compacts here when the arena is mostly dead. This one must not
  // while a mark is outstanding: compaction moves every run, and a Mark is a
  // set of offsets into the arenas it would move. Nothing is lost by waiting,
  // since release() truncates the dead runs away.
  if (0 == outstandingMarks_ &&
      links_.size() > 2 * liveLinks_ + compactionSlack) {
    zigzag::expectWritten(compact());
  }

  auto &cell = slots_[dense];
  if (cell.linkCount == std::numeric_limits<std::uint16_t>::max()) {
    return nullptr;
  }

  const std::size_t offset = cell.linkOffset;
  const std::size_t count  = cell.linkCount;
  if (offset + count != links_.size()) {
    if (links_.capacity() < links_.size() + count + 1) {
      links_.reserve(std::max(links_.size() * 2, links_.size() + count + 1));
    }
    const auto relocated = links_.size();
    for (std::size_t i = 0; i < count; i++) {
      links_.push_back(links_[offset + i]);
    }
    cell.linkOffset = static_cast<std::uint32_t>(relocated);
  }

  links_.push_back(DimLink{.dim = dim, .pos = noCell, .neg = noCell});
  cell.linkCount++;
  liveLinks_++;
  return &links_.back();
}

void ArenaManifold::setOneSide(const std::uint32_t dense, const DimRef dim,
                               const DimVector dir, const CellRef to) {
  // The one funnel every link write goes through, so the one place the trail
  // has to be consulted.
  trail(dense);
  if (isProxy(refOf(dense))) {
    const ShadowEdgeKey key{.dense = dense, .dim = dim, .dir = dir};
    if (!proxyShadowedEdges_.contains(key)) {
      proxyShadowedEdgeOrder_.push_back(key);
      try {
        proxyShadowedEdges_.insert(key);
      } catch (...) {
        proxyShadowedEdgeOrder_.pop_back();
        throw;
      }
    }
  }
  DimLink *const edge = linkFor(dense, dim);
  if (nullptr == edge) {
    throw std::length_error(
        "arena cell link run exceeds CellSlot::linkCount limit " +
        std::to_string(maxCellLinkRunCount));
  }
  edge->encodedNeighbor(dir) = to;
}

void ArenaManifold::appendLocalLink(const CellRef from, const DimRef dim,
                                    const DimVector dir, const CellRef to) {
  const auto fromOld = localLink(from, dim);
  const auto toOld   = localLink(to, dim);
  const auto displacedFrom =
      fromOld.has_value() ? fromOld->neighbor(dir) : std::nullopt;
  const auto displacedTo =
      toOld.has_value() ? toOld->neighbor(-dir) : std::nullopt;

  std::array<SideWrite, 4> writes{};
  std::size_t writeCount = 0;
  auto add               = [&](const CellRef cell, const DimVector side,
                 const std::optional<CellRef> neighbor) {
    writes[writeCount++] =
        SideWrite{.cell = cell, .dir = side, .neighbor = neighbor};
  };

  if (displacedFrom && displacedFrom != to) {
    add(*displacedFrom, -dir, std::nullopt);
  }
  if (displacedTo && displacedTo != from) {
    add(*displacedTo, dir, std::nullopt);
  }
  add(to, -dir, from);
  add(from, dir, to);

  applyLinkWrites(dim, std::span{writes}.first(writeCount));
}

std::optional<DimLink> ArenaManifold::localLink(const CellRef cell,
                                                const DimRef dim) const {
  if (const auto dense = denseOf(cell); dense.has_value()) {
    const auto &slot = slots_[*dense];
    for (std::uint16_t i = 0; i < slot.linkCount; i++) {
      const auto &edge = links_[static_cast<std::size_t>(slot.linkOffset) + i];
      if (edge.dim == dim) {
        return edge;
      }
    }
    return std::nullopt;
  }
  if (base_ != nullptr && base_->contains(cell)) {
    for (const auto &edge : base_->dimensionsOf(cell)) {
      if (edge.dim == dim) {
        return edge;
      }
    }
  }
  return std::nullopt;
}

void ArenaManifold::applyLinkWrites(const DimRef dim,
                                    const std::span<const SideWrite> writes) {
  std::array<std::uint32_t, 4> denseRows{};
  if (writes.size() > denseRows.size()) {
    throw std::logic_error("arena link write plan exceeds four sides");
  }

  // Capacity is checked against the current owner before shadow() can create
  // a COW slot, so a full base row leaves the overlay unchanged on refusal.
  for (std::size_t i = 0; i < writes.size(); i++) {
    const auto &write     = writes[i];
    std::size_t linkCount = 0;
    if (const auto dense = denseOf(write.cell); dense.has_value()) {
      linkCount = slots_[*dense].linkCount;
    } else if (base_ != nullptr && base_->contains(write.cell)) {
      linkCount = base_->dimensionsOf(write.cell).size();
    } else {
      throw std::logic_error("arena link write plan names an unknown cell");
    }
    if (!localLink(write.cell, dim).has_value() &&
        linkCount >= maxCellLinkRunCount) {
      throw std::length_error(
          "arena cell link run exceeds CellSlot::linkCount limit " +
          std::to_string(maxCellLinkRunCount));
    }
  }

  for (std::size_t i = 0; i < writes.size(); i++) {
    const auto dense = shadow(writes[i].cell);
    if (!dense.has_value()) {
      throw std::logic_error("arena link write cell could not be shadowed");
    }
    denseRows[i] = *dense;
  }

  for (std::size_t i = 0; i < writes.size(); i++) {
    const auto &write = writes[i];
    setOneSide(denseRows[i], dim, write.dir, write.neighbor.value_or(noCell));
  }
}

OptionalCell ArenaManifold::linked(const CellRef from, const DimRef dim,
                                   const DimVector dir) const noexcept {
  const auto dense = denseOf(from);
  if (!dense.has_value()) {
    return nullptr == base_ ? std::nullopt : base_->linked(from, dim, dir);
  }
  const auto d = *dense;
  if (isQuoteOccurrence(from)) {
    // Only authored / materialized local edges exist in this presentation view
    const auto &cell = slots_[d];
    for (std::uint16_t i = 0; i < cell.linkCount; i++) {
      const auto &edge = links_[static_cast<std::size_t>(cell.linkOffset) + i];
      if (edge.dim == dim) {
        return edge.neighbor(dir);
      }
    }
    return std::nullopt;
  }
  if (isProxy(from)) {
    // Check if explicitly shadowed locally
    if (proxyShadowedEdges_.contains(
            ShadowEdgeKey{.dense = d, .dim = dim, .dir = dir})) {
      const auto &cell = slots_[d];
      for (std::uint16_t i = 0; i < cell.linkCount; i++) {
        const auto &edge =
            links_[static_cast<std::size_t>(cell.linkOffset) + i];
        if (edge.dim == dim) {
          return edge.neighbor(dir);
        }
      }
      return std::nullopt;
    }
    // Delegate to foreign space
    const auto foreign = foreignOf(from);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        const auto fDim = dimIn(foreign->space, dim);
        if (fDim) {
          const auto fAns = s->manifold->linked(foreign->index, *fDim, dir);
          if (fAns.has_value()) {
            const auto existing = findExistingProxy(foreign->space, *fAns);
            return existing;
          }
        }
      }
    }
    return std::nullopt;
  }
  const auto &cell = slots_[d];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    const auto &edge = links_[static_cast<std::size_t>(cell.linkOffset) + i];
    if (edge.dim == dim) {
      return edge.neighbor(dir);
    }
  }
  return std::nullopt;
}

OptionalCell ArenaManifold::linkedMinting(const CellRef from, const DimRef dim,
                                          const DimVector dir) {
  if (const auto found = linked(from, dim, dir); found.has_value()) {
    return found;
  }
  // Only a proxy delegates to a foreign space, and only there can a
  // neighbour exist that linked() could not name.
  const auto dense = denseOf(from);
  if (!dense.has_value() || isQuoteOccurrence(from) || !isProxy(from) ||
      proxyShadowedEdges_.contains(
          ShadowEdgeKey{.dense = *dense, .dim = dim, .dir = dir})) {
    return std::nullopt;
  }
  const auto foreign = foreignOf(from);
  if (!foreign) {
    return std::nullopt;
  }
  const auto space = spaceAt(foreign->space);
  if (!space || !space->manifold) {
    return std::nullopt;
  }
  const auto foreignDim = dimIn(foreign->space, dim);
  if (!foreignDim) {
    return std::nullopt;
  }
  const auto answer = space->manifold->linked(foreign->index, *foreignDim, dir);
  if (!answer.has_value()) {
    return std::nullopt;
  }
  return proxyFor(foreign->space, *answer);
}

std::span<const DimLink>
ArenaManifold::dimensionsOf(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  if (!dense.has_value()) {
    return nullptr == base_ ? std::span<const DimLink>{}
                            : base_->dimensionsOf(ref);
  }
  const auto d = *dense;
  if (isProxy(ref)) {
    const auto &cell = slots_[d];
    if (cell.linkCount > 0) {
      return std::span<const DimLink>{links_.data() + cell.linkOffset,
                                      cell.linkCount};
    }
    const auto foreign = foreignOf(ref);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        return s->manifold->dimensionsOf(foreign->index);
      }
    }
  }
  const auto &cell = slots_[d];
  return std::span<const DimLink>{links_.data() + cell.linkOffset,
                                  cell.linkCount};
}

std::span<const xanadu::PrimediaSpan>
ArenaManifold::contentOf(const CellRef ref) const noexcept {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        return s->manifold->contentOf(foreign->index);
      }
    }
  }
  const auto dense = denseOf(target);
  if (!dense.has_value()) {
    return nullptr == base_ ? std::span<const xanadu::PrimediaSpan>{}
                            : base_->contentOf(target);
  }
  const auto &cell = slots_[*dense];
  return std::span<const xanadu::PrimediaSpan>{
      content_.data() + cell.spanOffset, cell.spanCount};
}

std::optional<CellRef>
ArenaManifold::cloneMaster(const CellRef ref,
                           const DimRef cloneDim) const noexcept {
  return rankEnd(*this, ref, cloneDim, DimVector::NEG);
}

xanadu::ValueKind ArenaManifold::valueKindOf(const CellRef ref) const noexcept {
  return slot(ref)
      .transform([](const CellSlot &cell) noexcept {
        return static_cast<xanadu::ValueKind>(cell.valueKind);
      })
      .value_or(xanadu::ValueKind::None);
}

std::optional<double>
ArenaManifold::asDouble(const CellRef ref) const noexcept {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        return s->manifold->asDouble(foreign->index);
      }
    }
  }
  const auto cell = slot(ref);
  if (!cell || xanadu::ValueKind::Double !=
                   static_cast<xanadu::ValueKind>(cell->valueKind)) {
    return std::nullopt;
  }
  return std::bit_cast<double>(cell->valueBits);
}

std::optional<bool> ArenaManifold::asBool(const CellRef ref) const noexcept {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        return s->manifold->asBool(foreign->index);
      }
    }
  }
  const auto cell = slot(ref);
  if (!cell || xanadu::ValueKind::Bool !=
                   static_cast<xanadu::ValueKind>(cell->valueKind)) {
    return std::nullopt;
  }
  return 0 != cell->valueBits;
}

std::optional<std::int64_t>
ArenaManifold::asInt64(const CellRef ref) const noexcept {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        return s->manifold->asInt64(foreign->index);
      }
    }
  }
  const auto cell = slot(ref);
  if (!cell || xanadu::ValueKind::Int64 !=
                   static_cast<xanadu::ValueKind>(cell->valueKind)) {
    return std::nullopt;
  }
  return std::bit_cast<std::int64_t>(cell->valueBits);
}

std::optional<CellRef>
ArenaManifold::handleTarget(const CellRef ref) const noexcept {
  const auto cell = slot(ref);
  if (!cell || xanadu::ValueKind::OpHandle !=
                   static_cast<xanadu::ValueKind>(cell->valueKind)) {
    return std::nullopt;
  }
  return static_cast<CellRef>(cell->valueBits);
}

std::string_view
ArenaManifold::scratchTextOf(const xanadu::PrimediaSpan &span) const noexcept {
  if (xanadu::scratchScroll != span.scroll || span.start > scratch_.size()) {
    return {};
  }
  const auto length =
      std::min<std::uint64_t>(span.length, scratch_.size() - span.start);
  return std::string_view{scratch_}.substr(span.start, length);
}

std::string ArenaManifold::textOf(const CellRef ref,
                                  const xanadu::SpanReader *reader) const {
  if (!reader) reader = defaultReader_;
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s) {
        const xanadu::SpanReader *useReader = s->reader;
        if (useReader == nullptr) {
          useReader = s->store != nullptr ? s->store : reader;
        }
        if (s->manifold != nullptr && useReader != nullptr) {
          return s->manifold->textOf(foreign->index, *useReader);
        }
      }
    }
  }
  // A quotation's spans are in its space's namespace, whatever was passed.
  if (const auto quoted = quoteSpace_.find(target);
      quoteSpace_.end() != quoted) {
    if (const auto s = spaceAt(quoted->second)) {
      if (nullptr != s->reader) {
        reader = s->reader;
      } else if (nullptr != s->store) {
        reader = s->store;
      }
    }
  }
  std::string out;
  for (const auto &span : contentOf(ref)) {
    if (xanadu::scratchScroll == span.scroll) {
      out += scratchTextOf(span);
    } else if (nullptr != reader) {
      out += reader->read(span);
    } else if (nullptr != store_) {
      out += store_->read(span);
    } else if (nullptr != base_ && nullptr != base_->store()) {
      out += base_->store()->read(span);
    }
  }
  return out;
}

std::expected<std::string, DimensionLookupRefusal> ArenaManifold::boundedTextOf(
    const CellRef ref, DimensionLookupBudget &budget,
    const std::optional<gleditor::cpp26::function_ref<bool(std::size_t)>>
        charge) const {
  const xanadu::SpanReader *reader = defaultReader_;
  CellRef target                   = ref;
  bool directSourceRead            = false;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  if (isProxy(target)) {
    const auto foreign = foreignOf(target);
    if (foreign) {
      if (const auto s = spaceAt(foreign->space)) {
        if (s->reader != nullptr) {
          reader = s->reader;
        } else if (s->store != nullptr) {
          reader = s->store;
        }
        directSourceRead = s->manifold != nullptr && reader != nullptr;
      }
    }
  }
  if (!directSourceRead) {
    if (const auto quoted = quoteSpace_.find(target);
        quoteSpace_.end() != quoted) {
      if (const auto s = spaceAt(quoted->second)) {
        if (s->reader != nullptr) {
          reader = s->reader;
        } else if (s->store != nullptr) {
          reader = s->store;
        }
      }
    }
  }
  if (reader == nullptr) {
    reader = store_ != nullptr ? store_
                               : (base_ != nullptr ? base_->store() : nullptr);
  }

  const auto spans = contentOf(ref);
  return readDimensionTextBounded(
      spans, budget,
      [&](const xanadu::PrimediaSpan &span) {
        if (!directSourceRead && span.scroll == xanadu::scratchScroll) {
          return std::string{scratchTextOf(span)};
        }
        return reader != nullptr ? reader->read(span) : std::string{};
      },
      charge);
}

xanadu::PrimediaSpan ArenaManifold::intern(const std::string_view text) {
  consumeWork();
  if (scratchLimit_ && (scratch_.size() > *scratchLimit_ ||
                        text.size() > *scratchLimit_ - scratch_.size()))
    throw std::runtime_error("Vortex evaluation byte budget exhausted");
  const auto start = scratch_.size();
  scratch_ += text;
  return xanadu::PrimediaSpan{
      .scroll = xanadu::scratchScroll, .start = start, .length = text.size()};
}

CellRef
ArenaManifold::mintSlot(const xanadu::ValueKind kind, const std::uint64_t bits,
                        const std::span<const xanadu::PrimediaSpan> content) {
  if (content.size() > maxCellContentSpanCount) {
    throw std::length_error("ArenaManifold content run exceeds CellSlot");
  }
  consumeWork();
  if (slots_.size() >= allocationLimit_) {
    throw std::runtime_error(
        "ArenaManifold exhausted: cannot allocate beyond " +
        std::to_string(allocationLimit_));
  }
  const auto dense = static_cast<std::uint32_t>(slots_.size());
  slots_.push_back(CellSlot{
      .spanOffset  = static_cast<std::uint32_t>(content_.size()),
      .spanCount   = 0,
      .formatFlags = 0,
      // An arena cell has no birth operation. birthOp carries its own ref so
      // that code shared with Manifold -- which reads slot.birthOp to name the
      // cell a slot belongs to -- means the same thing here.
      .birthOp    = refOf(dense),
      .lastOp     = refOf(dense),
      .linkOffset = static_cast<std::uint32_t>(links_.size()),
      .linkCount  = 0,
      .valueKind  = static_cast<std::uint8_t>(kind),
      .flags      = 0,
      .valueBits  = bits,
  });
  if (!content.empty()) {
    setContentAt(dense, content);
  }
  return refOf(dense);
}

CellRef ArenaManifold::makeCell() {
  return mintSlot(xanadu::ValueKind::None, 0, {});
}

CellRef ArenaManifold::makeCell(const xanadu::PrimediaSpan &content) {
  return mintSlot(xanadu::ValueKind::None, 0,
                  std::span<const xanadu::PrimediaSpan>{&content, 1});
}

CellRef
ArenaManifold::makeQuote(const std::uint32_t space,
                         const std::span<const xanadu::PrimediaSpan> content) {
  const auto cell = mintSlot(xanadu::ValueKind::None, 0, content);
  quoteSpace_.emplace(cell, space);
  quoteSpaceOrder_.push_back(cell);
  return cell;
}

std::optional<ArenaManifold::QuotedContent>
ArenaManifold::quotedContent(const CellRef ref) const {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref).value();
  }
  const xanadu::Store *store = store_;
  if (const auto foreign = foreignOf(target)) {
    const auto s = spaceAt(foreign->space);
    store        = s ? s->store : nullptr;
  } else if (const auto quoted = quoteSpace_.find(target);
             quoteSpace_.end() != quoted) {
    const auto s = spaceAt(quoted->second);
    store        = s ? s->store : nullptr;
  } else if (nullptr == store && nullptr != base_) {
    store = base_->store();
  }
  const auto spans = contentOf(ref);
  if (nullptr == store || spans.empty() ||
      std::ranges::any_of(spans, [](const xanadu::PrimediaSpan &span) {
        return xanadu::scratchScroll == span.scroll;
      })) {
    return std::nullopt;
  }
  return QuotedContent{.store = store, .spans = {spans.begin(), spans.end()}};
}

CellRef ArenaManifold::makeCell(const std::string_view text) {
  // intern() first: it may reallocate the scratch buffer, and doing it before
  // the slot exists keeps the two steps independent.
  const auto span = intern(text);
  return mintSlot(xanadu::ValueKind::None, 0,
                  std::span<const xanadu::PrimediaSpan>{&span, 1});
}

CellRef ArenaManifold::makeScalarCell(const double value) {
  // Bits only: no rendering, no scratch byte. R6's other half is promote()'s
  // job, and an evaluation that only computes never pays for it. See §5.5.
  return mintSlot(xanadu::ValueKind::Double, xanadu::canonicalDoubleBits(value),
                  {});
}

CellRef ArenaManifold::makeScalarCell(const bool value) {
  return mintSlot(xanadu::ValueKind::Bool, value ? 1 : 0, {});
}

CellRef ArenaManifold::makeScalarCell(const std::int64_t value) {
  return mintSlot(xanadu::ValueKind::Int64, std::bit_cast<std::uint64_t>(value),
                  {});
}

void ArenaManifold::setContentAt(
    const std::uint32_t dense,
    const std::span<const xanadu::PrimediaSpan> spans) {
  if (spans.size() > maxCellContentSpanCount) {
    throw std::length_error("ArenaManifold content run exceeds CellSlot");
  }

  auto &cell = slots_[dense];
  liveContent_ -= cell.spanCount;

  const std::size_t offset = cell.spanOffset;
  const bool atTail        = offset + cell.spanCount == content_.size();
  if (atTail && spans.size() <= cell.spanCount) {
    content_.resize(offset + spans.size());
  } else if (!atTail || spans.size() > cell.spanCount) {
    if (content_.capacity() < content_.size() + spans.size()) {
      content_.reserve(
          std::max(content_.size() * 2, content_.size() + spans.size()));
    }
    cell.spanOffset = static_cast<std::uint32_t>(content_.size());
    content_.resize(content_.size() + spans.size());
  }
  std::ranges::copy(spans, content_.begin() + cell.spanOffset);
  cell.spanCount = static_cast<std::uint16_t>(spans.size());
  liveContent_ += cell.spanCount;

  if (0 == outstandingMarks_ &&
      content_.size() > 2 * liveContent_ + compactionSlack) {
    zigzag::expectWritten(compact());
  }
}

ArenaResult
ArenaManifold::setContent(const CellRef cell,
                          const std::span<const xanadu::PrimediaSpan> spans) {
  if (!contains(cell)) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  if (spans.size() > maxCellContentSpanCount) {
    return std::unexpected{ArenaRefusal::ContentRunTooLong};
  }

  const auto dense = shadow(cell);
  if (!dense.has_value()) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  trail(*dense);
  setContentAt(*dense, spans);
  return this;
}

ArenaResult ArenaManifold::setValueBits(const CellRef cell,
                                        const xanadu::ValueKind kind,
                                        const std::uint64_t bits) {
  const auto dense = shadow(cell);
  if (!dense.has_value()) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  trail(*dense);
  slots_[*dense].valueKind = static_cast<std::uint8_t>(kind);
  slots_[*dense].valueBits = bits;
  return this;
}

ArenaResult ArenaManifold::link(const CellRef from, const DimRef dim,
                                const DimVector dir,
                                const std::optional<CellRef> to) {
  // Resolve visible occupants first, then preflight every affected row before
  // applyLinkWrites() creates any copy-on-write shadows.
  if (!contains(from)) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  if (!contains(dim)) {
    return std::unexpected{ArenaRefusal::UnknownDimension};
  }
  const CellRef toCell = to.value_or(noCell);
  if (to.has_value() && !contains(*to)) {
    return std::unexpected{ArenaRefusal::UnknownTarget};
  }
  const auto needsForeignResolution = [&](const CellRef ref,
                                          const DimVector side) {
    const auto dense = denseOf(ref);
    return dense.has_value() && isProxy(ref) &&
           !proxyShadowedEdges_.contains(
               ShadowEdgeKey{.dense = *dense, .dim = dim, .dir = side});
  };
  const auto visibleFrom = linked(from, dim, dir);
  const auto visibleTo = to.has_value() ? linked(*to, dim, -dir) : std::nullopt;
  const bool needsSavepoint =
      needsForeignResolution(from, dir) ||
      (to.has_value() && needsForeignResolution(*to, -dir)) ||
      (visibleFrom.has_value() && needsForeignResolution(*visibleFrom, -dir)) ||
      (visibleTo.has_value() && needsForeignResolution(*visibleTo, dir));
  const std::optional<Mark> savepoint =
      needsSavepoint ? std::optional<Mark>{mark()} : std::nullopt;

  try {
    // Resolve visible old occupants before shadowing or writing. A proxy may
    // inherit either end from its foreign manifold, and linkedMinting gives
    // that occupant its canonical arena identity.
    const auto displacedUs = linkedMinting(from, dim, dir);
    const auto displacedIt =
        to.has_value() ? linkedMinting(*to, dim, -dir) : std::nullopt;

    if (displacedUs && displacedUs != toCell) {
      const auto inverse = linkedMinting(*displacedUs, dim, -dir);
      if (inverse != from) {
        throw std::logic_error("arena link has a mismatched visible inverse");
      }
    }
    if (to.has_value() && displacedIt && displacedIt != from) {
      const auto inverse = linkedMinting(*displacedIt, dim, dir);
      if (inverse != *to) {
        throw std::logic_error("arena link has a mismatched visible inverse");
      }
    }

    std::array<SideWrite, 4> writes{};
    std::size_t writeCount = 0;
    auto add               = [&](const CellRef cell, const DimVector side,
                   const std::optional<CellRef> neighbor) {
      writes[writeCount++] =
          SideWrite{.cell = cell, .dir = side, .neighbor = neighbor};
    };

    // A link is one edge two cells share, so setting it breaks whatever each
    // end held. Plan all clears before either new reciprocal end is written.
    if (displacedUs && displacedUs != toCell) {
      add(*displacedUs, -dir, std::nullopt);
    }
    if (displacedIt && displacedIt != from) {
      add(*displacedIt, dir, std::nullopt);
    }
    if (to.has_value()) {
      writes[writeCount++] =
          SideWrite{.cell = *to, .dir = -dir, .neighbor = from};
    }
    writes[writeCount++] = SideWrite{.cell = from, .dir = dir, .neighbor = to};
    applyLinkWrites(dim, std::span{writes}.first(writeCount));
  } catch (...) {
    if (savepoint.has_value()) {
      release(*savepoint);
    }
    throw;
  }

  if (savepoint.has_value()) {
    discard(*savepoint);
  }
  return this;
}

bool ArenaManifold::rankContains(const CellRef anchor, const CellRef cell,
                                 const DimRef dim) const noexcept {
  if (noCell == anchor || noCell == cell || noCell == dim) {
    return false;
  }
  if (anchor == cell) {
    return true;
  }
  const auto bound = traversalBound();
  // Traverse POSward
  {
    CellRef cur       = anchor;
    std::size_t limit = bound;
    while (limit-- > 0) {
      const auto next = linked(cur, dim, DimVector::POS);
      if (!next.has_value() || *next == cur || *next == anchor) {
        break;
      }
      if (*next == cell) {
        return true;
      }
      cur = *next;
    }
  }
  // Traverse NEGward
  {
    CellRef cur       = anchor;
    std::size_t limit = bound;
    while (limit-- > 0) {
      const auto prev = linked(cur, dim, DimVector::NEG);
      if (!prev.has_value() || *prev == cur || *prev == anchor) {
        break;
      }
      if (*prev == cell) {
        return true;
      }
      cur = *prev;
    }
  }
  return false;
}

ArenaManifold *ArenaManifold::insertIntoRank(const CellRef cell,
                                             const CellRef anchor,
                                             const DimRef dim,
                                             const DimVector dir) {
  if (noCell == cell || noCell == anchor || noCell == dim) {
    return this;
  }
  if (rankContains(anchor, cell, dim)) {
    return this;
  }
  const auto displaced = linked(anchor, dim, dir);
  std::ignore          = link(anchor, dim, dir, cell);
  if (displaced.has_value()) {
    std::ignore = link(cell, dim, dir, *displaced);
  }
  return this;
}

ArenaResult ArenaManifold::linkChain(const std::span<const CellRef> cells,
                                     const DimRef dim, const DimVector dir) {
  if (noCell == dim) {
    return this;
  }
  // Chain consecutive non-absent cells. Each link() writes both reciprocal
  // ends (RL1); the first refusal stops and is returned, having changed only
  // what the successful links before it did -- the caller releases its mark on
  // a refusal exactly as it would for a single link().
  CellRef prev = noCell;
  for (const CellRef cur : cells) {
    if (noCell == cur) {
      continue;
    }
    if (noCell != prev) {
      if (auto res = link(prev, dim, dir, cur); !res.has_value()) {
        return res;
      }
    }
    prev = cur;
  }
  return this;
}

std::optional<std::uint32_t> ArenaManifold::shadow(const CellRef ref) {
  if (const auto dense = denseOf(ref); dense.has_value()) {
    return dense;
  }
  if (nullptr == base_) {
    return std::nullopt;
  }
  const auto theirs = base_->slot(ref);
  if (!theirs) {
    return std::nullopt;
  }

  // Copy the whole cell rather than recording an override. Every later read is
  // then answered from one place, so no read has to merge an override with
  // what it overrides -- which is the kind of thing that is correct until the
  // day two overrides disagree about a run.
  const auto dense = static_cast<std::uint32_t>(slots_.size());
  CellSlot copy    = *theirs;
  copy.spanOffset  = static_cast<std::uint32_t>(content_.size());
  copy.spanCount   = 0;
  copy.linkOffset  = static_cast<std::uint32_t>(links_.size());
  copy.linkCount   = 0;
  slots_.push_back(copy);

  // birthOp comes across untouched, so the shadow's name is the base cell's
  // ref and not refOf(dense): an overlaid cell is the *same* cell.
  const auto spans = base_->contentOf(ref);
  if (!spans.empty()) {
    setContentAt(dense, spans);
  }
  for (const auto &edge : base_->dimensionsOf(ref)) {
    links_.push_back(edge);
    slots_[dense].linkCount++;
    liveLinks_++;
  }

  overlay_.emplace(ref, dense);
  shadowOrder_.push_back(ref);
  return dense;
}

void ArenaManifold::trail(const std::uint32_t dense) {
  if (0 == outstandingMarks_) {
    return;
  }
  const auto &floor = floors_.back();
  // §5.3's condition: a cell minted under this mark is undone by the truncation
  // that removes it, so trailing it would be recording an undo for something
  // that will not exist.
  if (dense >= floor.cells) {
    return;
  }

  auto &cell = slots_[dense];
  // Already copied out under this mark -- a nonempty run above the floor is
  // reclaimed by truncation, and its header already has an undo entry. Empty
  // runs can sit exactly at both floors without any copy, so they still need a
  // saved header (notably for scalar value bits).
  if ((cell.linkCount > 0 || cell.spanCount > 0) &&
      cell.linkOffset >= floor.links && cell.spanOffset >= floor.content) {
    return;
  }

  trail_.push_back(TrailEntry{.dense = dense, .saved = cell});

  // **Saving the header is not enough on its own.** A DimLink lives in the
  // shared arena, so overwriting one in a run that sits *below* the mark would
  // survive truncation -- release() would restore an offset pointing at a run
  // whose contents had already been edited. So trailing a cell also copies its
  // runs to the arenas' tails, above the mark: the originals below are left
  // pristine, every later write lands above, and restoring the header is
  // enough because the header is all that points at the copy.
  //
  // Copy-on-write per cell per choice point, in other words, and the WAM's
  // conditional-trailing rule is what bounds how often it happens: a variable
  // minted for this clause activation is never copied, which is the common
  // case in head unification.
  if (cell.linkCount > 0) {
    const std::size_t offset = cell.linkOffset;
    const std::size_t count  = cell.linkCount;
    if (links_.capacity() < links_.size() + count) {
      links_.reserve(std::max(links_.size() * 2, links_.size() + count));
    }
    const auto relocated = links_.size();
    for (std::size_t i = 0; i < count; i++) {
      links_.push_back(links_[offset + i]);
    }
    cell.linkOffset = static_cast<std::uint32_t>(relocated);
  } else {
    cell.linkOffset = static_cast<std::uint32_t>(links_.size());
  }

  if (cell.spanCount > 0) {
    const std::size_t offset = cell.spanOffset;
    const std::size_t count  = cell.spanCount;
    if (content_.capacity() < content_.size() + count) {
      content_.reserve(std::max(content_.size() * 2, content_.size() + count));
    }
    const auto relocated = content_.size();
    for (std::size_t i = 0; i < count; i++) {
      content_.push_back(content_[offset + i]);
    }
    cell.spanOffset = static_cast<std::uint32_t>(relocated);
  } else {
    cell.spanOffset = static_cast<std::uint32_t>(content_.size());
  }
}

Mark ArenaManifold::mark() {
  const Mark taken{
      .cellCount   = static_cast<std::uint32_t>(slots_.size()),
      .linkSize    = static_cast<std::uint32_t>(links_.size()),
      .contentSize = static_cast<std::uint32_t>(content_.size()),
      .scratchSize = static_cast<std::uint32_t>(scratch_.size()),
      .trailSize   = static_cast<std::uint32_t>(trail_.size()),
      .liveLinks   = static_cast<std::uint32_t>(liveLinks_),
      .liveContent = static_cast<std::uint32_t>(liveContent_),
      .shadowCount = static_cast<std::uint32_t>(shadowOrder_.size()),
      .proxyCount  = static_cast<std::uint32_t>(proxyOrder_.size()),
      .quoteOccurrenceCount =
          static_cast<std::uint32_t>(quoteOccurrenceOrder_.size()),
      .proxyShadowedEdgeCount =
          static_cast<std::uint32_t>(proxyShadowedEdgeOrder_.size()),
      .quoteSpaceCount = static_cast<std::uint32_t>(quoteSpaceOrder_.size()),
      .dimensionCount  = static_cast<std::uint32_t>(dimensionOrder_.size()),
      .spaceCount      = static_cast<std::uint32_t>(spaces_.size()),
  };
  floors_.push_back(Floors{.cells   = taken.cellCount,
                           .links   = taken.linkSize,
                           .content = taken.contentSize});
  outstandingMarks_++;
  return taken;
}

ArenaManifold *ArenaManifold::discard(const Mark &m) noexcept {
  std::ignore = m;
  if (0 == outstandingMarks_) {
    return this;
  }
  outstandingMarks_--;
  floors_.pop_back();
  // An enclosing choice point still needs this mark's undo records. Once
  // the last mark is gone, no release can use them and committed turns must
  // not retain an ever-growing trail.
  if (0 == outstandingMarks_) {
    trail_.clear();
    dimensionOrder_.clear();
  }
  return this;
}

ArenaManifold *ArenaManifold::release(const Mark &m) noexcept {
  for (std::size_t i = dimensionOrder_.size(); i > m.dimensionCount; i--) {
    const auto &entry = dimensionOrder_[i - 1];
    const auto found  = arenaDims_.find(entry.name);
    if (found != arenaDims_.end() && found->second == entry.dim) {
      arenaDims_.erase(found);
    }
  }
  dimensionOrder_.resize(m.dimensionCount);

  for (std::size_t i = quoteSpaceOrder_.size(); i > m.quoteSpaceCount; i--) {
    quoteSpace_.erase(quoteSpaceOrder_[i - 1]);
  }
  quoteSpaceOrder_.resize(m.quoteSpaceCount);
  // Truncate proxy shadowed edges
  for (std::size_t i = proxyShadowedEdgeOrder_.size();
       i > m.proxyShadowedEdgeCount; i--) {
    proxyShadowedEdges_.erase(proxyShadowedEdgeOrder_[i - 1]);
  }
  proxyShadowedEdgeOrder_.resize(m.proxyShadowedEdgeCount);

  // Truncate quote occurrences
  for (std::size_t i = quoteOccurrenceOrder_.size(); i > m.quoteOccurrenceCount;
       i--) {
    const CellRef occ = quoteOccurrenceOrder_[i - 1];
    const auto canIt  = occurrenceToCanonical_.find(occ);
    const auto viewIt = occurrenceToView_.find(occ);
    if (canIt != occurrenceToCanonical_.end() &&
        viewIt != occurrenceToView_.end()) {
      viewCanonicalToOccurrence_.erase(
          std::make_pair(viewIt->second, canIt->second));
    }
    if (canIt != occurrenceToCanonical_.end()) {
      occurrenceToCanonical_.erase(canIt);
    }
    if (viewIt != occurrenceToView_.end()) {
      occurrenceToView_.erase(viewIt);
    }
  }
  quoteOccurrenceOrder_.resize(m.quoteOccurrenceCount);

  // Truncate proxies
  for (std::size_t i = proxyOrder_.size(); i > m.proxyCount; i--) {
    const auto &pe = proxyOrder_[i - 1];
    if (pe.space > 0 && pe.space <= spaceProxies_.size()) {
      spaceProxies_[pe.space - 1].erase(pe.foreignIndex);
    }
  }
  proxyOrder_.resize(m.proxyCount);

  // Detach the spaces attached under the mark, newest first, now that their
  // proxies are gone. Their store cells and rank links are cell and link
  // writes, undone below with everything else; what is left to hand back is
  // the base and store an attach may have adopted and the stores-rank tail.
  for (std::size_t i = spaces_.size(); i > m.spaceCount; i--) {
    const auto &adoption = spaceAdoptions_[i - 1];
    if (adoption.base) base_ = nullptr;
    if (adoption.store) store_ = nullptr;
  }
  if (spaces_.size() > m.spaceCount) {
    const auto keep = static_cast<std::ptrdiff_t>(m.spaceCount);
    spaces_.erase(spaces_.begin() + keep, spaces_.end());
    spaceAdoptions_.erase(spaceAdoptions_.begin() + keep,
                          spaceAdoptions_.end());
    spaceProxies_.erase(spaceProxies_.begin() + keep, spaceProxies_.end());
    spaceStoreRefsTail_.erase(spaceStoreRefsTail_.begin() + keep,
                              spaceStoreRefsTail_.end());
    spaceStoreRefsDimensions_.erase(spaceStoreRefsDimensions_.begin() + keep,
                                    spaceStoreRefsDimensions_.end());
    // Dimension bindings that named a detached space would dangle.
    for (auto it = boundDimensions_.begin(); it != boundDimensions_.end();) {
      auto &members     = it->second.members;
      const bool before = !members.empty();
      std::erase_if(members, [&](const BoundDimensionMember &b) {
        return b.space > m.spaceCount;
      });
      // Only a set this sweep emptied goes; one that was empty before is not
      // this mark's to undo.
      it = before && members.empty() ? boundDimensions_.erase(it)
                                     : std::next(it);
    }
    if (spaces_.empty())
      storesRankTail_.reset();
    else
      storesRankTail_ = spaces_.back().storeCell;
  }

  // Reverse, so that a cell written more than once under this mark ends up
  // holding the value it had when the mark was taken rather than the one it
  // held in between. Duplicates in the trail are what make this necessary and
  // are also why not deduplicating is safe.
  for (std::size_t i = trail_.size(); i > m.trailSize; i--) {
    const auto &entry   = trail_[i - 1];
    slots_[entry.dense] = entry.saved;
  }
  trail_.resize(m.trailSize);

  // Dropping a shadow is the undo, for an overlaid cell: the slot goes and the
  // reads fall through to the base again, which is where the cell's unmodified
  // state has been sitting all along. Nothing had to be saved to make that so.
  for (std::size_t i = shadowOrder_.size(); i > m.shadowCount; i--) {
    overlay_.erase(shadowOrder_[i - 1]);
  }
  shadowOrder_.resize(m.shadowCount);

  slots_.resize(m.cellCount);
  links_.resize(m.linkSize);
  content_.resize(m.contentSize);
  scratch_.resize(m.scratchSize);
  liveLinks_   = m.liveLinks;
  liveContent_ = m.liveContent;

  for (auto &dimension : spaceStoreRefsDimensions_) {
    if (dimension && isEphemeral(*dimension) && !contains(*dimension)) {
      dimension.reset();
    }
  }

  for (std::size_t s = 0; s < spaces_.size(); ++s) {
    const auto dimStoreRefs =
        s < std::numeric_limits<std::uint32_t>::max()
            ? storeRefsDimension(static_cast<std::uint32_t>(s + 1))
            : std::nullopt;
    if (dimStoreRefs.has_value()) {
      CellRef cur       = *spaces_[s].storeCell;
      std::size_t limit = traversalBound();
      while (cur != noCell && limit-- > 0) {
        const auto nxt = linked(cur, *dimStoreRefs, DimVector::POS);
        if (!nxt) {
          break;
        }
        cur = *nxt;
      }
      if (s < spaceStoreRefsTail_.size()) {
        spaceStoreRefsTail_[s] = cur;
      }
    } else if (s < spaceStoreRefsTail_.size() && spaces_[s].storeCell) {
      spaceStoreRefsTail_[s] = *spaces_[s].storeCell;
    }
  }

  if (outstandingMarks_ > 0) {
    outstandingMarks_--;
    floors_.pop_back();
    if (outstandingMarks_ == 0) {
      dimensionOrder_.clear();
    }
  }
  return this;
}

ArenaResult ArenaManifold::compact() {
  if (outstandingMarks_ > 0) {
    return std::unexpected{ArenaRefusal::MarksOutstanding};
  }

  std::vector<xanadu::PrimediaSpan> tightContent;
  tightContent.reserve(liveContent_);
  for (auto &cell : slots_) {
    const std::size_t offset = cell.spanOffset;
    cell.spanOffset          = static_cast<std::uint32_t>(tightContent.size());
    for (std::uint16_t i = 0; i < cell.spanCount; i++) {
      tightContent.push_back(content_[offset + i]);
    }
  }
  content_.swap(tightContent);
  liveContent_ = content_.size();

  std::vector<DimLink> tight;
  tight.reserve(liveLinks_);
  for (auto &cell : slots_) {
    const std::size_t offset = cell.linkOffset;
    cell.linkOffset          = static_cast<std::uint32_t>(tight.size());
    for (std::uint16_t i = 0; i < cell.linkCount; i++) {
      tight.push_back(links_[offset + i]);
    }
  }
  links_.swap(tight);
  liveLinks_ = links_.size();
  return this;
}

std::optional<Promoted> promote(xanadu::Store &store,
                                const xanadu::MicroversionId &parent,
                                const ArenaManifold &from, const CellRef root,
                                const PromotionBudget budget,
                                const std::span<const CellRef> extraRoots) {
  if (!from.contains(root) || from.isProvenanceCell(root)) {
    return std::nullopt;
  }

  // The reachable subgraph, in discovery order. Reachability from the answer is
  // what makes "promote the answer, not the search" a graph walk: a failed
  // branch's cells are not reachable from a cell the answer names.
  std::vector<CellRef> order{root};
  std::vector<CellRef> frontier{root};
  std::unordered_set<CellRef> seen{root};
  std::unordered_set<CellRef> queued{root};
  // extraRoots seed the walk with cells the caller knows hold writes the
  // root walk cannot reach — a shadowed base cell behind an unshadowed
  // one. Only held cells qualify: expanding an unshadowed base would
  // recurse into the document the terminal condition exists to avoid.
  for (const CellRef extra : extraRoots) {
    if (!from.contains(extra) || from.isProvenanceCell(extra) ||
        !from.holdsOwn(extra)) {
      continue;
    }
    if (seen.insert(extra).second) order.push_back(extra);
    if (queued.insert(extra).second) frontier.push_back(extra);
  }
  for (std::size_t i = 0; i < frontier.size(); i++) {
    const CellRef current = frontier[i];
    // A base cell only read through the overlay is an existing endpoint, not
    // a request to copy the document reachable beyond it. A dimension names
    // an edge; its own ranks are likewise outside this answer unless a link
    // also reaches it as a cell. A proxy is an external endpoint; its internal
    // store-refs filing edges must not be followed.
    if ((current != root && !isEphemeral(current) && !from.holdsOwn(current)) ||
        from.isProxy(current)) {
      continue;
    }
    if (xanadu::isCellRelocationHandle(from, current)) {
      const auto target = from.handleTarget(current);
      if (!target || !from.contains(*target)) return std::nullopt;
      if (seen.insert(*target).second) order.push_back(*target);
      if (queued.insert(*target).second) frontier.push_back(*target);
    }
    for (const auto &edge : from.dimensionsOf(current)) {
      if (from.textOf(edge.dim) == "d.dims") {
        continue;
      }
      auto discover = [&](const CellRef next, const bool expand) {
        if (noCell == next || !from.contains(next) ||
            from.isProvenanceCell(next)) {
          return;
        }
        if (seen.insert(next).second) {
          order.push_back(next);
        }
        if (expand && queued.insert(next).second) {
          frontier.push_back(next);
        }
      };
      discover(edge.dim, false);
      discover(edge.pos, true);
      discover(edge.neg, true);
    }
    if (order.size() > budget.maxOps) {
      return std::nullopt;
    }
  }

  // An arena cell has no operation yet. Explicit cell relocations may name
  // its eventual birth. Cycles acquire births before their values are set,
  // so no operation needs a forward reference to an operation not yet minted.
  std::vector<CellRef> mintOrder;
  std::vector<CellRef> deferred;
  std::vector<CellRef> cyclicHandles;
  std::unordered_set<CellRef> ready;
  for (const auto cell : order) {
    if (from.valueKindOf(cell) == xanadu::ValueKind::OpHandle &&
        isEphemeral(cell) && !from.isProxy(cell)) {
      const auto target = from.handleTarget(cell);
      if (!target) return std::nullopt;
      if (isEphemeral(*target)) {
        if (!xanadu::isCellRelocationHandle(from, cell) ||
            !seen.contains(*target))
          return std::nullopt;
        deferred.push_back(cell);
        continue;
      }
      if (!store.getCompactOp(*target)) return std::nullopt;
    }
    mintOrder.push_back(cell);
    ready.insert(cell);
  }
  while (!deferred.empty()) {
    const auto before = deferred.size();
    std::erase_if(deferred, [&](CellRef cell) {
      if (!ready.contains(*from.handleTarget(cell))) return false;
      mintOrder.push_back(cell);
      ready.insert(cell);
      return true;
    });
    if (deferred.size() == before) {
      cyclicHandles = std::move(deferred);
      mintOrder.insert(mintOrder.end(), cyclicHandles.begin(),
                       cyclicHandles.end());
      break;
    }
  }
  if (order.size() > budget.maxOps ||
      cyclicHandles.size() > budget.maxOps - order.size())
    return std::nullopt;
  const std::unordered_set<CellRef> delayed(cyclicHandles.begin(),
                                            cyclicHandles.end());

  // A quotation of another store's bytes is written as a transclusion of them,
  // so each of its spans must be nameable here; checked before anything is
  // written, for the same reason as the budget.
  for (const CellRef arena : mintOrder) {
    if (from.valueKindOf(arena) == xanadu::ValueKind::ForeignOpHandle &&
        !xanadu::foreignOpTarget(from, arena)) {
      return std::nullopt;
    }
    if (!isEphemeral(arena) || from.isProxy(arena)) {
      continue;
    }
    if (const auto quoted = from.quotedContent(arena);
        quoted && quoted->store != &store &&
        !std::ranges::all_of(
            quoted->spans, [&](const xanadu::PrimediaSpan &span) {
              return xanadu::canCarry(*quoted->store, store, span);
            })) {
      return std::nullopt;
    }
  }

  // Cross-store guard (delta-completeness §4.5): when a caller opts into
  // committing shadowed-cell diffs, a base store that differs from the
  // destination is a wrong-cell write -- setLink resolves refs in the
  // destination index space, so an identity-mapped persistent ref would graft
  // onto an unrelated cell. Refuse outright: a refusal writes nothing. The
  // non-opted-in additive path carries the same latent hazard for re-points
  // today; rather than change those callers (export*ToStore relies on it) we
  // warn and stay bit-identical. mesh_jobs is exempt: there base==dest.
  if (nullptr != from.base() && nullptr != from.base()->store() &&
      from.base()->store() != &store) {
    if (budget.commitShadowed) {
      GLEDITOR_LOG_WARN("zigzag.arena",
                        "promote: refusing -- base manifold's store differs "
                        "from the destination store");
      return std::nullopt;
    }
    GLEDITOR_LOG_WARN("zigzag.arena",
                      "promote: base store != destination (latent wrong-cell "
                      "hazard for repoints)");
  }

  // Nothing is written until the walk has finished and the budget has held, so
  // a refusal leaves the store exactly as it was.
  std::unordered_map<CellRef, CellRef> real;
  real.reserve(order.size());
  Promoted out{.version = parent, .cells = {}};
  out.cells.reserve(order.size());

  for (const CellRef arena : mintOrder) {
    // A cell the overlay was only reading through already has a name in this
    // document, so promotion maps it to itself and mints nothing. Only what
    // the evaluation invented is new.
    if (!isEphemeral(arena)) {
      real.emplace(arena, arena);
      continue;
    }
    if (from.isProxy(arena)) {
      const auto foreign = from.foreignOf(arena);
      if (foreign) {
        const auto s = from.spaceAt(foreign->space);
        if (s && s->store) {
          xanadu::MicroversionId produces;
          std::string globalKey;
          if (s->sealedAs != nullptr) {
            const auto globalOp =
                opRefOf(*s->store, foreign->index, *s->sealedAs);
            if (globalOp) {
              globalKey = globalOp->scroll;
              produces  = globalOp->produces;
            }
          }
          if (produces.isZero()) {
            produces = s->store->segmentedOps().idOf(foreign->index).value();
          }
          if (globalKey.empty()) {
            globalKey = !s->label.empty()
                            ? s->label
                            : ("store_" + std::to_string(foreign->space));
          }
          auto scrollIdOpt = store.scrollRegistry().scrollIdForKey(globalKey);
          if (!scrollIdOpt.has_value()) {
            out.version = store.registerScroll(out.version, globalKey);
            scrollIdOpt = store.scrollRegistry().scrollIdForKey(globalKey);
          }
          if (scrollIdOpt.has_value()) {
            const xanadu::ExternOpRef targetRef{.scroll   = *scrollIdOpt,
                                                .produces = produces};
            out.version = store.makeExternRef(out.version, targetRef);
            const auto placeholder = store.placeholderForExtern(targetRef);
            if (placeholder.has_value()) {
              real.emplace(arena, *placeholder);
              out.cells.push_back(*placeholder);
              continue;
            }
          }
        }
      }
    }
    const auto content = from.contentOf(arena);
    std::vector<xanadu::PrimediaSpan> spans(content.begin(), content.end());
    if (const auto quoted = from.quotedContent(arena);
        quoted && quoted->store != &store) {
      for (auto &span : spans) {
        if (const auto carried =
                xanadu::carrySpan(*quoted->store, store, span)) {
          span = *carried;
        }
      }
    }
    const auto kind = from.valueKindOf(arena);

    xanadu::MicroversionId minted = out.version;
    if (xanadu::ValueKind::Double == kind) {
      // R6's deferred half, written here: bits *and* a rendering.
      minted =
          store.makeScalarCell(out.version, from.asDouble(arena).value_or(0.0));
    } else if (xanadu::ValueKind::Bool == kind) {
      minted =
          store.makeScalarCell(out.version, from.asBool(arena).value_or(false));
    } else if (xanadu::ValueKind::Int64 == kind) {
      minted =
          store.makeScalarCell(out.version, from.asInt64(arena).value_or(0));
    } else if (xanadu::ValueKind::OpHandle == kind && delayed.contains(arena)) {
      // Cycle members get real birth operations before any value names them.
      // SetValue below then creates the cycle without a forward reference or
      // any edit of an immutable CompactOpNode.
      if (spans.empty())
        minted = store.makeCell(out.version, std::string_view{});
      else if (spans.front().scroll == xanadu::scratchScroll)
        minted = store.makeCell(out.version, from.scratchTextOf(spans.front()));
      else
        minted = store.makeCell(out.version, spans.front());
    } else if (xanadu::ValueKind::OpHandle == kind) {
      const auto target = *from.handleTarget(arena);
      minted            = store.makeOpHandle(
          out.version, isEphemeral(target) ? real.at(target) : target,
          from.textOf(arena));
    } else if (xanadu::ValueKind::ForeignOpHandle == kind) {
      const auto target = xanadu::foreignOpTarget(from, arena);
      minted            = store.makeForeignOpHandle(out.version, *target);
      spans.clear();
    } else if (spans.empty()) {
      minted = store.makeCell(out.version, std::string_view{});
    } else if (xanadu::scratchScroll == spans.front().scroll) {
      // A scratch span acquires a permanent address here, and only here.
      minted = store.makeCell(out.version, from.scratchTextOf(spans.front()));
    } else {
      minted = store.makeCell(out.version, spans.front());
    }
    out.version            = minted;
    const CellRef realCell = store.cellRefOf(minted).value();
    real.emplace(arena, realCell);
    out.cells.push_back(realCell);

    // Every span after the first is a splice, since one operation carries one
    // span until U3.4's CompactBinaryV4.
    std::uint64_t at = spans.empty() ? 0 : spans.front().length;
    for (std::size_t i = 1; i < spans.size(); i++) {
      const auto &span = spans[i];
      if (xanadu::scratchScroll == span.scroll) {
        out.version = store.spliceCell(out.version, realCell, at, 0,
                                       from.scratchTextOf(span));
      } else {
        out.version = store.spliceCellSpan(out.version, realCell, at, 0, span);
      }
      at += span.length;
    }
  }

  if (!cyclicHandles.empty()) {
    auto current = store.rebuildManifold(out.version);
    for (const auto handle : cyclicHandles) {
      const auto born    = real.at(handle);
      const auto content = current.contentOf(born);
      const std::vector<xanadu::PrimediaSpan> spans(content.begin(),
                                                    content.end());
      const auto target = *from.handleTarget(handle);
      out.version =
          store.setValue(out.version, born,
                         spans.empty() ? xanadu::PrimediaSpan{} : spans.front(),
                         xanadu::ValueKind::OpHandle,
                         isEphemeral(target) ? real.at(target) : target);
      std::uint64_t offset = spans.empty() ? 0 : spans.front().length;
      for (std::size_t i = 1; i < spans.size(); ++i) {
        out.version =
            store.spliceCellSpan(out.version, born, offset, 0, spans[i]);
        offset += spans[i].length;
      }
    }
  }
  out.cells.clear();
  out.arena.clear();
  for (const auto cell : order) {
    if (isEphemeral(cell)) {
      out.arena.push_back(cell);
      out.cells.push_back(real.at(cell));
    }
  }

  // Links last: both ends have to exist, and every ref is translated rather
  // than trusted -- an untranslated arena ref would be refused by setLink()
  // because it carries ephemeralBit, which is R8's boundary doing the checking
  // for us.
  // Folded once and advanced, not rebuilt per link: setLink() wants a manifold
  // so it can read a cell's lastOp instead of walking the ancestral path for
  // it, and rebuilding one per link would make promoting a large answer
  // quadratic -- which is the exact trap store.hpp's `known` parameter exists
  // to let a caller avoid.
  auto known = store.rebuildManifold(out.version);
  for (const CellRef arena : order) {
    if (!from.holdsOwn(arena) || from.isProxy(arena)) {
      continue;
    }
    // Per-cell emission policy. Only persistent shadowed cells (an owned slot
    // under a non-ephemeral name) consult commitShadowed -- mints, proxies and
    // untouched base fall-throughs stay additive. Suppress wins outright.
    const bool persistentShadow = !isEphemeral(arena);
    const ShadowCommit policy   = persistentShadow && budget.commitShadowed
                                      ? (*budget.commitShadowed)(arena)
                                      : ShadowCommit::Additive;
    if (policyHas(policy, ShadowCommit::Suppress)) {
      continue;
    }
    const bool mayClear = policyHas(policy, ShadowCommit::Clears);
    for (const auto &edge : from.dimensionsOf(arena)) {
      // Net-state diff against the base: emit only edges whose posward target
      // changed. A shadow is a whole-cell copy, so shadow-vs-base is complete.
      // For a persistent shadow the base lookup sees the original edge; for a
      // minted/ephemeral cell (or a base-less arena) there is no base edge.
      const CellRef basePos =
          persistentShadow && nullptr != from.base()
              ? from.base()
                    ->linked(arena, edge.dim, DimVector::POS)
                    .value_or(noCell)
              : noCell;
      if (edge.pos == basePos) {
        continue; // unchanged -- the additive dedup case
      }
      // A changed edge: add/repoint always (unless Suppress, handled above);
      // a dropped edge (pos == noCell) emits a durable clear only under Clears.
      const bool isClear = (noCell == edge.pos);
      if (isClear && !mayClear) {
        continue;
      }
      const auto dim = real.find(edge.dim);
      if (dim == real.end()) {
        continue;
      }
      if (isClear) {
        out.version = store.setLink(out.version, real.at(arena), dim->second,
                                    false, std::nullopt);
      } else {
        const auto to = real.find(edge.pos);
        if (to == real.end()) {
          continue;
        }
        out.version = store.setLink(out.version, real.at(arena), dim->second,
                                    false, to->second);
      }
      // The next setLink reads `known`, so a link it failed to fold would
      // make every later one validate against a stale view.
      if (const auto stepped = known.advance(store, out.version); !stepped) {
        GLEDITOR_LOG_WARN("zigzag.arena",
                          "promote: the fold refused a link it just minted "
                          "(kind {}, {})",
                          static_cast<int>(stepped.error().kind),
                          toString(stepped.error().refusal));
      }
    }
  }
  return out;
}

namespace {

struct PrefixRunRange {
  std::uint64_t begin{0};
  std::uint64_t end{0};
};

struct ArenaPrefixCodecLimits {
  static constexpr std::uint32_t maxSlots   = 1'000'000;
  static constexpr std::uint32_t maxLinks   = 4'000'000;
  static constexpr std::uint32_t maxContent = 4'000'000;
  static constexpr std::uint32_t maxShadows = 1'000'000;
  static constexpr std::uint32_t maxDims    = 100'000;
};

bool disjointRuns(std::vector<PrefixRunRange> &ranges) {
  std::ranges::sort(ranges, {}, &PrefixRunRange::begin);
  for (std::size_t i = 1; i < ranges.size(); i++) {
    if (ranges[i].begin < ranges[i - 1].end) {
      return false;
    }
  }
  return true;
}

bool prefixHasRepresentableSizes(
    const ArenaManifold::PrefixSnapshot &snapshot) noexcept {
  constexpr auto maxSize = std::numeric_limits<std::uint32_t>::max();
  return snapshot.slots.size() <= ephemeralBit &&
         snapshot.links.size() <= maxSize &&
         snapshot.content.size() <= maxSize &&
         snapshot.scratch.size() <= maxSize &&
         snapshot.shadowOrder.size() <= maxSize &&
         snapshot.arenaDims.size() <= maxSize &&
         std::ranges::all_of(snapshot.arenaDims, [&](const auto &entry) {
           return entry.first.size() <= maxSize;
         });
}

bool prefixWithinCodecLimits(
    const ArenaManifold::PrefixSnapshot &snapshot) noexcept {
  return snapshot.slots.size() <= ArenaPrefixCodecLimits::maxSlots &&
         snapshot.links.size() <= ArenaPrefixCodecLimits::maxLinks &&
         snapshot.content.size() <= ArenaPrefixCodecLimits::maxContent &&
         snapshot.shadowOrder.size() <= ArenaPrefixCodecLimits::maxShadows &&
         snapshot.arenaDims.size() <= ArenaPrefixCodecLimits::maxDims &&
         snapshot.scratch.size() <= std::numeric_limits<std::uint32_t>::max() &&
         std::ranges::all_of(snapshot.arenaDims, [&](const auto &entry) {
           return entry.first.size() <=
                  std::numeric_limits<std::uint32_t>::max();
         });
}

std::optional<std::uint32_t>
prefixWireSize(const ArenaManifold::PrefixSnapshot &snapshot) noexcept {
  std::uint64_t dimensionNameBytes = 0;
  for (const auto &[name, dim] : snapshot.arenaDims) {
    std::ignore = dim;
    if (name.size() >
        std::numeric_limits<std::uint64_t>::max() - dimensionNameBytes) {
      return std::nullopt;
    }
    dimensionNameBytes += name.size();
  }
  return detail::arenaPrefixWireSize(detail::ArenaPrefixWireCounts{
      .slots              = snapshot.slots.size(),
      .links              = snapshot.links.size(),
      .content            = snapshot.content.size(),
      .scratchBytes       = snapshot.scratch.size(),
      .shadows            = snapshot.shadowOrder.size(),
      .dimensions         = snapshot.arenaDims.size(),
      .dimensionNameBytes = dimensionNameBytes,
  });
}

bool prefixRefExists(const CellRef ref,
                     const ArenaManifold::PrefixSnapshot &snapshot,
                     const Manifold *base) noexcept {
  if (ref == noCell) {
    return true;
  }
  if (isEphemeral(ref)) {
    const auto dense = ref & ~ephemeralBit;
    return dense < snapshot.slots.size() &&
           snapshot.slots[dense].birthOp == ref;
  }
  return base != nullptr && base->contains(ref);
}

bool prefixRunsAreWellFormed(
    const ArenaManifold::PrefixSnapshot &snapshot, const Manifold *base,
    std::unordered_map<CellRef, std::uint32_t> &overlay) {
  std::vector<PrefixRunRange> linkRanges;
  std::vector<PrefixRunRange> contentRanges;
  linkRanges.reserve(snapshot.slots.size());
  contentRanges.reserve(snapshot.slots.size());

  std::uint64_t liveLinks   = 0;
  std::uint64_t liveContent = 0;
  for (std::size_t dense = 0; dense < snapshot.slots.size(); dense++) {
    const auto &slot              = snapshot.slots[dense];
    const std::uint64_t linkBegin = slot.linkOffset;
    const std::uint64_t spanBegin = slot.spanOffset;
    if (linkBegin > snapshot.links.size() ||
        slot.linkCount > snapshot.links.size() - linkBegin ||
        spanBegin > snapshot.content.size() ||
        slot.spanCount > snapshot.content.size() - spanBegin) {
      return false;
    }
    if (slot.linkCount > 0) {
      linkRanges.push_back(
          PrefixRunRange{linkBegin, linkBegin + slot.linkCount});
    }
    if (slot.spanCount > 0) {
      contentRanges.push_back(
          PrefixRunRange{spanBegin, spanBegin + slot.spanCount});
    }
    liveLinks += slot.linkCount;
    liveContent += slot.spanCount;

    if (isEphemeral(slot.birthOp)) {
      if (slot.birthOp !=
              ArenaManifold::refOf(static_cast<std::uint32_t>(dense)) ||
          slot.lastOp != slot.birthOp) {
        return false;
      }
    } else {
      const auto baseSlot = base ? base->slot(slot.birthOp) : SlotRef{};
      if (!baseSlot || baseSlot->birthOp != slot.birthOp ||
          baseSlot->lastOp != slot.lastOp ||
          !overlay.emplace(slot.birthOp, static_cast<std::uint32_t>(dense))
               .second) {
        return false;
      }
    }
  }
  if (!disjointRuns(linkRanges) || !disjointRuns(contentRanges) ||
      liveLinks != snapshot.mark.liveLinks ||
      liveContent != snapshot.mark.liveContent) {
    return false;
  }

  for (const auto &edge : snapshot.links) {
    if (edge.dim == noCell || !prefixRefExists(edge.dim, snapshot, base) ||
        !prefixRefExists(edge.pos, snapshot, base) ||
        !prefixRefExists(edge.neg, snapshot, base)) {
      return false;
    }
  }
  for (const auto &span : snapshot.content) {
    if (span.length > std::numeric_limits<std::uint64_t>::max() - span.start) {
      return false;
    }
    if (span.scroll == xanadu::scratchScroll &&
        (span.start > snapshot.scratch.size() ||
         span.length > snapshot.scratch.size() - span.start)) {
      return false;
    }
  }
  std::unordered_set<CellRef> shadowSet;
  shadowSet.reserve(snapshot.shadowOrder.size());
  for (const CellRef ref : snapshot.shadowOrder) {
    if (isEphemeral(ref) || !base || !base->contains(ref) ||
        !overlay.contains(ref) || !shadowSet.insert(ref).second) {
      return false;
    }
  }
  if (overlay.size() != shadowSet.size()) {
    return false;
  }
  for (const auto &[name, dim] : snapshot.arenaDims) {
    std::ignore = name;
    if (dim == noCell || !prefixRefExists(dim, snapshot, base)) {
      return false;
    }
  }
  return true;
}

} // namespace

bool ArenaManifold::hasUnsupportedUntrackedPrefixIdentity() const noexcept {
  return !spaces_.empty() || !spaceProxies_.empty() ||
         !spaceStoreRefsTail_.empty() || !spaceStoreRefsDimensions_.empty() ||
         !boundDimensions_.empty() || !provenanceCells_.empty() ||
         authorshipRoot_.has_value() || storesRankTail_.has_value();
}

std::expected<ArenaManifold::PrefixSnapshot, ArenaRefusal>
ArenaManifold::exportPrefix(const Mark &m) const {
  if (m.cellCount > slots_.size() || m.linkSize > links_.size() ||
      m.contentSize > content_.size() || m.scratchSize > scratch_.size() ||
      m.trailSize > trail_.size() || m.shadowCount > shadowOrder_.size() ||
      m.proxyCount > proxyOrder_.size() ||
      m.quoteOccurrenceCount > quoteOccurrenceOrder_.size() ||
      m.proxyShadowedEdgeCount > proxyShadowedEdgeOrder_.size() ||
      m.quoteSpaceCount > quoteSpaceOrder_.size() ||
      m.dimensionCount > dimensionOrder_.size() ||
      m.spaceCount > spaces_.size() || m.cellCount > ephemeralBit ||
      m.liveLinks > m.linkSize || m.liveContent > m.contentSize) {
    return std::unexpected{ArenaRefusal::MalformedPrefix};
  }
  const auto prefixDimCount =
      std::ranges::count_if(arenaDims_, [&](const auto &entry) {
        const DimRef dim = entry.second;
        return !isEphemeral(dim) || (dim & ~ephemeralBit) < m.cellCount;
      });
  if (m.cellCount > ArenaPrefixCodecLimits::maxSlots ||
      m.linkSize > ArenaPrefixCodecLimits::maxLinks ||
      m.contentSize > ArenaPrefixCodecLimits::maxContent ||
      m.shadowCount > ArenaPrefixCodecLimits::maxShadows ||
      prefixDimCount > ArenaPrefixCodecLimits::maxDims ||
      m.scratchSize > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected{ArenaRefusal::PrefixLimitExceeded};
  }
  if (m.proxyCount != 0 || m.quoteOccurrenceCount != 0 ||
      m.proxyShadowedEdgeCount != 0 || m.quoteSpaceCount != 0 ||
      hasUnsupportedUntrackedPrefixIdentity()) {
    return std::unexpected{ArenaRefusal::UnsupportedPrefixState};
  }

  std::uint64_t dimensionNameBytes = 0;
  for (const auto &[name, dim] : arenaDims_) {
    if (isEphemeral(dim) && (dim & ~ephemeralBit) >= m.cellCount) {
      continue;
    }
    if (name.size() >
        std::numeric_limits<std::uint32_t>::max() - dimensionNameBytes) {
      return std::unexpected{ArenaRefusal::PrefixLimitExceeded};
    }
    dimensionNameBytes += name.size();
  }
  if (!detail::arenaPrefixWireSize(detail::ArenaPrefixWireCounts{
          .slots              = m.cellCount,
          .links              = m.linkSize,
          .content            = m.contentSize,
          .scratchBytes       = m.scratchSize,
          .shadows            = m.shadowCount,
          .dimensions         = static_cast<std::uint64_t>(prefixDimCount),
          .dimensionNameBytes = dimensionNameBytes,
      })) {
    return std::unexpected{ArenaRefusal::PrefixLimitExceeded};
  }

  PrefixSnapshot out{};
  out.mark = m;
  // The snapshot carries the full name map, so a hydrated worker starts with
  // no dimension-cache undo entries of its own.
  // The trail itself is not serialized: the exported slots already contain
  // the state at the mark, and therefore form a fresh rollback baseline.
  out.mark.trailSize      = 0;
  out.mark.dimensionCount = 0;
  // A prefix carries no spaces (hasUnsupportedUntrackedPrefixIdentity).
  out.mark.spaceCount = 0;

  const auto cellCount = static_cast<std::size_t>(m.cellCount);
  out.slots.assign(slots_.begin(),
                   slots_.begin() + static_cast<std::ptrdiff_t>(cellCount));
  for (std::size_t i = trail_.size(); i > m.trailSize; --i) {
    const auto &entry = trail_[i - 1];
    if (entry.dense < out.slots.size()) {
      out.slots[entry.dense] = entry.saved;
    }
  }

  const auto linkSize = static_cast<std::size_t>(m.linkSize);
  out.links.assign(links_.begin(),
                   links_.begin() + static_cast<std::ptrdiff_t>(linkSize));

  const auto contentSize = static_cast<std::size_t>(m.contentSize);
  out.content.assign(content_.begin(),
                     content_.begin() +
                         static_cast<std::ptrdiff_t>(contentSize));

  const auto scratchSize = static_cast<std::size_t>(m.scratchSize);
  out.scratch.assign(scratch_.data(), scratchSize);

  const auto shadowCount = static_cast<std::size_t>(m.shadowCount);
  out.shadowOrder.assign(shadowOrder_.begin(),
                         shadowOrder_.begin() +
                             static_cast<std::ptrdiff_t>(shadowCount));

  for (const auto &[name, dim] : arenaDims_) {
    if (!isEphemeral(dim) || (dim & ~ephemeralBit) < cellCount) {
      out.arenaDims.emplace(name, dim);
    }
  }
  std::unordered_map<CellRef, std::uint32_t> overlay;
  if (!prefixHasRepresentableSizes(out) || !prefixWithinCodecLimits(out) ||
      !prefixWireSize(out) || !prefixRunsAreWellFormed(out, base_, overlay)) {
    return std::unexpected{ArenaRefusal::MalformedPrefix};
  }
  return out;
}

ArenaResult ArenaManifold::hydratePrefix(const PrefixSnapshot &snapshot) {
  if (outstandingMarks_ != 0) {
    return std::unexpected{ArenaRefusal::MarksOutstanding};
  }
  if (hasUnsupportedUntrackedPrefixIdentity() || !proxyOrder_.empty() ||
      !occurrenceToCanonical_.empty() || !occurrenceToView_.empty() ||
      !viewCanonicalToOccurrence_.empty() || !quoteOccurrenceOrder_.empty() ||
      !quoteSpace_.empty() || !quoteSpaceOrder_.empty() ||
      !proxyShadowedEdges_.empty() || !proxyShadowedEdgeOrder_.empty()) {
    return std::unexpected{ArenaRefusal::UnsupportedPrefixState};
  }
  const auto &m = snapshot.mark;
  if (m.proxyCount != 0 || m.quoteOccurrenceCount != 0 ||
      m.proxyShadowedEdgeCount != 0 || m.quoteSpaceCount != 0 ||
      m.spaceCount != 0) {
    return std::unexpected{ArenaRefusal::UnsupportedPrefixState};
  }
  if (!prefixHasRepresentableSizes(snapshot) ||
      !prefixWithinCodecLimits(snapshot) || !prefixWireSize(snapshot)) {
    return std::unexpected{ArenaRefusal::PrefixLimitExceeded};
  }
  if (snapshot.slots.size() != m.cellCount ||
      snapshot.links.size() != m.linkSize ||
      snapshot.content.size() != m.contentSize ||
      snapshot.scratch.size() != m.scratchSize ||
      snapshot.shadowOrder.size() != m.shadowCount || m.trailSize != 0 ||
      m.dimensionCount != 0 || m.cellCount > ephemeralBit ||
      m.liveLinks > m.linkSize || m.liveContent > m.contentSize) {
    return std::unexpected{ArenaRefusal::MalformedPrefix};
  }

  std::unordered_map<CellRef, std::uint32_t> overlay;
  overlay.reserve(snapshot.slots.size());
  if (!prefixRunsAreWellFormed(snapshot, base_, overlay)) {
    return std::unexpected{ArenaRefusal::MalformedPrefix};
  }

  std::vector<CellSlot> slots(snapshot.slots);
  std::vector<DimLink> links(snapshot.links);
  std::vector<xanadu::PrimediaSpan> content(snapshot.content);
  std::string scratch(snapshot.scratch);
  std::vector<CellRef> shadowOrder(snapshot.shadowOrder);
  std::unordered_map<std::string, DimRef> arenaDims(snapshot.arenaDims);
  std::vector<Floors> floors;
  floors.push_back(Floors{
      .cells = m.cellCount, .links = m.linkSize, .content = m.contentSize});

  slots_.swap(slots);
  links_.swap(links);
  content_.swap(content);
  scratch_.swap(scratch);
  shadowOrder_.swap(shadowOrder);
  arenaDims_.swap(arenaDims);
  overlay_.swap(overlay);
  trail_.clear();
  dimensionOrder_.clear();
  spaces_.clear();
  spaceAdoptions_.clear();
  spaceProxies_.clear();
  spaceStoreRefsTail_.clear();
  spaceStoreRefsDimensions_.clear();
  proxyOrder_.clear();
  occurrenceToCanonical_.clear();
  occurrenceToView_.clear();
  viewCanonicalToOccurrence_.clear();
  quoteOccurrenceOrder_.clear();
  quoteSpace_.clear();
  quoteSpaceOrder_.clear();
  boundDimensions_.clear();
  proxyShadowedEdges_.clear();
  proxyShadowedEdgeOrder_.clear();
  provenanceCells_.clear();
  authorshipRoot_.reset();
  storesRankTail_.reset();
  floors_.swap(floors);
  liveLinks_        = m.liveLinks;
  liveContent_      = m.liveContent;
  outstandingMarks_ = 1;
  return this;
}

ArenaManifold *ArenaManifold::evictOldestMark() noexcept {
  if (outstandingMarks_ == 0 || floors_.empty()) {
    return this;
  }
  floors_.erase(floors_.begin());
  outstandingMarks_--;
  return this;
}

namespace {

void writeU8(std::string &out, const std::uint8_t v) {
  out.push_back(static_cast<char>(v));
}

void writeU16(std::string &out, const std::uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFFU));
  out.push_back(static_cast<char>((v >> 8U) & 0xFFU));
}

void writeU32(std::string &out, const std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFU));
  }
}

void writeU64(std::string &out, const std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>((v >> (i * 8)) & 0xFFULL));
  }
}

void writeStr(std::string &out, const std::string_view s) {
  if (s.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("APF2 string exceeds u32 length");
  }
  writeU32(out, static_cast<std::uint32_t>(s.size()));
  out.append(s.data(), s.size());
}

bool readU8(std::string_view &in, std::uint8_t &out) {
  if (in.empty()) return false;
  out = static_cast<std::uint8_t>(in.front());
  in.remove_prefix(1);
  return true;
}

bool readU16(std::string_view &in, std::uint16_t &out) {
  if (in.size() < 2) return false;
  out = static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[0])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[1])) << 8U);
  in.remove_prefix(2);
  return true;
}

bool readU32(std::string_view &in, std::uint32_t &out) {
  if (in.size() < 4) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) {
    out |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[i]))
           << (i * 8);
  }
  in.remove_prefix(4);
  return true;
}

bool readU64(std::string_view &in, std::uint64_t &out) {
  if (in.size() < 8) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[i]))
           << (i * 8);
  }
  in.remove_prefix(8);
  return true;
}

bool readStr(std::string_view &in, std::string &out) {
  std::uint32_t len = 0;
  if (!readU32(in, len) || in.size() < len) return false;
  out.assign(in.data(), len);
  in.remove_prefix(len);
  return true;
}

} // namespace

std::string encodeArenaPrefix(const ArenaManifold::PrefixSnapshot &snapshot) {
  if (!prefixHasRepresentableSizes(snapshot) ||
      !prefixWithinCodecLimits(snapshot) || !prefixWireSize(snapshot)) {
    throw std::length_error("arena prefix exceeds APF2 codec limits");
  }
  std::string out;
  out.append("APF2", 4);
  writeU32(out, snapshot.mark.cellCount);
  writeU32(out, snapshot.mark.linkSize);
  writeU32(out, snapshot.mark.contentSize);
  writeU32(out, snapshot.mark.scratchSize);
  writeU32(out, snapshot.mark.trailSize);
  writeU32(out, snapshot.mark.liveLinks);
  writeU32(out, snapshot.mark.liveContent);
  writeU32(out, snapshot.mark.shadowCount);
  writeU32(out, snapshot.mark.proxyCount);
  writeU32(out, snapshot.mark.quoteOccurrenceCount);
  writeU32(out, snapshot.mark.proxyShadowedEdgeCount);
  writeU32(out, snapshot.mark.quoteSpaceCount);
  writeU32(out, snapshot.mark.dimensionCount);

  writeU32(out, static_cast<std::uint32_t>(snapshot.slots.size()));
  for (const auto &s : snapshot.slots) {
    writeU32(out, s.spanOffset);
    writeU16(out, s.spanCount);
    writeU16(out, s.formatFlags);
    writeU32(out, s.birthOp);
    writeU32(out, s.lastOp);
    writeU32(out, s.linkOffset);
    writeU16(out, s.linkCount);
    writeU8(out, s.valueKind);
    writeU8(out, s.flags);
    writeU64(out, s.valueBits);
  }

  writeU32(out, static_cast<std::uint32_t>(snapshot.links.size()));
  for (const auto &l : snapshot.links) {
    writeU32(out, l.dim);
    writeU32(out, l.pos);
    writeU32(out, l.neg);
  }

  writeU32(out, static_cast<std::uint32_t>(snapshot.content.size()));
  for (const auto &c : snapshot.content) {
    writeU32(out, c.scroll);
    writeU64(out, c.start);
    writeU64(out, c.length);
  }

  writeStr(out, snapshot.scratch);

  writeU32(out, static_cast<std::uint32_t>(snapshot.shadowOrder.size()));
  for (const auto ref : snapshot.shadowOrder) {
    writeU32(out, ref);
  }

  writeU32(out, static_cast<std::uint32_t>(snapshot.arenaDims.size()));
  for (const auto &[name, dim] : snapshot.arenaDims) {
    writeStr(out, name);
    writeU32(out, dim);
  }
  return out;
}

std::optional<ArenaManifold::PrefixSnapshot>
decodeArenaPrefix(std::string_view bytes) {
  if (bytes.size() > std::numeric_limits<std::uint32_t>::max() ||
      bytes.size() < 4 || bytes.substr(0, 4) != "APF2") {
    return std::nullopt;
  }
  bytes.remove_prefix(4);

  ArenaManifold::PrefixSnapshot out{};
  if (!readU32(bytes, out.mark.cellCount) ||
      !readU32(bytes, out.mark.linkSize) ||
      !readU32(bytes, out.mark.contentSize) ||
      !readU32(bytes, out.mark.scratchSize) ||
      !readU32(bytes, out.mark.trailSize) ||
      !readU32(bytes, out.mark.liveLinks) ||
      !readU32(bytes, out.mark.liveContent) ||
      !readU32(bytes, out.mark.shadowCount) ||
      !readU32(bytes, out.mark.proxyCount) ||
      !readU32(bytes, out.mark.quoteOccurrenceCount) ||
      !readU32(bytes, out.mark.proxyShadowedEdgeCount) ||
      !readU32(bytes, out.mark.quoteSpaceCount) ||
      !readU32(bytes, out.mark.dimensionCount)) {
    return std::nullopt;
  }

  std::uint32_t slotCount = 0;
  if (!readU32(bytes, slotCount) ||
      slotCount > ArenaPrefixCodecLimits::maxSlots) {
    return std::nullopt;
  }
  out.slots.resize(slotCount);
  for (std::uint32_t i = 0; i < slotCount; ++i) {
    auto &s = out.slots[i];
    if (!readU32(bytes, s.spanOffset) || !readU16(bytes, s.spanCount) ||
        !readU16(bytes, s.formatFlags) || !readU32(bytes, s.birthOp) ||
        !readU32(bytes, s.lastOp) || !readU32(bytes, s.linkOffset) ||
        !readU16(bytes, s.linkCount) || !readU8(bytes, s.valueKind) ||
        !readU8(bytes, s.flags) || !readU64(bytes, s.valueBits)) {
      return std::nullopt;
    }
  }

  std::uint32_t linkCount = 0;
  if (!readU32(bytes, linkCount) ||
      linkCount > ArenaPrefixCodecLimits::maxLinks) {
    return std::nullopt;
  }
  out.links.resize(linkCount);
  for (std::uint32_t i = 0; i < linkCount; ++i) {
    auto &l = out.links[i];
    if (!readU32(bytes, l.dim) || !readU32(bytes, l.pos) ||
        !readU32(bytes, l.neg)) {
      return std::nullopt;
    }
  }

  std::uint32_t contentCount = 0;
  if (!readU32(bytes, contentCount) ||
      contentCount > ArenaPrefixCodecLimits::maxContent) {
    return std::nullopt;
  }
  out.content.resize(contentCount);
  for (std::uint32_t i = 0; i < contentCount; ++i) {
    auto &c = out.content[i];
    if (!readU32(bytes, c.scroll) || !readU64(bytes, c.start) ||
        !readU64(bytes, c.length)) {
      return std::nullopt;
    }
  }

  if (!readStr(bytes, out.scratch)) {
    return std::nullopt;
  }

  std::uint32_t shadowCount = 0;
  if (!readU32(bytes, shadowCount) ||
      shadowCount > ArenaPrefixCodecLimits::maxShadows) {
    return std::nullopt;
  }
  out.shadowOrder.resize(shadowCount);
  for (std::uint32_t i = 0; i < shadowCount; ++i) {
    if (!readU32(bytes, out.shadowOrder[i])) {
      return std::nullopt;
    }
  }

  std::uint32_t dimCount = 0;
  if (!readU32(bytes, dimCount) || dimCount > ArenaPrefixCodecLimits::maxDims) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < dimCount; ++i) {
    std::string name;
    DimRef dim = noCell;
    if (!readStr(bytes, name) || !readU32(bytes, dim)) {
      return std::nullopt;
    }
    if (!out.arenaDims.emplace(std::move(name), dim).second) {
      return std::nullopt;
    }
  }
  if (!bytes.empty()) {
    return std::nullopt;
  }
  return out;
}

} // namespace zigzag
