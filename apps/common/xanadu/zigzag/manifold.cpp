#include "common/xanadu/zigzag/manifold.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <iterator>
#include <limits>
#include <ranges>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "common/xanadu/zigzag/dimension_registry.hpp"

namespace zigzag {

std::uint32_t Manifold::denseOf(const CellRef ref) const noexcept {
  if (noCell == ref || isEphemeral(ref)) {
    return noDense;
  }
  const auto found = byRef.find(ref);
  return found == byRef.end() ? noDense : found->second;
}

const CellSlot *Manifold::slot(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  return noDense == dense ? nullptr : &slots[dense];
}

common::cpp26::optional<const CellSlot &>
Manifold::findSlot(const CellRef ref) const noexcept {
  const auto *const s = slot(ref);
  return nullptr == s ? common::cpp26::nullopt
                      : common::cpp26::optional<const CellSlot &>(*s);
}

DimLink *Manifold::existingLink(const std::uint32_t dense,
                                const DimRef dim) noexcept {
  const auto &cell = slots[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    if (links_[static_cast<std::size_t>(cell.linkOffset) + i].dim == dim) {
      return &links_[static_cast<std::size_t>(cell.linkOffset) + i];
    }
  }
  return nullptr;
}

DimLink *Manifold::linkFor(const std::uint32_t dense, const DimRef dim) {
  if (DimLink *const found = existingLink(dense, dim); nullptr != found) {
    return found;
  }
  // Before growing rather than after: compact() moves every run, so a pointer
  // handed out first would be left pointing at a dead one.
  if (links_.size() > 2 * liveLinks + compactionSlack) {
    compact();
  }

  auto &cell = slots[dense];
  if (cell.linkCount == std::numeric_limits<std::uint16_t>::max()) {
    return nullptr;
  }

  const std::size_t offset = cell.linkOffset;
  const std::size_t count  = cell.linkCount;
  if (offset + count != links_.size()) {
    // Not the arena's tail, so the run cannot simply be extended. Copy it to
    // the end and leave the old one dead for compact() to reclaim. Reserving
    // first is what makes the push_backs below safe to source from the same
    // vector, and geometric so that relocating repeatedly stays linear.
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
  liveLinks++;
  return &links_.back();
}

void Manifold::setOneSide(const std::uint32_t dense, const DimRef dim,
                          const DimVector dir, const CellRef to) {
  DimLink *const link = linkFor(dense, dim);
  if (nullptr == link) {
    return;
  }
  link->neighbor(dir) = to;
}

std::span<const xanadu::PrimediaSpan>
Manifold::contentOf(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  if (noDense == dense) {
    return {};
  }
  const auto &cell = slots[dense];
  return std::span<const xanadu::PrimediaSpan>{content.data() + cell.spanOffset,
                                               cell.spanCount};
}

void Manifold::setContent(const std::uint32_t dense,
                          const std::span<const xanadu::PrimediaSpan> spans) {
  auto &cell = slots[dense];
  liveContent -= cell.spanCount;

  // Grown in place when this cell's run is the arena's tail, relocated to the
  // end otherwise -- the same compromise the link arena makes, and compact()
  // reclaims what relocating leaves behind.
  const std::size_t offset = cell.spanOffset;
  const bool atTail        = offset + cell.spanCount == content.size();
  if (atTail && spans.size() <= cell.spanCount) {
    content.resize(offset + spans.size());
  } else if (!atTail || spans.size() > cell.spanCount) {
    if (content.capacity() < content.size() + spans.size()) {
      content.reserve(
          std::max(content.size() * 2, content.size() + spans.size()));
    }
    cell.spanOffset = static_cast<std::uint32_t>(content.size());
    content.resize(content.size() + spans.size());
  }
  std::ranges::copy(spans, content.begin() + cell.spanOffset);
  cell.spanCount = static_cast<std::uint16_t>(spans.size());
  liveContent += cell.spanCount;

  if (content.size() > 2 * liveContent + compactionSlack) {
    compact();
  }
}

void Manifold::spliceContent(const std::uint32_t dense, const std::uint64_t at,
                             const std::uint64_t removing,
                             const xanadu::PrimediaSpan &inserted) {
  // Version's algorithm, over the arena rather than over a vector per cell.
  // splitAt() cuts a piece so that a boundary exists where the edit lands, and
  // the pieces on either side keep the addresses they had -- which is the whole
  // point: an edit must not move the text it did not touch.
  const auto existing = contentOf(slots[dense].birthOp);
  std::vector<xanadu::PrimediaSpan> rebuilt;
  rebuilt.reserve(existing.size() + 2);

  std::uint64_t seen = 0;
  for (const auto &piece : existing) {
    const auto pieceEnd = seen + piece.length;
    // Everything wholly before the edit, and the head of a piece the edit cuts.
    if (seen < at) {
      rebuilt.push_back(piece.slice(0, std::min(piece.length, at - seen)));
    }
    // Everything wholly after it, and the tail of a piece the edit cuts.
    const auto removedEnd = at + removing;
    if (pieceEnd > removedEnd) {
      const auto from = removedEnd > seen ? removedEnd - seen : 0;
      rebuilt.push_back(piece.slice(from, piece.length - from));
    }
    seen = pieceEnd;
  }

  // The inserted span goes where the edit landed, which is after every piece
  // that ended at or before `at`.
  if (!inserted.empty()) {
    std::uint64_t upTo = 0;
    std::size_t where  = 0;
    for (; where < rebuilt.size() && upTo < at; where++) {
      upTo += rebuilt[where].length;
    }
    rebuilt.insert(rebuilt.begin() + static_cast<std::ptrdiff_t>(where),
                   inserted);
  }

  // Coalescing is safe here and was worth checking rather than assuming.
  // joins() merges only pieces of the same scroll that are already contiguous,
  // so the merged piece covers exactly the addresses the two did: every
  // question a cell is asked -- what it says, which addresses it holds, who
  // else quotes them -- is answered from addresses and gets the same answer
  // either way. §1's "a cell must not merge" is about *cells*, which d.clone
  // keeps distinct however identical their content; it says nothing about the
  // pieces within one. Merging also gives a canonical form, so a cell edited
  // and edited back matches one that was never touched.
  std::erase_if(rebuilt, [](const auto &piece) { return piece.empty(); });
  for (std::size_t i = 0; i + 1 < rebuilt.size();) {
    if (rebuilt[i].scroll == rebuilt[i + 1].scroll &&
        rebuilt[i].end() == rebuilt[i + 1].start) {
      rebuilt[i].length += rebuilt[i + 1].length;
      rebuilt.erase(rebuilt.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      continue;
    }
    i++;
  }

  setContent(dense, rebuilt);
}

FoldResult
Manifold::applyStructure(const std::uint32_t opIndex,
                         const xanadu::CompactOpNode &node) noexcept {
  if (xanadu::OpKind::Structure != node.kind) {
    return {};
  }
  foldedThrough_ = std::max(opIndex, foldedThrough_);

  // R8's boundary on the *address* side, and the twin of the isEphemeral()
  // check the SetLink case makes on cell refs. A span in the scratch scroll
  // names bytes an ArenaManifold constructed and has no permanent address, so a
  // persisted cell quoting one would be a transclusion into a scroll that does
  // not exist -- and one span is structurally identical to another, so nothing
  // downstream could tell. promote() is the only thing that turns scratch bytes
  // into an address; everything else is refused here. See spool.hpp's
  // scratchScroll and design/vlog-logic-extension.md §5.5.
  if (xanadu::scratchScroll == node.span().scroll) {
    return refuse(FoldRefusal::ScratchAddress);
  }

  switch (xanadu::structureVerbOf(node.flags)) {
  case xanadu::StructureVerb::MakeCell: {
    if (byRef.contains(opIndex)) {
      return refuse(FoldRefusal::DuplicateCell);
    }
    const auto dense = static_cast<std::uint32_t>(slots.size());
    // The empty runs start at the arenas' tails, so this cell's first link and
    // first span are appends in place rather than relocations.
    slots.push_back(CellSlot{
        .spanOffset  = static_cast<std::uint32_t>(content.size()),
        .spanCount   = 0,
        .formatFlags = 0,
        .birthOp     = opIndex,
        .lastOp      = opIndex,
        .linkOffset  = static_cast<std::uint32_t>(links_.size()),
        .linkCount   = 0,
        .valueKind = static_cast<std::uint8_t>(xanadu::valueKindOf(node.flags)),
        .flags     = 0,
        .valueBits = node.value,
    });
    byRef.emplace(opIndex, dense);
    if (!node.span().empty()) {
      const auto only = node.span();
      setContent(dense, std::span<const xanadu::PrimediaSpan>{&only, 1});
    }
    // Genesis mints home first and d.dims second, by fiat, because linking the
    // first dimension onto the d.dims rank needs d.dims to be nameable
    // already. See R12.
    if (noCell == home_) {
      home_ = opIndex;
    } else if (noCell == dimsDim_) {
      dimsDim_ = opIndex;
    }
    return {};
  }

  case xanadu::StructureVerb::SetLink: {
    // The subject is named by the micro-history chain rather than by a field of
    // its own: sourceOpIndex is the previous operation on this same cell, and
    // the chain's first link is the MakeCell whose index *is* the CellRef. See
    // R7, and byRef, which holds every operation in a chain for exactly this.
    const auto dense = denseOf(node.sourceOpIndex);
    const DimRef dim = node.linkId;
    const CellRef to = node.to;
    if (noDense == dense) {
      return refuse(FoldRefusal::UnknownSubject);
    }
    if (noDense == denseOf(dim)) {
      return refuse(FoldRefusal::UnknownDimension);
    }
    // R8's boundary as a bit: a link into a derived cell cannot be persisted,
    // and a stored link naming a cell this fold does not hold would be a
    // traversal walking into nothing.
    if (isEphemeral(to)) {
      return refuse(FoldRefusal::EphemeralTarget);
    }
    if (noCell != to && noDense == denseOf(to)) {
      return refuse(FoldRefusal::UnknownTarget);
    }

    const CellRef self  = slots[dense].birthOp;
    const DimVector dir = xanadu::structureDirectionOf(node.flags);
    CellRef displacedUs = noCell;
    CellRef displacedIt = noCell;
    // Read both sides out before touching anything: setOneSide() can grow a
    // run, and growing a run can move every DimLink in the arena.
    if (const DimLink *const mine = existingLink(dense, dim); nullptr != mine) {
      displacedUs = mine->neighbor(dir);
    }
    const auto target = denseOf(to);
    if (noDense != target) {
      if (const DimLink *const theirs = existingLink(target, dim);
          nullptr != theirs) {
        displacedIt = theirs->neighbor(-dir);
      }
    }

    // A link is one edge shared by two cells, so setting it breaks whatever
    // each end was holding: the invariant this maintains is that
    // linked(a, d, dir) == b exactly when linked(b, d, -dir) == a. zzcore's
    // loader derives the same backlinks; here it has to be maintained rather
    // than derived, because an op arrives one at a time.
    if (noCell != displacedUs && displacedUs != to) {
      if (const auto other = denseOf(displacedUs); noDense != other) {
        setOneSide(other, dim, -dir, noCell);
      }
    }
    if (noCell != displacedIt && displacedIt != self) {
      if (const auto other = denseOf(displacedIt); noDense != other) {
        setOneSide(other, dim, dir, noCell);
      }
    }
    if (noDense != target) {
      setOneSide(target, dim, -dir, self);
    }
    setOneSide(dense, dim, dir, to);

    slots[dense].lastOp = opIndex;
    byRef.emplace(opIndex, dense);
    dimsCacheStale = true;
    return {};
  }

  case xanadu::StructureVerb::Splice: {
    const auto dense = denseOf(node.sourceOpIndex);
    if (noDense == dense) {
      return refuse(FoldRefusal::UnknownSubject);
    }
    spliceContent(dense, node.at, node.length, node.span());
    slots[dense].lastOp = opIndex;
    byRef.emplace(opIndex, dense);
    return {};
  }

  case xanadu::StructureVerb::SetValue: {
    const auto dense = denseOf(node.sourceOpIndex);
    if (noDense == dense) {
      return refuse(FoldRefusal::UnknownSubject);
    }
    // States the cell's content and value in full rather than merging with
    // what was there: an operation that reads the state it is applied to would
    // make the fold depend on the order two branches were folded in.
    const auto restated = node.span();
    setContent(dense, restated.empty() ? std::span<const xanadu::PrimediaSpan>{}
                                       : std::span<const xanadu::PrimediaSpan>{
                                             &restated, 1});
    auto &cell     = slots[dense];
    cell.valueKind = static_cast<std::uint8_t>(xanadu::valueKindOf(node.flags));
    cell.valueBits = node.value;
    cell.lastOp    = opIndex;
    byRef.emplace(opIndex, dense);
    return {};
  }
  }

  // A verb this build does not know. Counted rather than ignored: the whole
  // point of refusedOps() is that a fold cannot throw and still must not
  // silently mean something else.
  return refuse(FoldRefusal::UnknownVerb);
}

AdvanceResult Manifold::advance(const xanadu::Store &store,
                                const xanadu::MicroversionId &version) {
  store_           = const_cast<xanadu::Store *>(&store);
  const auto index = store.segmentedOps().indexOf(version);
  if (0 == index) {
    return std::unexpected{
        AdvanceError{.kind = AdvanceError::Kind::UnknownVersion}};
  }
  const auto *const node = store.getCompactOp(index);
  if (nullptr == node) {
    return std::unexpected{
        AdvanceError{.kind = AdvanceError::Kind::MissingNode}};
  }
  return applyStructure(index, *node)
      .transform_error([](const FoldRefusal why) {
        return AdvanceError{.kind    = AdvanceError::Kind::Refused,
                            .refusal = why};
      });
}

void Manifold::compact() {
  std::vector<xanadu::PrimediaSpan> tightContent;
  tightContent.reserve(liveContent);
  for (auto &cell : slots) {
    const std::size_t offset = cell.spanOffset;
    cell.spanOffset          = static_cast<std::uint32_t>(tightContent.size());
    for (std::uint16_t i = 0; i < cell.spanCount; i++) {
      tightContent.push_back(content[offset + i]);
    }
  }
  content.swap(tightContent);
  liveContent = content.size();

  std::vector<DimLink> tight;
  tight.reserve(liveLinks);
  for (auto &cell : slots) {
    const std::size_t offset = cell.linkOffset;
    cell.linkOffset          = static_cast<std::uint32_t>(tight.size());
    for (std::uint16_t i = 0; i < cell.linkCount; i++) {
      tight.push_back(links_[offset + i]);
    }
  }
  links_.swap(tight);
  liveLinks = links_.size();
}

CellRef Manifold::linked(const CellRef from, const DimRef dim,
                         const DimVector dir) const noexcept {
  const auto dense = denseOf(from);
  if (noDense == dense) {
    return noCell;
  }
  const auto &cell = slots[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    const auto &link = links_[static_cast<std::size_t>(cell.linkOffset) + i];
    if (link.dim == dim) {
      return link.neighbor(dir);
    }
  }
  return noCell;
}

std::span<const DimLink>
Manifold::dimensionsOf(const CellRef ref) const noexcept {
  const auto dense = denseOf(ref);
  if (noDense == dense) {
    return {};
  }
  const auto &cell = slots[dense];
  return std::span<const DimLink>{links_.data() + cell.linkOffset,
                                  cell.linkCount};
}

std::span<const DimRef> Manifold::dimensions() const {
  if (!dimsCacheStale) {
    return dimsCache;
  }
  dimsCache.clear();
  if (noCell != home_) {
    // From home rather than from its neighbour, so the ring guard is what
    // stops the walk at home -- the rank is a ring through it.
    std::ranges::copy(rankAfter(*this, home_, dimsDim_),
                      std::back_inserter(dimsCache));
  }
  dimsCacheStale = false;
  return dimsCache;
}

std::optional<DimRef>
Manifold::dimensionNamed(const std::string_view name,
                         const xanadu::SpanReader &reader) const {
  const auto held = [this](const DimRef dim) {
    return contains(dim) ? std::optional{dim} : std::nullopt;
  };
  if (nullptr != store_) {
    if (const auto fast =
            DimensionRegistry::instance().get(*store_, name).and_then(held)) {
      return fast;
    }
  }
  auto named = dimensions() | std::views::filter([&](const DimRef dim) {
                 return textOf(dim, reader) == name;
               });
  return firstOf(named).transform([&](const DimRef dim) {
    if (nullptr != store_) {
      DimensionRegistry::instance().registerDim(*store_, name, dim);
    }
    return dim;
  });
}

std::optional<DimRef>
Manifold::dimensionNamed(const std::string_view name) const {
  if (nullptr == store_) {
    return std::nullopt;
  }
  return dimensionNamed(name, *store_);
}

std::string Manifold::textOf(const CellRef ref,
                             const xanadu::SpanReader &reader) const {
  std::string out;
  for (const auto &span : contentOf(ref)) {
    out += reader.read(span);
  }
  return out;
}

std::vector<std::uint32_t> Manifold::historyOf(const CellRef cell) const {
  if (nullptr == store_) {
    return {};
  }
  const auto *const cellSlot = slot(cell);
  if (nullptr == cellSlot) {
    return {};
  }

  std::vector<std::uint32_t> chain;
  auto curr                  = cellSlot->lastOp;
  const auto birth           = cellSlot->birthOp;
  const std::size_t maxSteps = static_cast<std::size_t>(foldedThrough_) + 1;

  while (curr != 0 && chain.size() <= maxSteps) {
    chain.push_back(curr);
    if (curr == birth) {
      break;
    }
    const auto *const node = store_->getCompactOp(curr);
    if (nullptr == node || xanadu::OpKind::Structure != node->kind ||
        node->sourceOpIndex >= curr) {
      return {};
    }
    curr = node->sourceOpIndex;
  }

  if (curr != birth) {
    return {};
  }

  std::reverse(chain.begin(), chain.end());
  return chain;
}

std::vector<xanadu::PrimediaSpan>
Manifold::contentAsOf(const CellRef cell, const std::uint32_t op) const {
  const auto history = historyOf(cell);
  if (history.empty()) {
    return {};
  }

  const auto it = std::find(history.begin(), history.end(), op);
  if (it == history.end()) {
    return {};
  }

  bool hasSplice = false;
  for (auto curIt = history.begin(); curIt <= it; ++curIt) {
    const auto *const node = store_->getCompactOp(*curIt);
    if (nullptr == node) {
      return {};
    }
    if (xanadu::StructureVerb::Splice == xanadu::structureVerbOf(node->flags)) {
      hasSplice = true;
      break;
    }
  }

  if (!hasSplice) {
    for (auto curIt = it;; --curIt) {
      const auto *const node = store_->getCompactOp(*curIt);
      if (nullptr == node) {
        return {};
      }
      const auto verb = xanadu::structureVerbOf(node->flags);
      if (verb == xanadu::StructureVerb::MakeCell ||
          verb == xanadu::StructureVerb::SetValue) {
        if (!node->span().empty()) {
          return {node->span()};
        }
        return {};
      }
      if (curIt == history.begin()) {
        break;
      }
    }
    return {};
  }

  std::vector<xanadu::PrimediaSpan> currentContent;
  for (auto curIt = history.begin(); curIt <= it; ++curIt) {
    const auto *const node = store_->getCompactOp(*curIt);
    if (nullptr == node) {
      return {};
    }
    const auto verb = xanadu::structureVerbOf(node->flags);
    switch (verb) {
    case xanadu::StructureVerb::MakeCell:
    case xanadu::StructureVerb::SetValue: {
      if (!node->span().empty()) {
        currentContent = {node->span()};
      } else {
        currentContent.clear();
      }
      break;
    }
    case xanadu::StructureVerb::SetLink:
      break;
    case xanadu::StructureVerb::Splice: {
      std::vector<xanadu::PrimediaSpan> rebuilt;
      rebuilt.reserve(currentContent.size() + 2);
      std::uint64_t seen   = 0;
      const auto at        = node->at;
      const auto removing  = node->length;
      const auto &inserted = node->span();
      for (const auto &piece : currentContent) {
        const auto pieceEnd = seen + piece.length;
        if (seen < at) {
          rebuilt.push_back(piece.slice(0, std::min(piece.length, at - seen)));
        }
        const auto removedEnd = at + removing;
        if (pieceEnd > removedEnd) {
          const auto from = removedEnd > seen ? removedEnd - seen : 0;
          rebuilt.push_back(piece.slice(from, piece.length - from));
        }
        seen = pieceEnd;
      }
      if (!inserted.empty()) {
        std::uint64_t upTo = 0;
        std::size_t where  = 0;
        for (; where < rebuilt.size() && upTo < at; where++) {
          upTo += rebuilt[where].length;
        }
        rebuilt.insert(rebuilt.begin() + static_cast<std::ptrdiff_t>(where),
                       inserted);
      }
      std::erase_if(rebuilt, [](const auto &piece) { return piece.empty(); });
      for (std::size_t i = 0; i + 1 < rebuilt.size();) {
        if (rebuilt[i].scroll == rebuilt[i + 1].scroll &&
            rebuilt[i].end() == rebuilt[i + 1].start) {
          rebuilt[i].length += rebuilt[i + 1].length;
          rebuilt.erase(rebuilt.begin() + static_cast<std::ptrdiff_t>(i) + 1);
          continue;
        }
        i++;
      }
      currentContent = std::move(rebuilt);
      break;
    }
    }
  }

  return currentContent;
}

xanadu::ValueKind Manifold::valueKindOf(const CellRef ref) const noexcept {
  return findSlot(ref)
      .transform([](const CellSlot &cell) noexcept {
        return static_cast<xanadu::ValueKind>(cell.valueKind);
      })
      .value_or(xanadu::ValueKind::None);
}

std::optional<double> Manifold::asDouble(const CellRef ref) const noexcept {
  const auto cell = findSlot(ref);
  if (!cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Double)) {
    return std::nullopt;
  }
  return std::bit_cast<double>(cell->valueBits);
}

std::optional<bool> Manifold::asBool(const CellRef ref) const noexcept {
  const auto cell = findSlot(ref);
  if (!cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Bool)) {
    return std::nullopt;
  }
  return 0 != cell->valueBits;
}

std::optional<std::int64_t>
Manifold::asInt64(const CellRef ref) const noexcept {
  const auto cell = findSlot(ref);
  if (!cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Int64)) {
    return std::nullopt;
  }
  return std::bit_cast<std::int64_t>(cell->valueBits);
}

