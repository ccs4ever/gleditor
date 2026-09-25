#include "common/xanadu/zigzag/arena_manifold.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

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

CellRef ArenaManifold::home() const noexcept {
  return base_ ? base_->home() : noCell;
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
  const auto d = dimensionNamed(name, &reader);
  if (d != noCell) {
    return d;
  }
  return std::nullopt;
}

DimRef ArenaManifold::dimensionNamed(const std::string_view name,
                                     const xanadu::SpanReader *reader) const {
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
  return noCell;
}

DimRef ArenaManifold::ensureDimension(const std::string_view name) {
  const auto existing = dimensionNamed(name);
  if (noCell != existing) {
    return existing;
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
  if (noCell != dimDims && noCell != home) {
    CellRef tail = home;
    while (true) {
      const CellRef nxt = linked(tail, dimDims, DimVector::POS);
      if (noCell == nxt) {
        break;
      }
      tail = nxt;
    }
    link(tail, dimDims, DimVector::POS, dim);
  }
  return dim;
}

void ArenaManifold::projectProvenance(const xanadu::Store &store) {
  if (!store.provenance().has_value()) {
    return;
  }
  const auto &signedProv = *store.provenance();
  if (signedProv.signature.empty() || signedProv.tsv.empty()) {
    return;
  }
  const auto check = xanadu::verifyProvenance(signedProv);
  if (!check.signatureValid) {
    return;
  }
  const auto prov = xanadu::parseProvenance(signedProv.tsv);
  if (!prov) {
    return;
  }

  const CellRef home = homeCell();
  if (noCell == home) {
    return;
  }

  const DimRef dimAuthorship = ensureDimension("d.authorship");
  provenanceCells_.insert(dimAuthorship);
  authorshipRoot_ = makeCell(intern("AUTHORSHIP.tsv"));
  provenanceCells_.insert(authorshipRoot_);
  link(home, dimAuthorship, DimVector::POS, authorshipRoot_);

  const DimRef dimSource = ensureDimension("d.source");
  provenanceCells_.insert(dimSource);
  CellRef bootstrapCell    = noCell;
  const auto &bootstrapKey = store.bootstrapPermascrollKey();
  if (!bootstrapKey.empty()) {
    const auto &registry = store.scrollRegistry();
    for (const auto &rec : registry.scrolls) {
      if (rec.globalKey == bootstrapKey && rec.cell != noCell) {
        bootstrapCell = rec.cell;
        break;
      }
    }
    if (noCell == bootstrapCell) {
      const DimRef dimScrolls = dimensionNamed("d.scrolls");
      if (noCell != dimScrolls) {
        CellRef cur = home;
        while (true) {
          cur = linked(cur, dimScrolls, DimVector::POS);
          if (noCell == cur) {
            break;
          }
          if (textOf(cur) == bootstrapKey) {
            bootstrapCell = cur;
            break;
          }
        }
      }
    }
    if (noCell == bootstrapCell) {
      bootstrapCell = makeCell(intern(bootstrapKey));
      provenanceCells_.insert(bootstrapCell);
    }
  }
  if (noCell != bootstrapCell) {
    link(authorshipRoot_, dimSource, DimVector::POS, bootstrapCell);
  }

  auto appendRank = [this](std::string_view dimName, std::string_view value) {
    const DimRef dim = ensureDimension(dimName);
    provenanceCells_.insert(dim);
    const CellRef cell = makeCell(intern(value));
    provenanceCells_.insert(cell);
    CellRef tail = authorshipRoot_;
    while (true) {
      const CellRef nxt = linked(tail, dim, DimVector::POS);
      if (noCell == nxt) {
        break;
      }
      tail = nxt;
    }
    link(tail, dim, DimVector::POS, cell);
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
}

void expectWritten(const ArenaResult &result,
                   const std::source_location where) noexcept {
  if (!result) {
    GLEDITOR_LOG_WARN("zigzag.arena", "arena write refused ({}) at {}:{}",
                      toString(result.error()), where.file_name(),
                      where.line());
  }
}
std::uint32_t ArenaManifold::denseOf(const CellRef ref) const noexcept {
  if (!isEphemeral(ref)) {
    // A base cell, which this arena holds only once it has been shadowed.
    // Until then denseOf() says "not mine" and every read falls through.
    const auto found = overlay_.find(ref);
    return found == overlay_.end() ? noDense : found->second;
  }
  const auto dense = ref & ~ephemeralBit;
  return dense < slots_.size() ? dense : noDense;
}

bool ArenaManifold::holdsOwn(const CellRef ref) const noexcept {
  return noDense != denseOf(ref);
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

  if (space.storeCell == noCell) {
    const std::string label =
        space.label.empty() ? ("space_" + std::to_string(id)) : space.label;
    space.storeCell = makeCell(label);
  }

  CellRef root = homeCell();
  if (root == noCell) {
    root = makeCell("home");
  }

  if (space.storeCell != root &&
      linked(space.storeCell, dimStores, DimVector::NEG) == noCell &&
      linked(space.storeCell, dimStores, DimVector::POS) == noCell) {
    if (storesRankTail_ == noCell) {
      CellRef cur = root;
      while (true) {
        const CellRef nxt = linked(cur, dimStores, DimVector::POS);
        if (noCell == nxt) {
          break;
        }
        cur = nxt;
      }
      zigzag::expectWritten(
          link(cur, dimStores, DimVector::POS, space.storeCell));
    } else {
      zigzag::expectWritten(
          link(storesRankTail_, dimStores, DimVector::POS, space.storeCell));
    }
  }
  storesRankTail_ = space.storeCell;

  spaceStoreRefsTail_.push_back(space.storeCell);
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
    return noCell;
  }
  auto &map = spaceProxies_[space - 1];
  if (const auto it = map.find(foreignIndex); it != map.end()) {
    return it->second;
  }

  const auto bits     = (static_cast<std::uint64_t>(space) << 32) |
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

CellRef
ArenaManifold::findExistingProxy(const std::uint32_t space,
                                 const CellRef foreignIndex) const noexcept {
  if (space == 0 || space > spaceProxies_.size() || foreignIndex == noCell) {
    return noCell;
  }
  const auto &map = spaceProxies_[space - 1];
  const auto it   = map.find(foreignIndex);
  if (it != map.end()) {
    return it->second;
  }
  return noCell;
}

bool ArenaManifold::isProxy(const CellRef ref) const noexcept {
  if (!isEphemeral(ref)) {
    return false;
  }
  if (isQuoteOccurrence(ref)) {
    return false;
  }
  const auto dense = denseOf(ref);
  if (noDense == dense || dense >= slots_.size()) {
    return false;
  }
  return slots_[dense].valueKind ==
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
    const CellRef nxt = linked(cur, dimQuoteOrigin, DimVector::POS);
    if (noCell == nxt) {
      break;
    }
    cur = nxt;
  }
  zigzag::expectWritten(link(cur, dimQuoteOrigin, DimVector::POS, occurrence));
  return occurrence;
}

bool ArenaManifold::isQuoteOccurrence(const CellRef ref) const noexcept {
  return occurrenceToCanonical_.contains(ref);
}

CellRef
ArenaManifold::canonicalProxyOf(const CellRef occurrence) const noexcept {
  const auto it = occurrenceToCanonical_.find(occurrence);
  return it != occurrenceToCanonical_.end() ? it->second : noCell;
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
  if (noDense == dense || dense >= slots_.size()) {
    return gleditor::cpp26::nullopt;
  }
  const auto &cell = slots_[dense];
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
  const auto foreign = foreignOf(ref);
  if (!foreign) {
    return gleditor::cpp26::nullopt;
  }
  const auto s = spaceAt(foreign->space);
  if (!s || !s->store) {
    return gleditor::cpp26::nullopt;
  }
  return std::make_pair(s->store, foreign->index);
}

DimRef ArenaManifold::dimIn(const std::uint32_t space,
                            const DimRef dim) const noexcept {
  if (space == 0 || space > spaces_.size() || dim == noCell) {
    return noCell;
  }
  if (const auto it = boundDimensions_.find(dim);
      it != boundDimensions_.end()) {
    for (const auto &member : it->second.members) {
      if (member.space == space) {
        return member.dim;
      }
    }
  }
  const auto s = spaceAt(space);
  if (!s || !s->manifold) {
    return noCell;
  }
  if (s->manifold->contains(dim)) {
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
  return noCell;
}

void ArenaManifold::bindDimension(const DimRef arenaDim,
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
      return;
    }
  }
  set.members.push_back(
      BoundDimensionMember{.space = space, .dim = foreignDim, .mode = mode});
}

