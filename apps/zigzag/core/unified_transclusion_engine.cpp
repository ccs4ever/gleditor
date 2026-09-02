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

UnifiedTransclusionEngine::UnifiedTransclusionEngine(xudu::Store &store)
    : store_(store) {}

void UnifiedTransclusionEngine::syncIncremental() {
  const auto &ops     = store_.segmentedOps();
  const auto totalOps = ops.size();

  for (std::uint32_t idx = lastSyncedOpIndex_ + 1; idx <= totalOps; ++idx) {
    const auto *node = ops.get(idx);
    if (!node) {
      continue;
    }
    buildCellFromOp(idx, *node);
  }
  lastSyncedOpIndex_ = static_cast<std::uint32_t>(totalOps);
}

void UnifiedTransclusionEngine::buildCellFromOp(
    const std::uint32_t opIndex, const xudu::CompactOpNode &node) {
  if (opIndexToCell_.contains(opIndex)) {
    return;
  }

  const CellID id = nextCellId_++;
  CompactZZCell cell;
  cell.id           = id;
  cell.spoolOpIndex = opIndex;
  cell.span         = node.span();
  cell.type         = "op";

  // 1. Link d.ops_time (Sequential spool order)
  if (opIndex > 1 && opIndexToCell_.contains(opIndex - 1)) {
    const CellID prevId = opIndexToCell_[opIndex - 1];
    cell.setLinks(DimOrdinal::OpsTime, {.pos = 0, .neg = prevId});
    if (cells_.contains(prevId)) {
      auto lp = cells_[prevId].linksOn(DimOrdinal::OpsTime);
      lp.pos  = id;
      cells_[prevId].setLinks(DimOrdinal::OpsTime, lp);
    }
  }

  // 2. Link d.ops_dag (Ancestral DAG)
  if (node.parentIndex != 0 && opIndexToCell_.contains(node.parentIndex)) {
    const CellID parentCellId = opIndexToCell_[node.parentIndex];
    cell.setLinks(DimOrdinal::OpsDag, {.pos = 0, .neg = parentCellId});
    if (cells_.contains(parentCellId)) {
      auto parentLp = cells_[parentCellId].linksOn(DimOrdinal::OpsDag);
      if (parentLp.pos == 0) {
        parentLp.pos = id;
        cells_[parentCellId].setLinks(DimOrdinal::OpsDag, parentLp);
      }
    }
  }

  // 3. Link d.transclude (Primedia Span Address Equivalence)
  if (!cell.span.empty()) {
    if (spanToMasterCell_.contains(cell.span)) {
      const CellID masterId = spanToMasterCell_[cell.span];
      cell.setLinks(DimOrdinal::Transclude, {.pos = 0, .neg = masterId});
      if (cells_.contains(masterId)) {
        auto masterLp = cells_[masterId].linksOn(DimOrdinal::Transclude);
        masterLp.pos  = id;
        cells_[masterId].setLinks(DimOrdinal::Transclude, masterLp);
      }
    } else {
      spanToMasterCell_[cell.span] = id;
    }
  }

  opIndexToCell_[opIndex] = id;
  cells_[id]              = std::move(cell);
}

void UnifiedTransclusionEngine::linkCells(const CellID a, const CellID b,
                                          const DimOrdinal dim) {
  auto *ca = findCell(a);
  auto *cb = findCell(b);
  if (!ca || !cb) {
    return;
  }

  auto lpa = ca->linksOn(dim);
  lpa.pos  = b;
  ca->setLinks(dim, lpa);

  auto lpb = cb->linksOn(dim);
  lpb.neg  = a;
  cb->setLinks(dim, lpb);
}

void UnifiedTransclusionEngine::linkCells(const CellID a, const CellID b,
                                          const DimID &dim) {
  if (const auto ord = dimOrdinalFromString(dim); ord.has_value()) {
    linkCells(a, b, *ord);
    return;
  }

  auto *ca = findCell(a);
  auto *cb = findCell(b);
  if (!ca || !cb) {
    return;
  }

  auto lpa = ca->linksOn(dim);
  lpa.pos  = b;
  ca->setLinks(dim, lpa);

  auto lpb = cb->linksOn(dim);
  lpb.neg  = a;
  cb->setLinks(dim, lpb);
}