std::optional<CellRef>
Manifold::handleTarget(const CellRef ref) const noexcept {
  const auto cell = findSlot(ref);
  if (!cell || cell->valueKind !=
                   static_cast<std::uint8_t>(xanadu::ValueKind::OpHandle)) {
    return std::nullopt;
  }
  return static_cast<CellRef>(cell->valueBits);
}

std::optional<CellRef>
Manifold::cloneMaster(const CellRef ref, const DimRef cloneDim) const noexcept {
  return rankEnd(*this, ref, cloneDim, DimVector::NEG);
}

bool Manifold::equivalentTo(const Manifold &other) const {
  if (slots.size() != other.slots.size() || home_ != other.home_ ||
      dimsDim_ != other.dimsDim_) {
    return false;
  }
  for (const auto &cell : slots) {
    const auto *const theirs = other.slot(cell.birthOp);
    if (nullptr == theirs || theirs->birthOp != cell.birthOp ||
        theirs->lastOp != cell.lastOp ||
        !std::ranges::equal(contentOf(cell.birthOp),
                            other.contentOf(cell.birthOp)) ||
        theirs->valueKind != cell.valueKind ||
        theirs->valueBits != cell.valueBits) {
      return false;
    }
    // By dimension rather than by position: an arena laid out by a different
    // sequence of appends and relocations is the same manifold, and offsets
    // are exactly what this comparison must not be sensitive to.
    auto mine  = std::vector<DimLink>{dimensionsOf(cell.birthOp).begin(),
                                      dimensionsOf(cell.birthOp).end()};
    auto yours = std::vector<DimLink>{other.dimensionsOf(cell.birthOp).begin(),
                                      other.dimensionsOf(cell.birthOp).end()};
    const auto byDim = [](const DimLink &lhs, const DimLink &rhs) {
      return lhs.dim < rhs.dim;
    };
    std::ranges::sort(mine, byDim);
    std::ranges::sort(yours, byDim);
    // A dimension a cell links on but holds nothing along is a run entry that
    // says nothing, which one fold can have and an equivalent one need not.
    const auto empty = [](const DimLink &link) {
      return noCell == link.pos && noCell == link.neg;
    };
    std::erase_if(mine, empty);
    std::erase_if(yours, empty);
    if (mine != yours) {
      return false;
    }
  }
  return true;
}

