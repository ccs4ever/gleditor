/**
 * @file unified_transclusion_engine.cpp
 * @brief Implementation of UnifiedTransclusionEngine.
 */
#include "unified_transclusion_engine.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <queue>
#include <set>
#include <utility>

namespace zigzag {

UnifiedTransclusionEngine::UnifiedTransclusionEngine(xanadu::Store &store)
    : store_(store) {
  syncIncremental();
}

void UnifiedTransclusionEngine::syncIncremental() {
  // A fold, not a projection. buildCellFromOp() used to synthesise a cell from
  // *every* operation -- an Insert included -- and then hand-build d.ops_time,
  // d.ops_dag and d.transclude ranks by writing into those cells directly. None
  // of that can survive a model where a CellRef is the index of the operation
  // that minted the cell: a text insert mints no cell, and a rank is structure,
  // which is now an operation rather than a field somebody assigns.
  //
  // So a xanadoc's pieces become cells only when something mints them --
  // sliceToStore() in zz_xudu_projector, or the makeCell/setLink verbs -- and
  // this walks new operations handing each to the manifold, which ignores the
  // kinds that are not Structure. §7's "d.transclude maintained as a stored
  // rank at write time" is the same point from the other side: the rank is
  // minted where the transclusion is recorded, not invented where it is
  // displayed.
  const auto &ops  = store_.segmentedOps();
  const auto total = static_cast<std::uint32_t>(ops.size());
  for (auto idx = lastSyncedOpIndex_ + 1; idx <= total; idx++) {
    if (const auto *const node = ops.get(idx); nullptr != node) {
      manifold_.applyStructure(idx, *node);
    }
  }
  lastSyncedOpIndex_ = total;
  if (total > 0) {
    head_ = ops.idOf(total);
  }
}

void UnifiedTransclusionEngine::ensureSliceBegun() {
  if (zigzag::noCell != store_.homeCell()) {
    return;
  }
  head_ = store_.sliceGenesis(head_);
  syncIncremental();
}

DimRef UnifiedTransclusionEngine::dimensionFor(const std::string_view name) {
  if (const auto found = manifold_.dimensionNamed(name, store_);
      zigzag::noCell != found) {
    return found;
  }
  ensureSliceBegun();
  const auto minted = store_.makeDimension(head_, name, &manifold_);
  head_             = minted.version;
  syncIncremental();
  return minted.dim;
}

CellRef UnifiedTransclusionEngine::addCell(const std::string_view text) {
  // Minting, where this used to be filing: a cell cannot exist without the
  // operation that names it, so adding one is recording one.
  ensureSliceBegun();
  head_ = store_.makeCell(head_, text);
  syncIncremental();
  return store_.cellRefOf(head_);
}

void UnifiedTransclusionEngine::updateCellText(const CellRef cell,
                                               const std::string_view text) {
  if (zigzag::noCell == cell) {
    return;
  }
  ensureSliceBegun();
  head_ = store_.setCellText(head_, cell, text, &manifold_);
  syncIncremental();
  clearShapingCache();
}

void UnifiedTransclusionEngine::setCold(const CellRef cell, ColdCell cold) {
  cold_[cell] = std::move(cold);
}

const UnifiedTransclusionEngine::ColdCell *
UnifiedTransclusionEngine::coldOf(const CellRef cell) const noexcept {
  const auto found = cold_.find(cell);
  return found == cold_.end() ? nullptr : &found->second;
}

void UnifiedTransclusionEngine::linkCells(const CellRef a, const CellRef b,
                                          const DimRef dim,
                                          const bool negward) {
  if (zigzag::noCell == a || zigzag::noCell == dim) {
    return;
  }
  // One operation, not two writes. The reciprocal edge is what the fold means
  // by a link rather than a second thing to remember to set -- which is what
  // the four-line pos-then-neg dance this replaces kept getting right by hand.
  head_ = store_.setLink(head_, a, dim, negward, b, &manifold_);
  syncIncremental();
}

void UnifiedTransclusionEngine::linkCells(const CellRef a, const CellRef b,
                                          const DimOrdinal dim,
                                          const bool negward) {
  linkCells(a, b, dimensionFor(dimOrdinalToString(dim)), negward);
}

void UnifiedTransclusionEngine::linkCells(const CellRef a, const CellRef b,
                                          const DimID &dim,
                                          const bool negward) {
  linkCells(a, b, dimensionFor(dim), negward);
}

void UnifiedTransclusionEngine::unlinkPositive(const CellRef a,
                                               const DimOrdinal dim) {
  linkCells(a, zigzag::noCell, dim, false);
}

const CellSlot *
UnifiedTransclusionEngine::findCell(const CellRef cell) const noexcept {
  if (isEphemeral(cell)) {
    const auto it = ephemeralSlots_.find(cell);
    if (it != ephemeralSlots_.end()) {
      if (const auto *masterSlot = manifold_.slot(it->second.dimension)) {
        ephemeralCellSlotDummy_         = *masterSlot;
        ephemeralCellSlotDummy_.birthOp = cell;
        return &ephemeralCellSlotDummy_;
      }
    }
    return nullptr;
  }
  return manifold_.slot(cell);
}

std::vector<DimRef>
UnifiedTransclusionEngine::metaDimensionsOf(const CellRef cell) const {
  std::vector<DimRef> dims;
  if (cell == noCell) {
    return dims;
  }
  for (const auto &link : manifold_.dimensionsOf(cell)) {
    if (link.pos != noCell || link.neg != noCell) {
      if (std::ranges::find(dims, link.dim) == dims.end()) {
        dims.push_back(link.dim);
      }
    }
  }
  const auto metaDim =
      const_cast<UnifiedTransclusionEngine *>(this)->dimensionFor(
          "d.meta-dims");
  if (metaDim != noCell) {
    if (std::ranges::find(dims, metaDim) == dims.end()) {
      dims.push_back(metaDim);
    }
  }
  return dims;
}

CellRef UnifiedTransclusionEngine::getOrCreateEphemeralCell(
    const CellRef parent, const std::size_t index, const DimRef dim,
    const std::size_t total) const {
  const auto key = std::pair{parent, index};
  if (const auto it = ephemeralByParentAndIndex_.find(key);
      it != ephemeralByParentAndIndex_.end()) {
    return it->second;
  }
  const CellRef ephId    = ephemeralBit | nextEphemeralId_++;
  ephemeralSlots_[ephId] = EphemeralMetaDimSlot{
      .parentCell = parent,
      .dimension  = dim,
      .index      = index,
      .totalCount = total,
  };
  ephemeralByParentAndIndex_[key] = ephId;
  return ephId;
}

CellRef UnifiedTransclusionEngine::linked(const CellRef from, const DimRef dim,
                                          const bool negward) const {
  if (from == noCell || dim == noCell) {
    return noCell;
  }

  const auto metaDim =
      const_cast<UnifiedTransclusionEngine *>(this)->dimensionFor(
          "d.meta-dims");
  const auto cloneDim = manifold_.dimensionNamed("d.clone", store_);

  if (isEphemeral(from)) {
    const auto it = ephemeralSlots_.find(from);
    if (it == ephemeralSlots_.end()) {
      return noCell;
    }
    const auto &slot = it->second;

    if (metaDim != noCell && dim == metaDim) {
      if (!negward) {
        if (slot.index + 1 < slot.totalCount) {
          const auto mDims = metaDimensionsOf(slot.parentCell);
          if (slot.index + 1 < mDims.size()) {
            return getOrCreateEphemeralCell(slot.parentCell, slot.index + 1,
                                            mDims[slot.index + 1],
                                            slot.totalCount);
          }
        }
        return noCell;
      } else {
        if (slot.index > 0) {
          const auto mDims = metaDimensionsOf(slot.parentCell);
          if (slot.index - 1 < mDims.size()) {
            return getOrCreateEphemeralCell(slot.parentCell, slot.index - 1,
                                            mDims[slot.index - 1],
                                            slot.totalCount);
          }
        }
        return slot.parentCell;
      }
    }

    if (cloneDim != noCell && dim == cloneDim) {
      if (negward) {
        return slot.dimension;
      }
      return noCell;
    }

    return noCell;
  }

  if (metaDim != noCell && dim == metaDim) {
    if (!negward) {
      const auto mDims = metaDimensionsOf(from);
      if (!mDims.empty()) {
        return getOrCreateEphemeralCell(from, 0, mDims[0], mDims.size());
      }
    }
    return noCell;
  }

  return manifold_.linked(from, dim, negward);
}

CellRef UnifiedTransclusionEngine::cloneMaster(const CellRef cell,
                                               const DimRef cloneDim) const {
  if (isEphemeral(cell)) {
    const auto it = ephemeralSlots_.find(cell);
    if (it != ephemeralSlots_.end()) {
      return it->second.dimension;
    }
    return noCell;
  }
  return manifold_.cloneMaster(cell, cloneDim);
}

bool UnifiedTransclusionEngine::isProtected(const CellRef cell) const {
  if (cell == noCell || isEphemeral(cell)) {
    return true;
  }
  if (cell == manifold_.home() || cell == manifold_.dimsDimension()) {
    return true;
  }
  for (const auto dim : manifold_.dimensions()) {
    if (cell == dim) {
      return true;
    }
  }
  const auto dimsDim = manifold_.dimsDimension();
  if (dimsDim != noCell) {
    if (manifold_.linked(cell, dimsDim, true) != noCell ||
        manifold_.linked(cell, dimsDim, false) != noCell) {
      return true;
    }
  }
  return false;
}

bool UnifiedTransclusionEngine::validate2RankManifold(
    std::string *const errorOut) const {
  // The fold maintains this, so a failure here means the manifold has drifted
  // from what its operations say rather than that a caller linked carelessly --
  // which is why it is worth keeping after the hand-written link code went.
  // Manifold::verifyAgainstFullRebuild() is the other half of the same check.
  for (const auto &slot : manifold_.cells()) {
    for (const auto &link : manifold_.dimensionsOf(slot.birthOp)) {
      if (zigzag::noCell != link.pos &&
          manifold_.linked(link.pos, link.dim, true) != slot.birthOp) {
        if (nullptr != errorOut) {
          *errorOut = std::format(
              "cell {} posward on dimension {} names {}, whose negward is {}",
              slot.birthOp, link.dim, link.pos,
              manifold_.linked(link.pos, link.dim, true));
        }
        return false;
      }
      if (zigzag::noCell != link.neg &&
          manifold_.linked(link.neg, link.dim, false) != slot.birthOp) {
        if (nullptr != errorOut) {
          *errorOut = std::format(
              "cell {} negward on dimension {} names {}, whose posward is {}",
              slot.birthOp, link.dim, link.neg,
              manifold_.linked(link.neg, link.dim, false));
        }
        return false;
      }
    }
  }
  return true;
}

std::string
UnifiedTransclusionEngine::resolveCellText(const CellRef cell) const {
  if (isEphemeral(cell)) {
    const auto it = ephemeralSlots_.find(cell);
    if (it != ephemeralSlots_.end()) {
      return resolveCellText(it->second.dimension);
    }
    return "";
  }
  if (const auto *const cold = coldOf(cell); nullptr != cold) {
    if (cold->resolutionStatus == xanadu::ResolutionStatus::WithheldRedacted) {
      return "[Redacted - Withheld]";
    }
    if (cold->resolutionStatus ==
        xanadu::ResolutionStatus::TranscopyrightLocked) {
      if (cold->transcopyrightInfo) {
        return "[🔒 " +
               std::to_string(cold->transcopyrightInfo->priceAtomicUnits) +
               " " + cold->transcopyrightInfo->currencySymbol + "]";
      }
      return "[🔒 Locked]";
    }
  }
  return manifold_.textOf(cell, store_);
}

std::string_view UnifiedTransclusionEngine::resolveLocalCellView(
    const CellRef cell) const noexcept {
  const auto *const slot = manifold_.slot(cell);
  if (nullptr == slot || slot->span.empty() || !slot->span.isLocal()) {
    return {};
  }
  return store_.primedia().readView(slot->span);
}

std::size_t UnifiedTransclusionEngine::ShapingKeyHash::operator()(
    const ShapingKey &k) const noexcept {
  // The text dominates; the rest are folded in so that the same words shaped
  // at a different width, or with different decorations, land elsewhere.
  std::size_t h  = std::hash<std::string>{}(k.text);
  const auto mix = [&h](const std::size_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
  };
  mix(std::hash<const void *>{}(k.font));
  mix(std::hash<float>{}(k.maxWidthPx));
  mix(std::hash<float>{}(k.maxHeightPx));
  mix(static_cast<std::size_t>(k.singleParagraph) |
      (static_cast<std::size_t>(k.ellipsize) << 1U));
  for (const auto &range : k.decoratedRanges) {
    mix(static_cast<std::size_t>(range.start));
    mix(static_cast<std::size_t>(range.end));
    mix(static_cast<std::size_t>(range.decorations));
  }
  for (const auto &box : k.boxes) {
    mix(static_cast<std::size_t>(box.anchor));
    mix(std::hash<float>{}(box.widthPx));
    mix(std::hash<float>{}(box.heightPx));
    mix(static_cast<std::size_t>(box.placement));
  }
  for (const auto &range : k.blockStyles) {
    mix(static_cast<std::size_t>(range.start));
    mix(static_cast<std::size_t>(range.end));
    mix(static_cast<std::size_t>(range.align));
  }
  mix(static_cast<std::size_t>(k.page.mode));
  mix(std::hash<float>{}(k.page.widthPx));
  mix(std::hash<float>{}(k.page.heightPx));
  return h;
}

const PageShaping &UnifiedTransclusionEngine::shapedPage(
    const std::string_view text, const gleditor::text::FontFacePtr &font,
    const gleditor::text::LayoutOptions &opts) {
  ShapingKey key{.text            = std::string(text),
                 .font            = font.get(),
                 .maxWidthPx      = opts.maxWidthPx,
                 .maxHeightPx     = opts.maxHeightPx,
                 .singleParagraph = opts.singleParagraph,
                 .ellipsize       = opts.ellipsize,
                 .decoratedRanges = opts.decoratedRanges,
                 .boxes           = opts.boxes,
                 .blockStyles     = opts.blockStyles,
                 .page            = opts.page};

  shapingTick_++;
  if (const auto found = shapingCache_.find(key);
      found != shapingCache_.end()) {
    found->second.lastUsedTick = shapingTick_;
    shapingHits_++;
    return found->second.shaping;
  }
  shapingMisses_++;

  // Evict before inserting, so the cache never exceeds capacity even briefly.
  // Oldest-used first: a staging pass sweeps a neighbourhood, so the entry
  // asked for least recently is the one the camera has moved away from.
  if (shapingCache_.size() >= kShapingCacheCapacity) {
    auto oldest = shapingCache_.begin();
    for (auto it = shapingCache_.begin(); it != shapingCache_.end(); ++it) {
      if (it->second.lastUsedTick < oldest->second.lastUsedTick) {
        oldest = it;
      }
    }
    shapingCache_.erase(oldest);
    shapingEvictions_++;
  }

  auto shaping = gleditor::text::TextLayout::layoutPage(text, font, opts);
  const auto [it, inserted] = shapingCache_.emplace(
      std::move(key), ShapingEntry{.shaping      = std::move(shaping),
                                   .lastUsedTick = shapingTick_});
  return it->second.shaping;
}

UnifiedTransclusionEngine::RenderInstanceBatch
UnifiedTransclusionEngine::stageVisibleCells(
    const RenderSliceRequest &req, const gleditor::text::FontFacePtr &font,
    gleditor::GlyphCache &glyphCache) {
  RenderInstanceBatch batch;
  if (0 == manifold_.cellCount() || !font) {
    return batch;
  }

  // The axes are named in the request and are cells here, so each is resolved
  // once per pass rather than per hop: a dimension is found by walking the
  // d.dims rank, which is cheap but not free.
  const auto axisX = manifold_.dimensionNamed(req.axisX, store_);
  const auto axisY = manifold_.dimensionNamed(req.axisY, store_);
  const auto axisZ = manifold_.dimensionNamed(req.axisZ, store_);

  const CellRef startId = manifold_.contains(req.focusCellId)
                              ? req.focusCellId
                              : manifold_.cells().front().birthOp;

  // Breadth-first collection along requested spatial dimensions
  std::set<CellID> visited;
  std::queue<std::pair<CellID, int>> queue;
  queue.push({startId, 0});
  visited.insert(startId);

  const int maxRadius = std::max({req.radiusX, req.radiusY, req.radiusZ, 1});

  while (!queue.empty()) {
    const auto [currId, dist] = queue.front();
    queue.pop();

    if (dist >= maxRadius) {
      continue;
    }

    const auto *cell = findCell(currId);
    if (!cell) {
      continue;
    }

    const auto checkNeighbor = [&](const CellID neighbor) {
      if (neighbor != 0 && !visited.contains(neighbor) &&
          findCell(static_cast<CellRef>(neighbor))) {
        visited.insert(neighbor);
        queue.push({neighbor, dist + 1});
      }
    };

    for (const auto axis : {axisX, axisY, axisZ}) {
      if (zigzag::noCell == axis) {
        continue;
      }
      checkNeighbor(linked(currId, axis, false));
      checkNeighbor(linked(currId, axis, true));
    }
  }

  // Layout and stage glyph quads for all visited cells
  for (const CellID cid : visited) {
    const std::string text = resolveCellText(cid);
    if (text.empty()) {
      continue;
    }

    const gleditor::text::LayoutOptions opts{.maxWidthPx      = 380.0F,
                                             .maxHeightPx     = 240.0F,
                                             .singleParagraph = false,
                                             .ellipsize       = true,
                                             .decoratedRanges = {}};

    // Shaping the same unchanged text again every frame is what a staging
    // pass used to spend nearly all of its time on. The glyph cache lookups
    // below still run each time, because their atlas coordinates move when
    // the atlas grows and a cached copy of them would go stale.
    const auto &shaping = shapedPage(text, font, opts);

    // Why a cell is not showing its content is a render-side fact and lives in
    // the cold table, not in the slot: a withheld span and a paid-for one are
    // the same address until the reader's keys say otherwise.
    std::uint32_t paperCol = Doc::VBORow::color(25);
    if (const auto *const cold = coldOf(cid); nullptr != cold) {
      if (cold->resolutionStatus ==
          xanadu::ResolutionStatus::WithheldRedacted) {
        paperCol = Doc::VBORow::color3(17, 24, 39);
      } else if (cold->resolutionStatus ==
                 xanadu::ResolutionStatus::TranscopyrightLocked) {
        paperCol = Doc::VBORow::color3(245, 158, 11);
      }
    }

    for (const auto &glyph : shaping.glyphs) {
      const auto sizes = glyphCache.put(glyph.chr, font);
      Doc::VBORow row{};
      row.pos = {glyph.clusterLeft, glyph.clusterTop};
      row.foreground =
          Doc::VBORow::ink(Doc::VBORow::color(230), Doc::VBORow::onText, false);
      row.atlas = Doc::VBORow::atlasAt(
          static_cast<unsigned int>(sizes.texCoords.topLeft.x),
          static_cast<unsigned int>(sizes.texCoords.topLeft.y));
      const auto w =
          static_cast<unsigned int>(std::to_underlying(sizes.dims.width));
      const auto h =
          static_cast<unsigned int>(std::to_underlying(sizes.dims.height));
      row.quad =
          Doc::VBORow::box(static_cast<unsigned char>(sizes.layer), w, h, 0);
      row.paper = Doc::VBORow::paperAt(paperCol, 0);
      batch.rows.push_back(row);
    }
  }

  batch.instanceCount = batch.rows.size();
  return batch;
}

std::size_t UnifiedTransclusionEngine::stageIntoStreamBuffer(
    const RenderSliceRequest &req, const gleditor::text::FontFacePtr &font,
    gleditor::GlyphCache &glyphCache, render::IStreamBuffer &streamBuffer) {
  const auto batch = stageVisibleCells(req, font, glyphCache);
  if (batch.rows.empty()) {
    return 0;
  }

  const std::size_t bytes = batch.rows.size() * sizeof(Doc::VBORow);
  auto chunk              = streamBuffer.allocate(bytes, 64);
  if (!chunk.ptr) {
    return 0;
  }

  std::memcpy(chunk.ptr, batch.rows.data(), bytes);
  streamBuffer.flushAndUnmap(chunk.offset, bytes);
  return chunk.offset;
}

UnifiedTransclusionEngine::ShapingCacheStats
UnifiedTransclusionEngine::shapingCacheStats() const noexcept {
  return ShapingCacheStats{.entries   = shapingCache_.size(),
                           .hits      = shapingHits_,
                           .misses    = shapingMisses_,
                           .evictions = shapingEvictions_};
}

void UnifiedTransclusionEngine::clearShapingCache() noexcept {
  shapingCache_.clear();
}

} // namespace zigzag
