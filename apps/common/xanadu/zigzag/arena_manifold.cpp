#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <algorithm>
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
  const auto span               = intern(name);
  const auto dim                = makeCell(span);
  arenaDims_[std::string(name)] = dim;

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
  if (base_ == nullptr && space.manifold != nullptr) {
    base_ = space.manifold;
  }
  if (store_ == nullptr && space.store != nullptr) {
    store_ = space.store;
  }
  if (space.reader == nullptr && space.store != nullptr) {
    space.reader = space.store;
  }

  const auto dimStores = ensureDimension("d.stores");
  ensureDimension("d.store-refs");

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
  spaceProxies_.emplace_back();
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

  const auto bits = (static_cast<std::uint64_t>(space) << 32) |
                    static_cast<std::uint64_t>(foreignIndex);
  const CellRef proxy = mintSlot(xanadu::ValueKind::ExternRef, bits, {});

  const auto dimStoreRefs = ensureDimension("d.store-refs");
  CellRef tail            = spaceStoreRefsTail_[space - 1];
  zigzag::expectWritten(link(tail, dimStoreRefs, DimVector::POS, proxy));
  spaceStoreRefsTail_[space - 1] = proxy;

  map[foreignIndex] = proxy;
  proxyOrder_.push_back(
      ProxyEntry{.space = space, .foreignIndex = foreignIndex, .proxy = proxy});
  return proxy;
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
  for (const auto &[arenaDim, bSet] : boundDimensions_) {
    bool matches = (arenaDim == dim);
    if (!matches) {
      for (const auto &member : bSet.members) {
        if (member.dim == dim) {
          matches = true;
          break;
        }
      }
    }
    if (matches) {
      for (const auto &member : bSet.members) {
        if (member.space == space) {
          return member.dim;
        }
      }
    }
  }

  const auto s = spaceAt(space);
  if (!s || !s->manifold) {
    return std::nullopt;
  }

  if (spaces_.size() == 1) {
    const auto dims = s->manifold->dimensions();
    if (std::ranges::find(dims, dim) != dims.end()) {
      return dim;
    }
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
    if (proxyShadowedEdges_.insert(key).second) {
      proxyShadowedEdgeOrder_.push_back(key);
    }
  }
  DimLink *const edge = linkFor(dense, dim);
  if (nullptr == edge) {
    return;
  }
  edge->encodedNeighbor(dir) = to;
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
  // The dimension and the far end are only *read* here, so they are resolved
  // rather than shadowed -- a link to a base cell shadows that cell because
  // its reciprocal end changes, which is what the second shadow() below is.
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
  const auto dense = shadow(from);
  if (!dense.has_value()) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  const auto target = to.has_value() ? shadow(*to) : std::nullopt;

  const auto d = *dense;
  // Both sides read out before anything is touched: linkFor() can grow a run,
  // and growing a run can move every DimLink in the arena.
  std::optional<CellRef> displacedUs;
  std::optional<CellRef> displacedIt;
  if (const DimLink *const mine = existingLink(d, dim); nullptr != mine) {
    displacedUs = mine->neighbor(dir);
  }
  if (target.has_value()) {
    if (const DimLink *const theirs = existingLink(*target, dim);
        nullptr != theirs) {
      displacedIt = theirs->neighbor(-dir);
    }
  }

  // A link is one edge two cells share, so setting it breaks whatever each end
  // held: the invariant is linked(a, d, dir) == b exactly when
  // linked(b, d, -dir) == a. Maintained rather than derived, same as Manifold.
  // shadow(), not denseOf(): a displaced occupant may still be living in the
  // base, and clearing its end of the edge is a write like any other.
  if (displacedUs && displacedUs != toCell) {
    if (const auto other = shadow(*displacedUs); other.has_value()) {
      setOneSide(*other, dim, -dir, noCell);
    }
  }
  if (displacedIt && displacedIt != from) {
    if (const auto other = shadow(*displacedIt); other.has_value()) {
      setOneSide(*other, dim, dir, noCell);
    }
  }
  if (target.has_value()) {
    setOneSide(*target, dim, -dir, from);
  }
  setOneSide(d, dim, dir, toCell);
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
  // Already copied out under this mark -- both runs sit above the floor, so
  // every write to them is already in territory release() truncates. Exact
  // rather than a heuristic, and it is what keeps one trail entry per old cell
  // per choice point instead of one per write.
  if (cell.linkOffset >= floor.links && cell.spanOffset >= floor.content) {
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

Mark ArenaManifold::mark() noexcept {
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
  }
  return this;
}