std::vector<CellRef> Manifold::cellsWithinRadius(CellRef start,
                                                 const int radius) const {
  if (slots.empty()) {
    return {};
  }
  if (radius < 0) {
    std::vector<CellRef> allCells;
    allCells.reserve(slots.size());
    for (const auto &cell : slots) {
      allCells.push_back(cell.birthOp);
    }
    return allCells;
  }

  CellRef root = start;
  if (noCell == root || !contains(root)) {
    root = (home_ != noCell && contains(home_)) ? home_ : slots.front().birthOp;
  }

  std::vector<CellRef> ordered;
  std::unordered_set<CellRef> visited;
  std::vector<std::pair<CellRef, int>> queue;
  queue.reserve(64);

  visited.insert(root);
  queue.emplace_back(root, 0);
  ordered.push_back(root);

  std::size_t head = 0;
  while (head < queue.size()) {
    const auto [curr, dist] = queue[head++];
    if (dist >= radius) {
      continue;
    }

    for (const auto &dimLink : dimensionsOf(curr)) {
      if (dimLink.pos != noCell && contains(dimLink.pos) &&
          visited.insert(dimLink.pos).second) {
        ordered.push_back(dimLink.pos);
        queue.emplace_back(dimLink.pos, dist + 1);
      }
      if (dimLink.neg != noCell && contains(dimLink.neg) &&
          visited.insert(dimLink.neg).second) {
        ordered.push_back(dimLink.neg);
        queue.emplace_back(dimLink.neg, dist + 1);
      }
    }
  }

  return ordered;
}