gleditor::cpp26::optional<const BoundDimensionSet &>
ArenaManifold::boundDimensionSet(const DimRef dim) const noexcept {
  const auto it = boundDimensions_.find(dim);
  if (it != boundDimensions_.end()) {
    return it->second;
  }
  return gleditor::cpp26::nullopt;
}

void ArenaManifold::materializeFrontier(const std::uint32_t space,
                                        const CellRef foreignHead,
                                        const DimRef foreignDim,
                                        const DimVector dir,
                                        const std::size_t maxSteps) {
  const auto s = spaceAt(space);
  if (!s || !s->manifold) {
    return;
  }
  CellRef cur = foreignHead;
  if (isProxy(foreignHead)) {
    const auto foreign = foreignOf(foreignHead);
    if (foreign && foreign->space == space) {
      cur = foreign->index;
    }
  }
  DimRef fDim = foreignDim;
  if (contains(foreignDim)) {
    const auto resolved = dimIn(space, foreignDim);
    if (noCell != resolved) {
      fDim = resolved;
    }
  }
  std::size_t steps = 0;
  while (cur != noCell && steps < maxSteps) {
    proxyFor(space, cur);
    cur = s->manifold->linked(cur, fDim, dir);
    steps++;
  }
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
  if (noDense != dense) {
    return SlotRef{slots_[dense]};
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
  edge->neighbor(dir) = to;
}

CellRef ArenaManifold::linked(const CellRef from, const DimRef dim,
                              const DimVector dir) const noexcept {
  const auto dense = denseOf(from);
  if (noDense == dense) {
    return nullptr == base_ ? noCell : base_->linked(from, dim, dir);
  }
  if (isQuoteOccurrence(from)) {
    // Only authored / materialized local edges exist in this presentation view
    const auto &cell = slots_[dense];
    for (std::uint16_t i = 0; i < cell.linkCount; i++) {
      const auto &edge = links_[static_cast<std::size_t>(cell.linkOffset) + i];
      if (edge.dim == dim) {
        return edge.neighbor(dir);
      }
    }
    return noCell;
  }
  if (isProxy(from)) {
    // Check if explicitly shadowed locally
    if (proxyShadowedEdges_.contains(
            ShadowEdgeKey{.dense = dense, .dim = dim, .dir = dir})) {
      const auto &cell = slots_[dense];
      for (std::uint16_t i = 0; i < cell.linkCount; i++) {
        const auto &edge =
            links_[static_cast<std::size_t>(cell.linkOffset) + i];
        if (edge.dim == dim) {
          return edge.neighbor(dir);
        }
      }
      return noCell;
    }
    // Delegate to foreign space
    const auto foreign = foreignOf(from);
    if (foreign) {
      const auto s = spaceAt(foreign->space);
      if (s && s->manifold) {
        const auto fDim = dimIn(foreign->space, dim);
        if (noCell != fDim) {
          const auto fAns = s->manifold->linked(foreign->index, fDim, dir);
          if (noCell != fAns) {
            return findExistingProxy(foreign->space, fAns);
          }
        }
      }
    }
    return noCell;
  }
  const auto &cell = slots_[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    const auto &edge = links_[static_cast<std::size_t>(cell.linkOffset) + i];
    if (edge.dim == dim) {
      return edge.neighbor(dir);
    }
  }
  return noCell;
}

std::span<const DimLink>
ArenaManifold::dimensionsOf(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  if (noDense == dense) {
    return nullptr == base_ ? std::span<const DimLink>{}
                            : base_->dimensionsOf(ref);
  }
  if (isProxy(ref)) {
    const auto &cell = slots_[dense];
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
  const auto &cell = slots_[dense];
  return std::span<const DimLink>{links_.data() + cell.linkOffset,
                                  cell.linkCount};
}

std::span<const xanadu::PrimediaSpan>
ArenaManifold::contentOf(const CellRef ref) const noexcept {
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref);
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
  if (noDense == dense) {
    return nullptr == base_ ? std::span<const xanadu::PrimediaSpan>{}
                            : base_->contentOf(target);
  }
  const auto &cell = slots_[dense];
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
    target = canonicalProxyOf(ref);
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
    target = canonicalProxyOf(ref);
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
    target = canonicalProxyOf(ref);
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
  CellRef target = ref;
  if (isQuoteOccurrence(ref)) {
    target = canonicalProxyOf(ref);
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
  const auto start = scratch_.size();
  scratch_ += text;
  return xanadu::PrimediaSpan{
      .scroll = xanadu::scratchScroll, .start = start, .length = text.size()};
}

CellRef
ArenaManifold::mintSlot(const xanadu::ValueKind kind, const std::uint64_t bits,
                        const std::span<const xanadu::PrimediaSpan> content) {
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
  if (noDense == dense) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  trail(dense);
  setContentAt(dense, spans);
  return {};
}

ArenaResult ArenaManifold::setValueBits(const CellRef cell,
                                        const xanadu::ValueKind kind,
                                        const std::uint64_t bits) {
  const auto dense = shadow(cell);
  if (noDense == dense) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  trail(dense);
  slots_[dense].valueKind = static_cast<std::uint8_t>(kind);
  slots_[dense].valueBits = bits;
  return {};
}

ArenaResult ArenaManifold::link(const CellRef from, const DimRef dim,
                                const DimVector dir, const CellRef to) {
  // The dimension and the far end are only *read* here, so they are resolved
  // rather than shadowed -- a link to a base cell shadows that cell because
  // its reciprocal end changes, which is what the second shadow() below is.
  if (!contains(from)) {
    return std::unexpected{ArenaRefusal::UnknownCell};
  }
  if (!contains(dim)) {
    return std::unexpected{ArenaRefusal::UnknownDimension};
  }
  if (noCell != to && !contains(to)) {
    return std::unexpected{ArenaRefusal::UnknownTarget};
  }
  const auto dense  = shadow(from);
  const auto target = noCell == to ? noDense : shadow(to);

  // Both sides read out before anything is touched: linkFor() can grow a run,
  // and growing a run can move every DimLink in the arena.
  CellRef displacedUs = noCell;
  CellRef displacedIt = noCell;
  if (const DimLink *const mine = existingLink(dense, dim); nullptr != mine) {
    displacedUs = mine->neighbor(dir);
  }
  if (noDense != target) {
    if (const DimLink *const theirs = existingLink(target, dim);
        nullptr != theirs) {
      displacedIt = theirs->neighbor(-dir);
    }
  }

  // A link is one edge two cells share, so setting it breaks whatever each end
  // held: the invariant is linked(a, d, dir) == b exactly when
  // linked(b, d, -dir) == a. Maintained rather than derived, same as Manifold.
  // shadow(), not denseOf(): a displaced occupant may still be living in the
  // base, and clearing its end of the edge is a write like any other.
  if (noCell != displacedUs && displacedUs != to) {
    if (const auto other = shadow(displacedUs); noDense != other) {
      setOneSide(other, dim, -dir, noCell);
    }
  }
  if (noCell != displacedIt && displacedIt != from) {
    if (const auto other = shadow(displacedIt); noDense != other) {
      setOneSide(other, dim, dir, noCell);
    }
  }
  if (noDense != target) {
    setOneSide(target, dim, -dir, from);
  }
  setOneSide(dense, dim, dir, to);
  return {};
}

std::uint32_t ArenaManifold::shadow(const CellRef ref) {
  if (const auto dense = denseOf(ref); noDense != dense) {
    return dense;
  }
  if (nullptr == base_) {
    return noDense;
  }
  const auto theirs = base_->slot(ref);
  if (!theirs) {
    return noDense;
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
  };
  floors_.push_back(Floors{.cells   = taken.cellCount,
                           .links   = taken.linkSize,
                           .content = taken.contentSize});
  outstandingMarks_++;
  return taken;
}

void ArenaManifold::discard(const Mark &m) noexcept {
  (void)m;
  if (0 == outstandingMarks_) {
    return;
  }
  outstandingMarks_--;
  floors_.pop_back();
  // The trail entries stay. They were recorded for whatever choice point
  // encloses this one, which still needs them -- this is why cut can throw
  // away a choice point without undoing the bindings made under it (§5.4).
}

void ArenaManifold::release(const Mark &m) noexcept {
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
  if (dimStoreRefs != noCell) {
    for (std::size_t s = 0; s < spaces_.size(); ++s) {
      CellRef cur = spaces_[s].storeCell;
      while (cur != noCell) {
        const CellRef nxt = linked(cur, dimStoreRefs, DimVector::POS);
        if (nxt == noCell) {
          break;
        }
        cur = nxt;
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
  return {};
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
    if (!from.holdsOwn(current) || from.isProxy(current)) {
      continue;
    }
    for (const auto &edge : from.dimensionsOf(current)) {
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

  // Nothing is written until the walk has finished and the budget has held, so
  // a refusal leaves the store exactly as it was.
  std::unordered_map<CellRef, CellRef> real;
  real.reserve(order.size());
  Promoted out{.version = parent, .cells = {}};
  out.cells.reserve(order.size());

  for (const CellRef arena : order) {
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
            if (!globalOp.empty()) {
              globalKey = globalOp.scroll;
              produces  = globalOp.produces;
            }
          }
          if (produces.isZero()) {
            produces = s->store->segmentedOps().idOf(foreign->index);
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
            out.version = store.makeExternRef(out.version, targetRef, nullptr);
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
    const auto spans = from.contentOf(arena);
    const auto kind  = from.valueKindOf(arena);

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
    } else if (xanadu::ValueKind::OpHandle == kind) {
      minted =
          store.makeOpHandle(out.version, from.handleTarget(arena).value_or(0),
                             from.textOf(arena));
    } else if (spans.empty()) {
      minted = store.makeCell(out.version, std::string_view{});
    } else if (xanadu::scratchScroll == spans.front().scroll) {
      // A scratch span acquires a permanent address here, and only here.
      minted = store.makeCell(out.version, from.scratchTextOf(spans.front()));
    } else {
      minted = store.makeCell(out.version, spans.front());
    }
    out.version            = minted;
    const CellRef realCell = store.cellRefOf(minted);
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
    if (from.isProxy(arena)) {
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
                                  false, to->second, &known);
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

} // namespace zigzag
