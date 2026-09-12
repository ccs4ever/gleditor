/**
 * @file spanfilade.cpp
 * @brief Implementation of the True Spanfilade (1D Interval B-Enfilade).
 */
#include "common/xanadu/enfilade/spanfilade.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::enfilade {

// ============================================================================
// ScrollSpanfilade Implementation
// ============================================================================

void ScrollSpanfilade::clear() {
  nodes_.clear();
  entries_.clear();
  rootIndex_ = 0;
}

void ScrollSpanfilade::insert(const SpanEntry &entry) {
  entries_.push_back(entry);
  bulkLoad(entries_);
}

void ScrollSpanfilade::bulkLoad(std::vector<SpanEntry> newEntries) {
  entries_ = std::move(newEntries);
  nodes_.clear();
  rootIndex_ = 0;

  if (entries_.empty()) {
    return;
  }

  // Sort entries by (start, end)
  std::ranges::sort(entries_, [](const SpanEntry &a, const SpanEntry &b) {
    if (a.start != b.start) {
      return a.start < b.start;
    }
    return a.end() < b.end();
  });

  // Pack leaf crums
  std::vector<uint32_t> currentLevel;
  currentLevel.reserve((entries_.size() + 7) / 8);

  for (std::size_t i = 0; i < entries_.size(); i += SpanCrum::BranchingFactor) {
    const auto chunkSize =
        std::min(SpanCrum::BranchingFactor, entries_.size() - i);
    SpanCrum leaf{};
    leaf.isLeaf     = 1;
    leaf.childCount = static_cast<uint8_t>(chunkSize);

    for (std::size_t j = 0; j < chunkSize; ++j) {
      const auto entryIdx = static_cast<uint32_t>(i + j);
      leaf.children[j]    = entryIdx;
      leaf.dsps[j]        = SpanDsp{0};
      leaf.wids[j] =
          SpanWid{entries_[entryIdx].start, entries_[entryIdx].end(), 1};
    }

    const auto nodeIdx = static_cast<uint32_t>(nodes_.size());
    nodes_.push_back(leaf);
    currentLevel.push_back(nodeIdx);
  }

  // Build interior levels bottom-up until 1 root remains
  while (currentLevel.size() > 1) {
    std::vector<uint32_t> nextLevel;
    nextLevel.reserve((currentLevel.size() + 7) / 8);

    for (std::size_t i = 0; i < currentLevel.size();
         i += SpanCrum::BranchingFactor) {
      const auto chunkSize =
          std::min(SpanCrum::BranchingFactor, currentLevel.size() - i);
      SpanCrum parent{};
      parent.isLeaf        = 0;
      parent.childCount    = static_cast<uint8_t>(chunkSize);
      const auto parentIdx = static_cast<uint32_t>(nodes_.size());

      for (std::size_t j = 0; j < chunkSize; ++j) {
        const auto childIdx          = currentLevel[i + j];
        parent.children[j]           = childIdx;
        parent.dsps[j]               = SpanDsp{0};
        parent.wids[j]               = nodes_[childIdx].totalWid();
        nodes_[childIdx].parentIndex = parentIdx;
      }

      nodes_.push_back(parent);
      nextLevel.push_back(parentIdx);
    }
    currentLevel = std::move(nextLevel);
  }

  rootIndex_ = currentLevel.empty() ? 0 : currentLevel[0];
}

void ScrollSpanfilade::stab(const uint64_t qStart, const uint64_t qEnd,
                            std::vector<SpanEntry> &results) const {
  if (nodes_.empty() || qStart >= qEnd) {
    return;
  }
  stabNode(rootIndex_, 0, qStart, qEnd, results);
}

void ScrollSpanfilade::stabNode(const uint32_t nodeIdx, const int64_t parentDsp,
                                const uint64_t qStart, const uint64_t qEnd,
                                std::vector<SpanEntry> &results) const {
  if (nodeIdx >= nodes_.size()) {
    return;
  }
  const auto &node = nodes_[nodeIdx];

  for (std::size_t i = 0; i < node.childCount; ++i) {
    const auto effectiveDsp = parentDsp + node.dsps[i].delta;
    const auto wid          = node.wids[i];
    if (wid.isEmpty()) {
      continue;
    }

    const auto effectiveWid = SpanDsp{effectiveDsp}.act(wid);
    if (!effectiveWid.overlaps(qStart, qEnd)) {
      continue; // Subtree pruned in O(1)!
    }

    if (node.isLeaf) {
      const auto entryIdx = node.children[i];
      if (entryIdx < entries_.size()) {
        const auto &entry = entries_[entryIdx];
        const auto eStart =
            (effectiveDsp >= 0)
                ? (entry.start + static_cast<uint64_t>(effectiveDsp))
                : (entry.start > static_cast<uint64_t>(-effectiveDsp)
                       ? entry.start - static_cast<uint64_t>(-effectiveDsp)
                       : static_cast<uint64_t>(0));
        const auto eEnd = eStart + entry.length;
        if (std::max(qStart, eStart) < std::min(qEnd, eEnd)) {
          results.push_back(entry);
        }
      }
    } else {
      stabNode(node.children[i], effectiveDsp, qStart, qEnd, results);
    }
  }
}