std::unordered_set<CellRef>
Manifold::cellsWithinRadiusSet(CellRef start, const int radius) const {
  const auto list = cellsWithinRadius(start, radius);
  return std::unordered_set<CellRef>{list.begin(), list.end()};
}

void Manifold::setFormatFlags(const CellRef ref,
                              const std::uint16_t flags) noexcept {
  const auto dense = denseOf(ref);
  if (dense < slots.size()) {
    slots[dense].formatFlags = flags;
  }
}

bool Manifold::verifyAgainstFullRebuild(const xanadu::Store &store) const {
  const auto cold = store.rebuildManifoldFromIndex(foldedThrough_);
  if (!equivalentTo(cold)) {
    return false;
  }
  const auto regWarm = scrollRegistry(store);
  const auto regCold = cold.scrollRegistry(store);
  if (regWarm.scrolls != regCold.scrolls || regWarm.byKey != regCold.byKey ||
      regWarm.byCell != regCold.byCell) {
    return false;
  }
  const auto linksWarm = links(store);
  const auto linksCold = cold.links(store);
  if (linksWarm != linksCold) {
    return false;
  }
  return true;
}

std::vector<Manifold::Edition> Manifold::editions() const {
  if (nullptr == store_) {
    return {};
  }
  const auto home = store_->homeCell();
  if (noCell == home) {
    return {};
  }
  const auto dimEditions = dimensionNamed("d.editions", *store_);
  if (!dimEditions) {
    return {};
  }
  const auto dimEditionOf = dimensionNamed("d.edition-of", *store_);
  if (!dimEditionOf) {
    return {};
  }

  std::vector<Edition> result;
  for (const auto cell : rankAfter(*this, home, *dimEditions)) {
    const auto handle =
        step(*this, cell, *dimEditionOf, DimVector::POS)
            .or_else([&] {
              return step(*this, cell, *dimEditionOf, DimVector::NEG);
            })
            .value_or(noCell);
    const auto target = handleTarget(handle);
    result.push_back(Edition{
        .cell     = cell,
        .name     = textOf(cell, *store_),
        .handle   = handle,
        .targetOp = target.value_or(0),
    });
  }

  return result;
}