ArenaManifold *ArenaManifold::release(const Mark &m) noexcept {
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

  const auto dimStoreRefs = dimensionNamed("d.store-refs");
  if (dimStoreRefs) {
    for (std::size_t s = 0; s < spaces_.size(); ++s) {
      CellRef cur = *spaces_[s].storeCell;
      while (cur != noCell) {
        const auto nxt = linked(cur, *dimStoreRefs, DimVector::POS);
        if (!nxt) {
          break;
        }
        cur = *nxt;
      }
      if (s < spaceStoreRefsTail_.size()) {
        spaceStoreRefsTail_[s] = cur;
      }
    }
  }

  if (outstandingMarks_ > 0) {
    outstandingMarks_--;
    floors_.pop_back();
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
                                const PromotionBudget budget) {
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
  for (const auto cell : order) {
    if (isEphemeral(cell)) out.cells.push_back(real.at(cell));
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
    for (const auto &edge : from.dimensionsOf(arena)) {
      // Only the posward side: the negward one is the same edge read from the
      // other end, and setLink() maintains both.
      if (noCell == edge.pos) {
        continue;
      }
      const auto dim = real.find(edge.dim);
      const auto to  = real.find(edge.pos);
      if (dim == real.end() || to == real.end()) {
        continue;
      }
      if (nullptr != from.base() && !isEphemeral(arena) &&
          !isEphemeral(edge.dim) && !isEphemeral(edge.pos)) {
        if (from.base()->linked(arena, edge.dim, DimVector::POS) == edge.pos) {
          continue;
        }
      }
      out.version = store.setLink(out.version, real.at(arena), dim->second,
                                  false, to->second);
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

ArenaManifold::PrefixSnapshot ArenaManifold::exportPrefix(const Mark &m) const {
  PrefixSnapshot out{};
  out.mark = m;

  const auto cellCount = std::min<std::size_t>(m.cellCount, slots_.size());
  out.slots.assign(slots_.begin(),
                   slots_.begin() + static_cast<std::ptrdiff_t>(cellCount));
  for (std::size_t i = trail_.size(); i > m.trailSize; --i) {
    const auto &entry = trail_[i - 1];
    if (entry.dense < out.slots.size()) {
      out.slots[entry.dense] = entry.saved;
    }
  }

  const auto linkSize = std::min<std::size_t>(m.linkSize, links_.size());
  out.links.assign(links_.begin(),
                   links_.begin() + static_cast<std::ptrdiff_t>(linkSize));

  const auto contentSize =
      std::min<std::size_t>(m.contentSize, content_.size());
  out.content.assign(content_.begin(),
                     content_.begin() +
                         static_cast<std::ptrdiff_t>(contentSize));

  const auto scratchSize =
      std::min<std::size_t>(m.scratchSize, scratch_.size());
  out.scratch.assign(scratch_.data(), scratchSize);

  const auto shadowCount =
      std::min<std::size_t>(m.shadowCount, shadowOrder_.size());
  out.shadowOrder.assign(shadowOrder_.begin(),
                         shadowOrder_.begin() +
                             static_cast<std::ptrdiff_t>(shadowCount));

  for (const auto &[name, dim] : arenaDims_) {
    if (!isEphemeral(dim) || (dim & ~ephemeralBit) < cellCount) {
      out.arenaDims.emplace(name, dim);
    }
  }
  return out;
}

ArenaManifold *ArenaManifold::hydratePrefix(const PrefixSnapshot &snapshot) {
  slots_       = snapshot.slots;
  links_       = snapshot.links;
  content_     = snapshot.content;
  scratch_     = snapshot.scratch;
  shadowOrder_ = snapshot.shadowOrder;
  arenaDims_   = snapshot.arenaDims;
  liveLinks_   = snapshot.mark.liveLinks;
  liveContent_ = snapshot.mark.liveContent;

  overlay_.clear();
  for (std::uint32_t dense = 0; dense < slots_.size(); ++dense) {
    if (!isEphemeral(slots_[dense].birthOp)) {
      overlay_[slots_[dense].birthOp] = dense;
    }
  }

  trail_.clear();
  floors_.clear();
  floors_.push_back(Floors{
      .cells   = static_cast<std::uint32_t>(slots_.size()),
      .links   = static_cast<std::uint32_t>(links_.size()),
      .content = static_cast<std::uint32_t>(content_.size()),
  });
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
  std::string out;
  out.append("APFX", 4);
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
  if (bytes.size() < 4 || bytes.substr(0, 4) != "APFX") {
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
      !readU32(bytes, out.mark.quoteSpaceCount)) {
    return std::nullopt;
  }

  std::uint32_t slotCount = 0;
  if (!readU32(bytes, slotCount) || slotCount > 1000000U) {
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
  if (!readU32(bytes, linkCount) || linkCount > 4000000U) {
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
  if (!readU32(bytes, contentCount) || contentCount > 4000000U) {
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
  if (!readU32(bytes, shadowCount) || shadowCount > 1000000U) {
    return std::nullopt;
  }
  out.shadowOrder.resize(shadowCount);
  for (std::uint32_t i = 0; i < shadowCount; ++i) {
    if (!readU32(bytes, out.shadowOrder[i])) {
      return std::nullopt;
    }
  }

  std::uint32_t dimCount = 0;
  if (!readU32(bytes, dimCount) || dimCount > 100000U) {
    return std::nullopt;
  }
  for (std::uint32_t i = 0; i < dimCount; ++i) {
    std::string name;
    DimRef dim = noCell;
    if (!readStr(bytes, name) || !readU32(bytes, dim)) {
      return std::nullopt;
    }
    out.arenaDims.emplace(std::move(name), dim);
  }
  return out;
}

} // namespace zigzag