// ============================================================================
// Spanfilade Implementation
// ============================================================================

void Spanfilade::clear() { scrolls_.clear(); }

bool Spanfilade::empty() const noexcept {
  for (const auto &[scroll, enfilade] : scrolls_) {
    if (!enfilade.empty()) {
      return false;
    }
  }
  return true;
}

std::size_t Spanfilade::totalSpans() const noexcept {
  std::size_t total = 0;
  for (const auto &[scroll, enfilade] : scrolls_) {
    total += enfilade.size();
  }
  return total;
}

void Spanfilade::indexSpan(const ScrollId scroll, const SpanEntry &entry) {
  scrolls_[scroll].insert(entry);
}

void Spanfilade::indexVersion(const uint32_t docId, const Version &ver) {
  std::uint32_t seen = 0;
  for (const auto &run : ver.pieces()) {
    if (!run.empty() && !isReservedScroll(run.scroll)) {
      SpanEntry entry{
          .start     = run.start,
          .length    = run.length,
          .docId     = docId,
          .docOffset = seen,
          .cellDense = 0,
          .spanIndex = 0,
          .flags     = 0,
      };
      scrolls_[run.scroll].insert(entry);
    }
    seen += static_cast<std::uint32_t>(run.length);
  }
}

void Spanfilade::indexViews(const std::vector<const Version *> &views) {
  for (std::size_t i = 0; i < views.size(); ++i) {
    if (nullptr != views[i]) {
      indexVersion(static_cast<uint32_t>(i), *views[i]);
    }
  }
  build();
}

void Spanfilade::indexManifold(const zigzag::Manifold &manifold) {
  for (std::size_t dense = 0; dense < manifold.cells().size(); ++dense) {
    const auto &slot   = manifold.cells()[dense];
    const auto ref     = slot.birthOp;
    const auto content = manifold.contentOf(ref);
    for (std::size_t sIdx = 0; sIdx < content.size(); ++sIdx) {
      const auto &span = content[sIdx];
      if (!span.empty() && !isReservedScroll(span.scroll)) {
        SpanEntry entry{
            .start     = span.start,
            .length    = span.length,
            .docId     = 0,
            .docOffset = 0,
            .cellDense = static_cast<uint32_t>(dense),
            .spanIndex = static_cast<uint16_t>(sIdx),
            .flags     = 1U, // isCell
        };
        scrolls_[span.scroll].insert(entry);
      }
    }
  }
  build();
}

void Spanfilade::build() {
  for (auto &[scroll, enfilade] : scrolls_) {
    enfilade.bulkLoad(enfilade.entries());
  }
}

std::vector<SpanEntry>
Spanfilade::findIntersections(const PrimediaSpan &span) const {
  if (span.empty() || isReservedScroll(span.scroll)) {
    return {};
  }
  const auto it = scrolls_.find(span.scroll);
  if (it == scrolls_.end()) {
    return {};
  }
  return it->second.query(span.start, span.end());
}