std::optional<Manifold::Edition>
Manifold::editionNamed(const std::string_view name) const {
  for (auto &ed : editions()) {
    if (ed.name == name) {
      return ed;
    }
  }
  return std::nullopt;
}

std::optional<CellRef>
Manifold::findOpHandle(const std::uint32_t targetOp) const noexcept {
  for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
    if (it->valueKind ==
            static_cast<std::uint8_t>(xanadu::ValueKind::OpHandle) &&
        it->valueBits == targetOp) {
      return it->birthOp;
    }
  }
  return std::nullopt;
}

std::optional<xanadu::VersionAnnotation>
Manifold::versionAnnotationForHandle(const CellRef handle,
                                     const xanadu::Store &store) const {
  xanadu::VersionAnnotation ann;
  const auto dimNotes   = dimensionNamed("d.notes", store);
  const auto dimTag     = dimensionNamed("d.tag", store);
  const auto dimAlias   = dimensionNamed("d.alias", store);
  const auto dimCreated = dimensionNamed("d.created", store);

  const auto prop =
      [&](const std::optional<DimRef> &dim) -> std::optional<std::string> {
    if (!dim) {
      return std::nullopt;
    }
    return step(*this, handle, *dim, DimVector::POS)
        .or_else([&] { return step(*this, handle, *dim, DimVector::NEG); })
        .transform([&](const CellRef c) { return textOf(c, store); });
  };

  if (const auto desc = prop(dimNotes)) {
    ann.description = *desc;
  }
  if (const auto tag = prop(dimTag)) {
    ann.tag = *tag;
  }
  if (const auto alias = prop(dimAlias)) {
    ann.alias = *alias;
  }
  if (const auto created = prop(dimCreated)) {
    ann.timestamp = *created;
  }

  // If alias was not directly on d.alias, check if an edition points to this
  // handle
  if (ann.alias.empty()) {
    for (const auto &ed : editions()) {
      if (ed.handle == handle && !ed.name.empty() && ed.name != "current") {
        ann.alias = ed.name;
        break;
      }
    }
  }

  if (ann.alias.empty() && ann.description.empty() && ann.tag.empty() &&
      ann.timestamp.empty()) {
    return std::nullopt;
  }
  return ann;
}

