#include "common/xanadu/zigzag/manifold.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>

#include "common/xanadu/store.hpp"

namespace zigzag {

namespace {

constexpr auto noDense = std::numeric_limits<std::uint32_t>::max();

/// Dead runs the arena will carry before compact() is worth doing. Bounded by
/// a multiple of what is live rather than by an absolute size, so a slice being
/// built pays a compaction a bounded number of times however large it gets.
constexpr std::size_t compactionSlack = 64;

} // namespace

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

DimLink *Manifold::existingLink(const std::uint32_t dense,
                                const DimRef dim) noexcept {
  const auto &cell = slots[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    if (links[static_cast<std::size_t>(cell.linkOffset) + i].dim == dim) {
      return &links[static_cast<std::size_t>(cell.linkOffset) + i];
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
  if (links.size() > 2 * liveLinks + compactionSlack) {
    compact();
  }

  auto &cell = slots[dense];
  if (cell.linkCount == std::numeric_limits<std::uint16_t>::max()) {
    return nullptr;
  }

  const std::size_t offset = cell.linkOffset;
  const std::size_t count  = cell.linkCount;
  if (offset + count != links.size()) {
    // Not the arena's tail, so the run cannot simply be extended. Copy it to
    // the end and leave the old one dead for compact() to reclaim. Reserving
    // first is what makes the push_backs below safe to source from the same
    // vector, and geometric so that relocating repeatedly stays linear.
    if (links.capacity() < links.size() + count + 1) {
      links.reserve(std::max(links.size() * 2, links.size() + count + 1));
    }
    const auto relocated = links.size();
    for (std::size_t i = 0; i < count; i++) {
      links.push_back(links[offset + i]);
    }
    cell.linkOffset = static_cast<std::uint32_t>(relocated);
  }

  links.push_back(DimLink{.dim = dim, .pos = noCell, .neg = noCell});
  cell.linkCount++;
  liveLinks++;
  return &links.back();
}

void Manifold::setOneSide(const std::uint32_t dense, const DimRef dim,
                          const bool negward, const CellRef to) {
  DimLink *const link = linkFor(dense, dim);
  if (nullptr == link) {
    return;
  }
  if (negward) {
    link->neg = to;
  } else {
    link->pos = to;
  }
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
  std::copy(spans.begin(), spans.end(), content.begin() + cell.spanOffset);
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

void Manifold::applyStructure(const std::uint32_t opIndex,
                              const xanadu::CompactOpNode &node) noexcept {
  if (xanadu::OpKind::Structure != node.kind) {
    return;
  }
  if (opIndex > foldedThrough_) {
    foldedThrough_ = opIndex;
  }

  // R8's boundary on the *address* side, and the twin of the isEphemeral()
  // check the SetLink case makes on cell refs. A span in the scratch scroll
  // names bytes an ArenaManifold constructed and has no permanent address, so a
  // persisted cell quoting one would be a transclusion into a scroll that does
  // not exist -- and one span is structurally identical to another, so nothing
  // downstream could tell. promote() is the only thing that turns scratch bytes
  // into an address; everything else is refused here. See spool.hpp's
  // scratchScroll and design/vlog-logic-extension.md §5.5.
  if (xanadu::scratchScroll == node.span().scroll) {
    refusedOps_++;
    return;
  }

  switch (xanadu::structureVerbOf(node.flags)) {
  case xanadu::StructureVerb::MakeCell: {
    if (byRef.contains(opIndex)) {
      refusedOps_++;
      return;
    }
    const auto dense = static_cast<std::uint32_t>(slots.size());
    // The empty runs start at the arenas' tails, so this cell's first link and
    // first span are appends in place rather than relocations.
    slots.push_back(CellSlot{
        .spanOffset = static_cast<std::uint32_t>(content.size()),
        .spanCount  = 0,
        .birthOp    = opIndex,
        .lastOp     = opIndex,
        .linkOffset = static_cast<std::uint32_t>(links.size()),
        .linkCount  = 0,
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
    return;
  }

  case xanadu::StructureVerb::SetLink: {
    // The subject is named by the micro-history chain rather than by a field of
    // its own: sourceOpIndex is the previous operation on this same cell, and
    // the chain's first link is the MakeCell whose index *is* the CellRef. See
    // R7, and byRef, which holds every operation in a chain for exactly this.
    const auto dense = denseOf(node.sourceOpIndex);
    const DimRef dim = node.linkId;
    const CellRef to = node.to;
    if (noDense == dense || noDense == denseOf(dim)) {
      refusedOps_++;
      return;
    }
    // R8's boundary as a bit: a link into a derived cell cannot be persisted,
    // and a stored link naming a cell this fold does not hold would be a
    // traversal walking into nothing.
    if (isEphemeral(to) || (noCell != to && noDense == denseOf(to))) {
      refusedOps_++;
      return;
    }

    const CellRef self  = slots[dense].birthOp;
    const bool negward  = xanadu::structureIsNegward(node.flags);
    CellRef displacedUs = noCell;
    CellRef displacedIt = noCell;
    // Read both sides out before touching anything: setOneSide() can grow a
    // run, and growing a run can move every DimLink in the arena.
    if (const DimLink *const mine = existingLink(dense, dim); nullptr != mine) {
      displacedUs = negward ? mine->neg : mine->pos;
    }
    const auto target = denseOf(to);
    if (noDense != target) {
      if (const DimLink *const theirs = existingLink(target, dim);
          nullptr != theirs) {
        displacedIt = negward ? theirs->pos : theirs->neg;
      }
    }

    // A link is one edge shared by two cells, so setting it breaks whatever
    // each end was holding: the invariant this maintains is that
    // linked(a, d, dir) == b exactly when linked(b, d, !dir) == a. zzcore's
    // loader derives the same backlinks; here it has to be maintained rather
    // than derived, because an op arrives one at a time.
    if (noCell != displacedUs && displacedUs != to) {
      if (const auto other = denseOf(displacedUs); noDense != other) {
        setOneSide(other, dim, !negward, noCell);
      }
    }
    if (noCell != displacedIt && displacedIt != self) {
      if (const auto other = denseOf(displacedIt); noDense != other) {
        setOneSide(other, dim, negward, noCell);
      }
    }
    if (noDense != target) {
      setOneSide(target, dim, !negward, self);
    }
    setOneSide(dense, dim, negward, to);

    slots[dense].lastOp = opIndex;
    byRef.emplace(opIndex, dense);
    dimsCacheStale = true;
    return;
  }

  case xanadu::StructureVerb::Splice: {
    const auto dense = denseOf(node.sourceOpIndex);
    if (noDense == dense) {
      refusedOps_++;
      return;
    }
    spliceContent(dense, node.at, node.length, node.span());
    slots[dense].lastOp = opIndex;
    byRef.emplace(opIndex, dense);
    return;
  }

  case xanadu::StructureVerb::SetValue: {
    const auto dense = denseOf(node.sourceOpIndex);
    if (noDense == dense) {
      refusedOps_++;
      return;
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
    return;
  }
  }

  // A verb this build does not know. Counted rather than ignored: the whole
  // point of refusedOps() is that a fold cannot throw and still must not
  // silently mean something else.
  refusedOps_++;
}

bool Manifold::advance(const xanadu::Store &store,
                       const xanadu::MicroversionId &version) {
  const auto index = store.segmentedOps().indexOf(version);
  if (0 == index) {
    return false;
  }
  const auto *const node = store.getCompactOp(index);
  if (nullptr == node) {
    return false;
  }
  applyStructure(index, *node);
  return true;
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
      tight.push_back(links[offset + i]);
    }
  }
  links.swap(tight);
  liveLinks = links.size();
}

CellRef Manifold::linked(const CellRef from, const DimRef dim,
                         const bool negward) const noexcept {
  const auto dense = denseOf(from);
  if (noDense == dense) {
    return noCell;
  }
  const auto &cell = slots[dense];
  for (std::uint16_t i = 0; i < cell.linkCount; i++) {
    const auto &link = links[static_cast<std::size_t>(cell.linkOffset) + i];
    if (link.dim == dim) {
      return negward ? link.neg : link.pos;
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
  return std::span<const DimLink>{links.data() + cell.linkOffset,
                                  cell.linkCount};
}

std::span<const DimRef> Manifold::dimensions() const {
  if (!dimsCacheStale) {
    return dimsCache;
  }
  dimsCache.clear();
  if (noCell != home_ && noCell != dimsDim_) {
    // A rank that loops -- which zzstructure allows -- would otherwise be
    // walked forever. Bounded by the cell count rather than by a visited set
    // so that the walk allocates nothing beyond the answer.
    CellRef cursor = linked(home_, dimsDim_, false);
    for (std::size_t step = 0; noCell != cursor && step <= slots.size();
         step++) {
      dimsCache.push_back(cursor);
      cursor = linked(cursor, dimsDim_, false);
      if (cursor == home_) {
        break;
      }
    }
  }
  dimsCacheStale = false;
  return dimsCache;
}

DimRef Manifold::dimensionNamed(const std::string_view name,
                                const xanadu::SpanReader &reader) const {
  for (const auto dim : dimensions()) {
    if (textOf(dim, reader) == name) {
      return dim;
    }
  }
  return noCell;
}

std::string Manifold::textOf(const CellRef ref,
                             const xanadu::SpanReader &reader) const {
  std::string out;
  for (const auto &span : contentOf(ref)) {
    out += reader.read(span);
  }
  return out;
}

xanadu::ValueKind Manifold::valueKindOf(const CellRef ref) const noexcept {
  const auto *const cell = slot(ref);
  return nullptr == cell ? xanadu::ValueKind::None
                         : static_cast<xanadu::ValueKind>(cell->valueKind);
}

std::optional<double> Manifold::asDouble(const CellRef ref) const noexcept {
  const auto *const cell = slot(ref);
  if (nullptr == cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Double)) {
    return std::nullopt;
  }
  return std::bit_cast<double>(cell->valueBits);
}

std::optional<bool> Manifold::asBool(const CellRef ref) const noexcept {
  const auto *const cell = slot(ref);
  if (nullptr == cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Bool)) {
    return std::nullopt;
  }
  return 0 != cell->valueBits;
}

std::optional<std::int64_t>
Manifold::asInt64(const CellRef ref) const noexcept {
  const auto *const cell = slot(ref);
  if (nullptr == cell ||
      cell->valueKind != static_cast<std::uint8_t>(xanadu::ValueKind::Int64)) {
    return std::nullopt;
  }
  return std::bit_cast<std::int64_t>(cell->valueBits);
}

CellRef Manifold::cloneMaster(const CellRef ref,
                              const DimRef cloneDim) const noexcept {
  if (noDense == denseOf(ref)) {
    return noCell;
  }
  CellRef cursor = ref;
  for (std::size_t step = 0; step <= slots.size(); step++) {
    const CellRef master = linked(cursor, cloneDim, true);
    if (noCell == master || master == cursor) {
      return cursor;
    }
    cursor = master;
  }
  return cursor;
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

bool Manifold::verifyAgainstFullRebuild(const xanadu::Store &store) const {
  return equivalentTo(store.rebuildManifoldFromIndex(foldedThrough_));
}

} // namespace zigzag