std::vector<Extent> Spanfilade::occurrencesOf(const PrimediaSpan &span,
                                              const uint32_t docId) const {
  if (span.empty() || isReservedScroll(span.scroll)) {
    return {};
  }

  const auto it = scrolls_.find(span.scroll);
  if (it == scrolls_.end()) {
    return {};
  }

  std::vector<SpanEntry> results;
  it->second.stab(span.start, span.end(), results);

  std::vector<Extent> found;
  for (const auto &entry : results) {
    if (entry.docId != docId || entry.isCell()) {
      continue;
    }
    const auto sharedStart = std::max(entry.start, span.start);
    const auto sharedEnd   = std::min(entry.end(), span.end());
    if (sharedEnd > sharedStart) {
      const auto sharedLen = sharedEnd - sharedStart;
      const auto into = static_cast<std::uint32_t>(sharedStart - entry.start);
      const auto extentStart = entry.docOffset + into;
      const auto extentEnd =
          extentStart + static_cast<std::uint32_t>(sharedLen);
      found.push_back(Extent{extentStart, extentEnd});
    }
  }

  // Sort extents by start
  std::ranges::sort(found, [](const Extent &a, const Extent &b) {
    if (a.start != b.start) {
      return a.start < b.start;
    }
    return a.end < b.end;
  });

  // Merge adjacent extents (Ruling R9)
  std::vector<Extent> merged;
  for (const auto &extent : found) {
    if (!merged.empty() && merged.back().end == extent.start) {
      merged.back().end = extent.end;
    } else {
      merged.push_back(extent);
    }
  }
  return merged;
}

void Spanfilade::placeTransclusions(
    const std::vector<const Version *> &views,
    std::vector<TransclusionPair> &pairs) const {
  pairs.clear();
  if (views.size() < 2) {
    return;
  }

  const auto viewCount = views.size();
  std::vector<std::vector<TransclusionPair>> pairBuckets(viewCount * viewCount);

  for (const auto &[scrollId, enfilade] : scrolls_) {
    const auto &pieces = enfilade.entries();
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      const auto &pI = pieces[i];
      if (pI.isCell()) {
        continue;
      }
      for (std::size_t j = i + 1; j < pieces.size(); ++j) {
        const auto &pJ = pieces[j];
        if (pJ.isCell()) {
          continue;
        }
        if (pJ.start >= pI.end()) {
          break;
        }
        if (pI.docId == pJ.docId) {
          continue;
        }

        const auto u   = std::min(pI.docId, pJ.docId);
        const auto v   = std::max(pI.docId, pJ.docId);
        const auto &pU = (pI.docId == u) ? pI : pJ;
        const auto &pV = (pI.docId == v) ? pI : pJ;

        const auto sharedStart = std::max(pU.start, pV.start);
        const auto sharedEnd   = std::min(pU.end(), pV.end());
        if (sharedStart >= sharedEnd) {
          continue;
        }
        const auto sharedLen = sharedEnd - sharedStart;

        const auto startU =
            pU.docOffset + static_cast<std::uint32_t>(sharedStart - pU.start);
        const auto endU = startU + static_cast<std::uint32_t>(sharedLen);

        const auto startV =
            pV.docOffset + static_cast<std::uint32_t>(sharedStart - pV.start);
        const auto endV = startV + static_cast<std::uint32_t>(sharedLen);

        const PrimediaSpan sharedSpan{scrollId, sharedStart, sharedLen};
        auto &bucket = pairBuckets[u * viewCount + v];

        if (!bucket.empty() && bucket.back().from.end == startU &&
            bucket.back().to.end == startV &&
            bucket.back().span.scroll == sharedSpan.scroll &&
            bucket.back().span.end() == sharedSpan.start) {
          bucket.back().from.end = endU;
          bucket.back().to.end   = endV;
          bucket.back().span.length += sharedSpan.length;
        } else {
          TransclusionPair tp{
              .from = LinkEnd{u, startU, endU},
              .to   = LinkEnd{v, startV, endV},
              .span = sharedSpan,
          };
          if (bucket.empty() || !(bucket.back() == tp)) {
            bucket.push_back(tp);
          }
        }
      }
    }
  }

  for (auto &bucket : pairBuckets) {
    for (auto &tp : bucket) {
      pairs.push_back(std::move(tp));
    }
  }
}

bool Spanfilade::verifyAgainstLinearScan(const PrimediaSpan &query,
                                         const Version &ver,
                                         const uint32_t docId) const {
  const auto fast = occurrencesOf(query, docId);
  const auto raw  = ver.occurrencesOf(query);
  return fast == raw;
}

Spanfilade Spanfilade::fromVersion(const Version &ver, const uint32_t docId) {
  Spanfilade filade;
  filade.indexVersion(docId, ver);
  filade.build();
  return filade;
}

Spanfilade Spanfilade::fromViews(const std::vector<const Version *> &views) {
  Spanfilade filade;
  filade.indexViews(views);
  return filade;
}

Spanfilade Spanfilade::fromManifold(const zigzag::Manifold &manifold) {
  Spanfilade filade;
  filade.indexManifold(manifold);
  return filade;
}

} // namespace xanadu::enfilade