std::optional<xanadu::VersionAnnotation>
Manifold::versionAnnotation(const std::uint32_t targetOp,
                            const xanadu::Store &store) const {
  xanadu::VersionAnnotation combined;
  bool foundAny = false;
  for (auto it = slots.rbegin(); it != slots.rend(); ++it) {
    if (it->valueKind ==
            static_cast<std::uint8_t>(xanadu::ValueKind::OpHandle) &&
        it->valueBits == targetOp) {
      const auto ann = versionAnnotationForHandle(it->birthOp, store);
      if (ann.has_value()) {
        foundAny = true;
        if (combined.description.empty() && !ann->description.empty()) {
          combined.description = ann->description;
        }
        if (combined.tag.empty() && !ann->tag.empty()) {
          combined.tag = ann->tag;
        }
        if (combined.alias.empty() && !ann->alias.empty()) {
          combined.alias = ann->alias;
        }
        if (combined.timestamp.empty() && !ann->timestamp.empty()) {
          combined.timestamp = ann->timestamp;
        }
      }
    }
  }
  if (foundAny) {
    return combined;
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, CellRef>>
Manifold::aliases(const xanadu::Store &store) const {
  std::vector<std::pair<std::string, CellRef>> result;
  for (const auto &ed : editions()) {
    if (!ed.name.empty() && ed.name != "current" && ed.targetOp > 0) {
      result.emplace_back(ed.name, static_cast<CellRef>(ed.targetOp));
    }
  }
  const auto dimAlias = dimensionNamed("d.alias", store);
  if (dimAlias) {
    for (const auto &cell : slots) {
      if (cell.valueKind ==
              static_cast<std::uint8_t>(xanadu::ValueKind::OpHandle) &&
          cell.valueBits > 0) {
        const auto aliasCell =
            step(*this, cell.birthOp, *dimAlias, DimVector::POS).or_else([&] {
              return step(*this, cell.birthOp, *dimAlias, DimVector::NEG);
            });
        if (aliasCell) {
          const auto name = textOf(*aliasCell, store);
          if (!name.empty()) {
            result.emplace_back(name, static_cast<CellRef>(cell.valueBits));
          }
        }
      }
    }
  }
  return result;
}

ScrollRegistry
Manifold::scrollRegistry(const xanadu::SpanReader &reader) const {
  ScrollRegistry registry;
  if (nullptr == store_ && noCell == home_) {
    return registry;
  }
  const auto dimScrolls = store_ ? dimensionNamed("d.scrolls", *store_)
                                 : dimensionNamed("d.scrolls");
  if (!dimScrolls || noCell == home_) {
    return registry;
  }

  // 1. Collect all scroll cells on d.scrolls rank off home
  std::vector<CellRef> scrollCells;
  for (const auto cell : rankAfter(*this, home_, *dimScrolls)) {
    scrollCells.push_back(cell);
  }

  if (scrollCells.empty()) {
    return registry;
  }

  // 2. Rooted dependency walk (§4):
  // Local scroll 0 is rooted. Mapped non-zero scrolls make further cells
  // readable.
  std::unordered_set<CellRef> resolved;
  std::unordered_map<CellRef, std::string> cellKeys;
  std::unordered_set<xanadu::ScrollId> resolvedScrollIds;
  resolvedScrollIds.insert(xanadu::localScroll);

  bool progress = true;
  while (resolved.size() < scrollCells.size() && progress) {
    progress = false;
    for (std::size_t i = 0; i < scrollCells.size(); ++i) {
      const auto cell = scrollCells[i];
      if (resolved.contains(cell)) {
        continue;
      }
      const auto spans = contentOf(cell);
      bool canRead     = true;
      for (const auto &span : spans) {
        if (span.scroll != xanadu::localScroll &&
            !resolvedScrollIds.contains(span.scroll)) {
          canRead = false;
          break;
        }
      }
      if (canRead) {
        std::string key;
        try {
          key = textOf(cell, reader);
        } catch (...) {
          canRead = false;
        }
        if (canRead && !key.empty()) {
          resolved.insert(cell);
          cellKeys[cell] = std::move(key);
          resolvedScrollIds.insert(static_cast<xanadu::ScrollId>(i + 1));
          progress = true;
        }
      }
    }
  }

  if (resolved.size() < scrollCells.size()) {
    std::string unresolvedList;
    for (std::size_t i = 0; i < scrollCells.size(); ++i) {
      if (!resolved.contains(scrollCells[i])) {
        if (!unresolvedList.empty()) {
          unresolvedList += ", ";
        }
        unresolvedList += "cell " + std::to_string(scrollCells[i]) +
                          " (scroll id " + std::to_string(i + 1) + ")";
      }
    }
    throw UnrootedRegistryDependency("unrooted scroll registry dependency: " +
                                     unresolvedList);
  }

  // 3. Populate registry with sequential 1-based ScrollIds
  registry.scrolls.reserve(scrollCells.size());
  for (std::size_t i = 0; i < scrollCells.size(); ++i) {
    const auto cell = scrollCells[i];
    const auto id   = static_cast<xanadu::ScrollId>(i + 1);
    const auto &key = cellKeys[cell];
    registry.scrolls.push_back(ScrollRecord{
        .id        = id,
        .cell      = cell,
        .globalKey = key,
    });
    registry.byKey[key]   = id;
    registry.byCell[cell] = id;
  }

  // 4. Map placeholders along d.scroll-refs (§6)
  const auto dimScrollRefs = store_ ? dimensionNamed("d.scroll-refs", *store_)
                                    : dimensionNamed("d.scroll-refs");
  if (dimScrollRefs) {
    for (const auto &rec : registry.scrolls) {
      for (const auto p : rankAfter(*this, rec.cell, *dimScrollRefs)) {
        registry.byCell[p] = rec.id;
      }
    }
  }

  return registry;
}

ScrollRegistry Manifold::scrollRegistry() const {
  if (nullptr == store_) {
    return {};
  }
  return scrollRegistry(*store_);
}

std::vector<ScrollRecord> Manifold::scrolls() const {
  return scrollRegistry().scrolls;
}

std::map<CellRef, xanadu::Link>
Manifold::links(const xanadu::SpanReader &reader) const {
  std::map<CellRef, xanadu::Link> result;
  if (noCell == home_) {
    return result;
  }
  const auto dimLinks = dimensionNamed("d.links", reader);
  if (!dimLinks) {
    return result;
  }
  const auto dimFrom    = dimensionNamed("d.from", reader);
  const auto dimTo      = dimensionNamed("d.to", reader);
  const auto dimType    = dimensionNamed("d.linktype", reader);
  const auto dimTier    = dimensionNamed("d.linktier", reader);
  const auto dimOwner   = dimensionNamed("d.owner", reader);
  const auto dimCurator = dimensionNamed("d.curator", reader);

  for (const auto cell : rankAfter(*this, home_, *dimLinks)) {
    xanadu::Link link;
    link.id = cell;

    const auto readEndpoint = [&](const std::optional<DimRef> &dim) {
      return dim
          .and_then(
              [this, cell](const DimRef d) { return step(*this, cell, d); })
          .transform([this](const CellRef c) {
            const auto spans = contentOf(c);
            return std::vector<xanadu::PrimediaSpan>(spans.begin(),
                                                     spans.end());
          })
          .value_or(std::vector<xanadu::PrimediaSpan>{});
    };

    const auto readProp =
        [&](const std::optional<DimRef> &dim) -> std::optional<std::string> {
      return dim
          .and_then(
              [this, cell](const DimRef d) { return step(*this, cell, d); })
          .and_then(
              [this, &reader](const CellRef c) -> std::optional<std::string> {
                try {
                  return textOf(c, reader);
                } catch (...) {
                  return std::nullopt;
                }
              });
    };

    link.left    = readEndpoint(dimFrom);
    link.right   = readEndpoint(dimTo);
    link.type    = readProp(dimType)
                       .transform(xanadu::linkTypeFromName)
                       .value_or(xanadu::LinkType::Comment);
    link.tier    = readProp(dimTier)
                       .transform(xanadu::prominenceTierFromName)
                       .value_or(xanadu::ProminenceTier::Author);
    link.owner   = readProp(dimOwner).value_or("");
    link.curator = readProp(dimCurator).value_or("");

    result.emplace(cell, std::move(link));
  }

  return result;
}

std::map<CellRef, xanadu::Link> Manifold::links() const {
  if (nullptr == store_) {
    return {};
  }
  return links(*store_);
}

} // namespace zigzag