void UnifiedTransclusionEngine::unlinkPositive(const CellID a,
                                               const DimOrdinal dim) {
  auto *ca = findCell(a);
  if (!ca) {
    return;
  }

  const auto b = ca->linksOn(dim).pos;
  auto lpa     = ca->linksOn(dim);
  lpa.pos      = 0;
  ca->setLinks(dim, lpa);

  if (b != 0) {
    if (auto *cb = findCell(b); cb != nullptr) {
      auto lpb = cb->linksOn(dim);
      if (lpb.neg == a) {
        lpb.neg = 0;
        cb->setLinks(dim, lpb);
      }
    }
  }
}

CellID UnifiedTransclusionEngine::addCell(CompactZZCell cell) {
  if (cell.id == 0) {
    cell.id = nextCellId_++;
  } else if (cell.id >= nextCellId_) {
    nextCellId_ = cell.id + 1;
  }

  const CellID id = cell.id;
  if (cell.spoolOpIndex != 0) {
    opIndexToCell_[cell.spoolOpIndex] = id;
  }
  if (!cell.span.empty() && !spanToMasterCell_.contains(cell.span)) {
    spanToMasterCell_[cell.span] = id;
  }

  cells_[id] = std::move(cell);
  return id;
}

const CompactZZCell *
UnifiedTransclusionEngine::findCell(const CellID id) const noexcept {
  const auto it = cells_.find(id);
  return (it != cells_.end()) ? &it->second : nullptr;
}

CompactZZCell *UnifiedTransclusionEngine::findCell(const CellID id) noexcept {
  const auto it = cells_.find(id);
  return (it != cells_.end()) ? &it->second : nullptr;
}

CellID UnifiedTransclusionEngine::cellForOp(
    const std::uint32_t opIndex) const noexcept {
  const auto it = opIndexToCell_.find(opIndex);
  return (it != opIndexToCell_.end()) ? it->second : 0;
}

bool UnifiedTransclusionEngine::validate2RankManifold(
    std::string *errorOut) const {
  for (const auto &[id, cell] : cells_) {
    // 1. Validate standard dimensions
    for (std::size_t i = 0; i < StandardDimensionCount; ++i) {
      const auto ord = static_cast<DimOrdinal>(i);
      const auto lp  = cell.linksOn(ord);

      if (lp.pos != 0) {
        const auto *target = findCell(lp.pos);
        if (!target) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} pos link on {} points to non-existent cell {}", id,
                dimOrdinalToString(ord), lp.pos);
          }
          return false;
        }
        if (target->linksOn(ord).neg != id) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} pos link on {} to {} is asymmetric (target neg is {})",
                id, dimOrdinalToString(ord), lp.pos, target->linksOn(ord).neg);
          }
          return false;
        }
      }

      if (lp.neg != 0) {
        const auto *target = findCell(lp.neg);
        if (!target) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} neg link on {} points to non-existent cell {}", id,
                dimOrdinalToString(ord), lp.neg);
          }
          return false;
        }
        if (target->linksOn(ord).pos != id) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} neg link on {} to {} is asymmetric (target pos is {})",
                id, dimOrdinalToString(ord), lp.neg, target->linksOn(ord).pos);
          }
          return false;
        }
      }
    }

    // 2. Validate dynamic dimensions
    for (const auto &dyn : cell.dynamicDimensions) {
      if (dyn.links.pos != 0) {
        const auto *target = findCell(dyn.links.pos);
        if (!target) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} pos link on {} points to non-existent cell {}", id,
                dyn.name, dyn.links.pos);
          }
          return false;
        }
        if (target->linksOn(dyn.name).neg != id) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} pos link on {} to {} is asymmetric (target neg is {})",
                id, dyn.name, dyn.links.pos, target->linksOn(dyn.name).neg);
          }
          return false;
        }
      }
      if (dyn.links.neg != 0) {
        const auto *target = findCell(dyn.links.neg);
        if (!target) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} neg link on {} points to non-existent cell {}", id,
                dyn.name, dyn.links.neg);
          }
          return false;
        }
        if (target->linksOn(dyn.name).pos != id) {
          if (errorOut) {
            *errorOut = std::format(
                "Cell {} neg link on {} to {} is asymmetric (target pos is {})",
                id, dyn.name, dyn.links.neg, target->linksOn(dyn.name).pos);
          }
          return false;
        }
      }
    }
  }
  return true;
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
                 .decoratedRanges = opts.decoratedRanges};

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

