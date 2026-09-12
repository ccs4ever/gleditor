/**
 * @file layoutfilade.cpp
 * @brief Implementation of the True Layoutfilade 2D B-enfilade.
 */
#include "common/xanadu/enfilade/layoutfilade.hpp"

#include <cmath>
#include <iostream>

namespace xanadu::enfilade {

namespace {

constexpr std::string_view kMediaAnchorString = "\xEF\xBF\xBC";

[[nodiscard]] LayoutWid computeWidForEntries(const LayoutEntry *entries,
                                             const std::size_t count) noexcept {
  LayoutWid w{};
  for (std::size_t i = 0; i < count; ++i) {
    const auto &e = entries[i];
    w.totalBytes += e.byteLength;
    w.totalHeightPx += (e.heightPx + (2.0F * e.marginPx));
    w.lineCount += 1;
    w.maxLineWidthPx =
        std::max(w.maxLineWidthPx, e.widthPx + (2.0F * e.marginPx));
    if (e.isMediaBox()) {
      w.mediaBoxCount += 1;
    }
  }
  return w;
}

} // namespace

Layoutfilade
Layoutfilade::buildFromEntries(const std::vector<LayoutEntry> &entries) {
  Layoutfilade filade;
  filade.entries_ = entries;
  filade.buildTree();
  return filade;
}

Layoutfilade
Layoutfilade::fromTextAndBoxes(const std::string_view text,
                               const float lineHeightPx,
                               const float defaultCharWidthPx,
                               const std::vector<gleditor::LayoutBox> &boxes) {
  std::vector<LayoutEntry> entries;

  // Index boxes by anchor
  std::size_t boxIdx = 0;

  std::size_t lineStart = 0;
  while (lineStart < text.size()) {
    // Check if there is an anchored box at current offset
    while (boxIdx < boxes.size() && boxes[boxIdx].anchor < lineStart) {
      boxIdx++;
    }

    if (boxIdx < boxes.size() && boxes[boxIdx].anchor == lineStart) {
      const auto &box   = boxes[boxIdx++];
      LayoutEntryKind k = LayoutEntryKind::BlockBox;
      if (gleditor::BoxPlacement::Inline == box.placement) {
        k = LayoutEntryKind::InlineBox;
      } else if (gleditor::BoxPlacement::FloatLeft == box.placement ||
                 gleditor::BoxPlacement::FloatRight == box.placement) {
        k = LayoutEntryKind::FloatBox;
      }

      // Check if text at lineStart is the U+FFFC sequence
      std::uint32_t byteLen = 1;
      if (lineStart + 3 <= text.size() &&
          text.substr(lineStart, 3) == kMediaAnchorString) {
        byteLen = 3;
      }

      entries.push_back(LayoutEntry{
          .byteLength       = byteLen,
          .heightPx         = box.heightPx,
          .widthPx          = box.widthPx,
          .marginPx         = box.marginPx,
          .baselineOffsetPx = box.baselineOffsetPx,
          .boxId            = box.id,
          .kind             = k,
          .placement        = static_cast<std::uint8_t>(box.placement),
          .flags            = 0,
      });
      lineStart += byteLen;
      continue;
    }

    // Standard line scan to next newline
    auto newlinePos = text.find('\n', lineStart);
    std::size_t lineEnd =
        (std::string_view::npos == newlinePos) ? text.size() : (newlinePos + 1);

    // If an anchored box sits inside this line before lineEnd, cut before box
    if (boxIdx < boxes.size() && boxes[boxIdx].anchor > lineStart &&
        boxes[boxIdx].anchor < lineEnd) {
      lineEnd = boxes[boxIdx].anchor;
    }

    const auto lineSlice = text.substr(lineStart, lineEnd - lineStart);
    const auto len       = static_cast<std::uint32_t>(lineSlice.size());
    const float visualLen =
        static_cast<float>(len > 0 && lineSlice.back() == '\n' ? len - 1 : len);

    entries.push_back(LayoutEntry{
        .byteLength       = len,
        .heightPx         = lineHeightPx,
        .widthPx          = visualLen * defaultCharWidthPx,
        .marginPx         = 0.0F,
        .baselineOffsetPx = 0.0F,
        .boxId            = 0,
        .kind             = LayoutEntryKind::TextLine,
        .placement        = 0,
        .flags            = static_cast<std::uint16_t>(
            len > 0 && lineSlice.back() == '\n' ? 1U : 0U),
    });

    lineStart = lineEnd;
  }

  return buildFromEntries(entries);
}

void Layoutfilade::recomputeWid(const std::size_t crumIdx) {
  if (crumIdx >= crums_.size()) {
    return;
  }
  auto &crum = crums_[crumIdx];
  if (crum.isLeaf) {
    crum.wid =
        computeWidForEntries(&entries_[crum.firstEntry], crum.entryCount);
  } else {
    LayoutWid acc{};
    for (std::size_t c = 0; c < crum.childCount; ++c) {
      acc = acc.combine(crums_[crum.firstChild + c].wid);
    }
    crum.wid = acc;
  }
}

void Layoutfilade::buildTree() {
  crums_.clear();
  rootIndex_ = 0;
  if (entries_.empty()) {
    return;
  }

  // 1. Build leaf level
  constexpr std::size_t B = LayoutCrum::BranchingFactor;
  std::vector<std::size_t> currentLevel;

  for (std::size_t i = 0; i < entries_.size(); i += B) {
    const auto count =
        static_cast<std::uint16_t>(std::min(B, entries_.size() - i));
    const auto idx = crums_.size();

    LayoutCrum crum;
    crum.firstEntry  = static_cast<std::uint32_t>(i);
    crum.entryCount  = count;
    crum.firstChild  = 0;
    crum.childCount  = 0;
    crum.parentIndex = 0;
    crum.isLeaf      = true;
    crum.wid         = computeWidForEntries(&entries_[i], count);

    crums_.push_back(crum);
    currentLevel.push_back(idx);
  }

  // 2. Build interior levels bottom-up
  while (currentLevel.size() > 1) {
    std::vector<std::size_t> nextLevel;
    for (std::size_t i = 0; i < currentLevel.size(); i += B) {
      const auto childCount =
          static_cast<std::uint16_t>(std::min(B, currentLevel.size() - i));
      const auto parentIdx  = crums_.size();
      const auto firstChild = static_cast<std::uint32_t>(currentLevel[i]);

      LayoutCrum parentCrum;
      parentCrum.firstEntry  = 0;
      parentCrum.entryCount  = 0;
      parentCrum.firstChild  = firstChild;
      parentCrum.childCount  = childCount;
      parentCrum.parentIndex = 0;
      parentCrum.isLeaf      = false;

      LayoutWid parentWid{};
      for (std::size_t c = 0; c < childCount; ++c) {
        const auto childIdx          = currentLevel[i + c];
        crums_[childIdx].parentIndex = static_cast<std::uint32_t>(parentIdx);
        parentWid                    = parentWid.combine(crums_[childIdx].wid);
      }
      parentCrum.wid = parentWid;

      crums_.push_back(parentCrum);
      nextLevel.push_back(parentIdx);
    }
    currentLevel = std::move(nextLevel);
  }

  rootIndex_ = currentLevel.empty() ? 0 : currentLevel.front();
}

std::optional<LayoutHit>
Layoutfilade::findEntryAtY(const float targetYPx) const {
  if (entries_.empty() || crums_.empty()) {
    return std::nullopt;
  }

  std::size_t currCrumIdx = rootIndex_;
  float currY             = 0.0F;
  std::uint32_t currByte  = 0;

  while (!crums_[currCrumIdx].isLeaf) {
    const auto &crum = crums_[currCrumIdx];
    bool stepped     = false;

    for (std::size_t c = 0; c < crum.childCount; ++c) {
      const auto childIdx  = crum.firstChild + c;
      const float chHeight = crums_[childIdx].wid.totalHeightPx;

      if (c == crum.childCount - 1 || targetYPx < (currY + chHeight)) {
        currCrumIdx = childIdx;
        stepped     = true;
        break;
      }
      currY += chHeight;
      currByte += crums_[childIdx].wid.totalBytes;
    }

    if (!stepped) {
      currCrumIdx = crum.firstChild + crum.childCount - 1;
    }
  }

  // At leaf crum
  const auto &leaf = crums_[currCrumIdx];
  for (std::size_t i = 0; i < leaf.entryCount; ++i) {
    const auto entryIdx = leaf.firstEntry + i;
    const auto &entry   = entries_[entryIdx];
    const float eHeight = entry.heightPx + (2.0F * entry.marginPx);

    if (i == leaf.entryCount - 1 || targetYPx < (currY + eHeight)) {
      return LayoutHit{
          .entryIndex      = entryIdx,
          .startByte       = currByte,
          .startYPx        = currY,
          .intraByteOffset = 0,
          .entry           = entry,
      };
    }
    currY += eHeight;
    currByte += entry.byteLength;
  }

  return std::nullopt;
}

std::optional<LayoutHit>
Layoutfilade::findEntryAtByte(const std::uint32_t targetByteOffset) const {
  if (entries_.empty() || crums_.empty()) {
    return std::nullopt;
  }

  std::size_t currCrumIdx = rootIndex_;
  float currY             = 0.0F;
  std::uint32_t currByte  = 0;

  while (!crums_[currCrumIdx].isLeaf) {
    const auto &crum = crums_[currCrumIdx];
    bool stepped     = false;

    for (std::size_t c = 0; c < crum.childCount; ++c) {
      const auto childIdx = crum.firstChild + c;
      const auto chBytes  = crums_[childIdx].wid.totalBytes;

      if (c == crum.childCount - 1 || targetByteOffset < (currByte + chBytes)) {
        currCrumIdx = childIdx;
        stepped     = true;
        break;
      }
      currByte += chBytes;
      currY += crums_[childIdx].wid.totalHeightPx;
    }

    if (!stepped) {
      currCrumIdx = crum.firstChild + crum.childCount - 1;
    }
  }

  // At leaf crum
  const auto &leaf = crums_[currCrumIdx];
  for (std::size_t i = 0; i < leaf.entryCount; ++i) {
    const auto entryIdx = leaf.firstEntry + i;
    const auto &entry   = entries_[entryIdx];
    const auto eBytes   = entry.byteLength;

    if (i == leaf.entryCount - 1 || targetByteOffset < (currByte + eBytes)) {
      const auto intra =
          (targetByteOffset >= currByte) ? (targetByteOffset - currByte) : 0U;
      return LayoutHit{
          .entryIndex      = entryIdx,
          .startByte       = currByte,
          .startYPx        = currY,
          .intraByteOffset = intra,
          .entry           = entry,
      };
    }
    currByte += eBytes;
    currY += entry.heightPx + (2.0F * entry.marginPx);
  }

  return std::nullopt;
}

std::optional<LayoutHit>
Layoutfilade::findEntryByIndex(const std::size_t entryIndex) const {
  if (entryIndex >= entries_.size() || crums_.empty()) {
    return std::nullopt;
  }

  std::size_t currCrumIdx = rootIndex_;
  float currY             = 0.0F;
  std::uint32_t currByte  = 0;
  std::size_t currLines   = 0;

  while (!crums_[currCrumIdx].isLeaf) {
    const auto &crum = crums_[currCrumIdx];
    bool stepped     = false;

    for (std::size_t c = 0; c < crum.childCount; ++c) {
      const auto childIdx = crum.firstChild + c;
      const auto chLines  = crums_[childIdx].wid.lineCount;

      if (c == crum.childCount - 1 || entryIndex < (currLines + chLines)) {
        currCrumIdx = childIdx;
        stepped     = true;
        break;
      }
      currLines += chLines;
      currByte += crums_[childIdx].wid.totalBytes;
      currY += crums_[childIdx].wid.totalHeightPx;
    }

    if (!stepped) {
      currCrumIdx = crum.firstChild + crum.childCount - 1;
    }
  }

  // At leaf crum
  const auto &leaf = crums_[currCrumIdx];
  for (std::size_t i = 0; i < leaf.entryCount; ++i) {
    const auto entryIdx = leaf.firstEntry + i;
    const auto &entry   = entries_[entryIdx];

    if (entryIdx == entryIndex) {
      return LayoutHit{
          .entryIndex      = entryIdx,
          .startByte       = currByte,
          .startYPx        = currY,
          .intraByteOffset = 0,
          .entry           = entry,
      };
    }
    currByte += entry.byteLength;
    currY += entry.heightPx + (2.0F * entry.marginPx);
  }

  return std::nullopt;
}

VisibleRange Layoutfilade::visibleRange(const float viewportTopY,
                                        const float viewportBottomY) const {
  if (entries_.empty() || crums_.empty()) {
    return VisibleRange{};
  }

  const float clampedTop    = std::max(0.0F, viewportTopY);
  const float clampedBottom = std::max(clampedTop, viewportBottomY);

  const auto hitTop    = findEntryAtY(clampedTop);
  const auto hitBottom = findEntryAtY(clampedBottom);

  if (!hitTop || !hitBottom) {
    return VisibleRange{};
  }

  const auto firstIdx = hitTop->entryIndex;
  const auto lastIdx  = std::max(firstIdx, hitBottom->entryIndex);

  VisibleRange range;
  range.firstEntryIndex = firstIdx;
  range.lastEntryIndex  = lastIdx;
  range.startByteOffset = hitTop->startByte;
  range.startYPx        = hitTop->startYPx;

  // Ending offset / coordinates
  range.endByteOffset = hitBottom->startByte + hitBottom->entry.byteLength;
  range.endYPx        = hitBottom->startYPx + hitBottom->entry.heightPx +
                        (2.0F * hitBottom->entry.marginPx);
  range.entryCount    = (lastIdx - firstIdx) + 1;

  for (std::size_t idx = firstIdx; idx <= lastIdx; ++idx) {
    if (entries_[idx].isMediaBox()) {
      range.mediaBoxCount++;
      range.visibleMediaBoxIndices.push_back(idx);
    }
  }

  return range;
}

bool Layoutfilade::updateEntry(const std::size_t entryIndex,
                               const LayoutEntry &newEntry) {
  if (entryIndex >= entries_.size() || crums_.empty()) {
    return false;
  }

  entries_[entryIndex] = newEntry;

  // Locate leaf crum: because leaves are built bottom-up sequentially in blocks
  // of B, the leaf index is entryIndex / B.
  constexpr std::size_t B   = LayoutCrum::BranchingFactor;
  const std::size_t leafIdx = entryIndex / B;

  if (leafIdx >= crums_.size() || !crums_[leafIdx].isLeaf) {
    // Fallback: rebuild whole tree if layout is fragmented
    buildTree();
    return true;
  }

  // Recompute leaf and bubble up parent chain
  recomputeWid(leafIdx);

  std::size_t curr = crums_[leafIdx].parentIndex;
  while (curr != rootIndex_ && curr < crums_.size()) {
    recomputeWid(curr);
    curr = crums_[curr].parentIndex;
  }
  recomputeWid(rootIndex_);

  return true;
}

bool Layoutfilade::verifyAgainstLinearScan(
    const std::vector<LayoutEntry> &groundTruth) const {
  if (groundTruth.size() != entries_.size()) {
    return false;
  }

  float linearY            = 0.0F;
  std::uint32_t linearByte = 0;

  for (std::size_t i = 0; i < groundTruth.size(); ++i) {
    const auto &e     = groundTruth[i];
    const auto hitIdx = findEntryByIndex(i);
    if (!hitIdx) {
      return false;
    }
    if (hitIdx->entryIndex != i || hitIdx->startByte != linearByte ||
        std::abs(hitIdx->startYPx - linearY) > 1e-3F) {
      return false;
    }

    // Verify Y stabbing
    const float midY = linearY + (0.5F * (e.heightPx + (2.0F * e.marginPx)));
    const auto hitY  = findEntryAtY(midY);
    if (!hitY || hitY->entryIndex != i) {
      return false;
    }

    // Verify byte stabbing
    if (e.byteLength > 0) {
      const auto hitByte = findEntryAtByte(linearByte);
      if (!hitByte || hitByte->entryIndex != i) {
        return false;
      }
    }

    linearY += (e.heightPx + (2.0F * e.marginPx));
    linearByte += e.byteLength;
  }

  // Verify total metrics
  const auto &m = metrics();
  if (m.totalBytes != linearByte ||
      std::abs(m.totalHeightPx - linearY) > 1e-3F ||
      m.lineCount != groundTruth.size()) {
    return false;
  }

  return true;
}

} // namespace xanadu::enfilade