std::string UnifiedTransclusionEngine::resolveCellText(const CellID id) const {
  const auto *cell = findCell(id);
  if (!cell) {
    return {};
  }
  return cell->readText(store_.primedia(), store_.contentResolver(),
                        store_.scrolls());
}

std::string_view UnifiedTransclusionEngine::resolveLocalCellView(
    const CellID id) const noexcept {
  const auto *cell = findCell(id);
  if (!cell) {
    return {};
  }
  return cell->resolveLocalView(store_.primedia());
}

ZzStructureDocument
UnifiedTransclusionEngine::toZzStructureDocument(const CellID focus) const {
  ZzStructureDocument doc;
  if (focus != 0 && cells_.contains(focus)) {
    doc.focus = focus;
  } else if (!cells_.empty()) {
    doc.focus = cells_.begin()->first;
  }

  for (const auto &[id, cell] : cells_) {
    zzCell zc;
    zc.id        = id;
    zc.type      = cell.type;
    zc.preflet   = cell.preflet;
    zc.text_data = resolveCellText(id);

    for (std::size_t i = 0; i < StandardDimensionCount; ++i) {
      const auto ord = static_cast<DimOrdinal>(i);
      const auto lp  = cell.linksOn(ord);
      if (lp.pos != 0 || lp.neg != 0) {
        zc.dimensions[std::string(dimOrdinalToString(ord))] = lp;
      }
    }

    for (const auto &dyn : cell.dynamicDimensions) {
      if (dyn.links.pos != 0 || dyn.links.neg != 0) {
        zc.dimensions[dyn.name] = dyn.links;
      }
    }

    doc.cells[id] = std::move(zc);
  }
  return doc;
}

void UnifiedTransclusionEngine::loadFromZzStructureDocument(
    const ZzStructureDocument &doc) {
  for (const auto &[id, zc] : doc.cells) {
    CompactZZCell cell;
    cell.id            = id;
    cell.type          = zc.type;
    cell.preflet       = zc.preflet;
    cell.ephemeralText = zc.text_data;

    for (const auto &[dimName, lp] : zc.dimensions) {
      cell.setLinks(dimName, lp);
    }

    cells_[id] = std::move(cell);
    if (id >= nextCellId_) {
      nextCellId_ = id + 1;
    }
  }
}

UnifiedTransclusionEngine::RenderInstanceBatch
UnifiedTransclusionEngine::stageVisibleCells(
    const RenderSliceRequest &req, const gleditor::text::FontFacePtr &font,
    gleditor::GlyphCache &glyphCache) {
  RenderInstanceBatch batch;
  if (cells_.empty() || !font) {
    return batch;
  }

  const CellID startId = cells_.contains(req.focusCellId)
                             ? req.focusCellId
                             : cells_.begin()->first;

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
          cells_.contains(neighbor)) {
        visited.insert(neighbor);
        queue.push({neighbor, dist + 1});
      }
    };

    const auto lpX = cell->linksOn(req.axisX);
    checkNeighbor(lpX.pos);
    checkNeighbor(lpX.neg);

    const auto lpY = cell->linksOn(req.axisY);
    checkNeighbor(lpY.pos);
    checkNeighbor(lpY.neg);

    const auto lpZ = cell->linksOn(req.axisZ);
    checkNeighbor(lpZ.pos);
    checkNeighbor(lpZ.neg);
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

    const auto *cell       = findCell(cid);
    std::uint32_t paperCol = Doc::VBORow::color(25);
    if (cell != nullptr) {
      if (cell->isWithheld()) {
        paperCol = Doc::VBORow::color3(17, 24, 39);
      } else if (cell->isTranscopyrightLocked()) {
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
    gleditor::GlyphCache &glyphCache,
    render::gl::StreamBufferGL &streamBuffer) {
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

} // namespace zigzag
