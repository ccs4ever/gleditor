#include "link_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

namespace {

/// The extent covering every occurrence of an endset in one document, or
/// nothing when none of it is there.
std::optional<std::pair<std::uint32_t, std::uint32_t>>
coveringExtent(const Version &pieces, const std::vector<PrimediaSpan> &ends) {
  auto first         = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t last = 0;
  bool any           = false;
  for (const auto &span : ends) {
    for (const auto &extent : pieces.occurrencesOf(span)) {
      any   = true;
      first = std::min(first, extent.start);
      last  = std::max(last, extent.end);
    }
  }
  if (!any) {
    return std::nullopt;
  }
  return std::make_pair(first, last);
}

/// The extent covering every occurrence of an endset in one Zigzag cell, or
/// nothing when none of it is there.
std::optional<UniversalLinkEnd>
cellCoveringExtent(const zigzag::Manifold &manifold, const zigzag::CellRef cell,
                   const std::vector<PrimediaSpan> &ends) {
  const auto content = manifold.contentOf(cell);
  if (content.empty()) {
    return std::nullopt;
  }
  auto first                 = std::numeric_limits<std::uint32_t>::max();
  std::uint32_t last         = 0;
  std::uint16_t firstSpanIdx = 0;
  bool any                   = false;

  for (const auto &endSpan : ends) {
    if (endSpan.empty() || isReservedScroll(endSpan.scroll)) {
      continue;
    }
    std::uint32_t seen = 0;
    for (std::size_t sIdx = 0; sIdx < content.size(); ++sIdx) {
      const auto &cellSpan = content[sIdx];
      if (cellSpan.scroll == endSpan.scroll) {
        const auto sharedStart = std::max(cellSpan.start, endSpan.start);
        const auto sharedEnd   = std::min(cellSpan.end(), endSpan.end());
        if (sharedEnd > sharedStart) {
          const auto off =
              seen + static_cast<std::uint32_t>(sharedStart - cellSpan.start);
          const auto len = static_cast<std::uint32_t>(sharedEnd - sharedStart);
          if (!any) {
            firstSpanIdx = static_cast<std::uint16_t>(sIdx);
          }
          any   = true;
          first = std::min(first, off);
          last  = std::max(last, off + len);
        }
      }
      seen += static_cast<std::uint32_t>(cellSpan.length);
    }
  }

  if (!any) {
    return std::nullopt;
  }
  return UniversalLinkEnd::forCell(cell, first, last, firstSpanIdx);
}

struct ViewPiece {
  PrimediaSpan span;
  std::uint32_t targetId{0};   ///< docIndex or CellRef
  std::uint32_t textOffset{0}; ///< offset in doc concatext or cell text
  std::uint16_t spanIndex{0};  ///< index in cell content run (0 for doc)
  LinkTargetKind kind{LinkTargetKind::Document};
};

} // namespace

void placeLinks(const std::map<std::uint64_t, Link> &links,
                const UniversalViewContext &ctx,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving) {
  between.clear();
  leaving.clear();

  for (const auto &[id, link] : links) {
    if (LinkType::Format == link.type) {
      continue;
    }
    std::vector<UniversalLinkEnd> lefts;
    std::vector<UniversalLinkEnd> rights;

    // Document views
    for (std::uint32_t doc = 0; doc < ctx.docViews.size(); ++doc) {
      if (nullptr == ctx.docViews[doc]) {
        continue;
      }
      if (const auto extent = coveringExtent(*ctx.docViews[doc], link.left)) {
        lefts.push_back(
            UniversalLinkEnd::forDocument(doc, extent->first, extent->second));
      }
      if (const auto extent = coveringExtent(*ctx.docViews[doc], link.right)) {
        rights.push_back(
            UniversalLinkEnd::forDocument(doc, extent->first, extent->second));
      }
    }

    // Manifold views
    for (std::size_t mIdx = 0; mIdx < ctx.manifoldViews.size(); ++mIdx) {
      if (nullptr == ctx.manifoldViews[mIdx]) {
        continue;
      }
      const auto &manifold = *ctx.manifoldViews[mIdx];
      const auto focus     = (mIdx < ctx.manifoldFoci.size())
                                 ? ctx.manifoldFoci[mIdx]
                                 : zigzag::noCell;
      const auto allowed   = manifold.cellsWithinRadius(focus, ctx.cellRadius);
      for (const auto cell : allowed) {
        if (const auto endPt = cellCoveringExtent(manifold, cell, link.left)) {
          lefts.push_back(*endPt);
        }
        if (const auto endPt = cellCoveringExtent(manifold, cell, link.right)) {
          rights.push_back(*endPt);
        }
      }
    }

    for (const auto &left : lefts) {
      for (const auto &right : rights) {
        if (left == right) {
          continue;
        }
        if (left.isDocument() && right.isDocument() && left.doc == right.doc) {
          continue;
        }
        if (left.isCell() && right.isCell() && left.cell() == right.cell()) {
          continue;
        }
        between.push_back(LinkedPair{.link = id,
                                     .type = link.type,
                                     .tier = link.tier,
                                     .from = left,
                                     .to   = right});
      }
    }

    if (lefts.empty() != rights.empty()) {
      leaving.push_back(
          HalfLink{.link      = id,
                   .type      = link.type,
                   .tier      = link.tier,
                   .here      = lefts.empty() ? rights.front() : lefts.front(),
                   .elsewhere = lefts.empty() ? link.left : link.right});
    }
  }
}

void placeLinks(const std::map<std::uint64_t, Link> &links,
                const std::vector<const Version *> &views,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving) {
  placeLinks(links, UniversalViewContext{.docViews = views}, between, leaving);
}

void placeTransclusions(const UniversalViewContext &ctx,
                        std::vector<TransclusionPair> &pairs) {
  pairs.clear();

  std::vector<ViewPiece> pieces;

  // 1. Collect pieces from document views
  for (std::uint32_t i = 0; i < ctx.docViews.size(); ++i) {
    if (nullptr == ctx.docViews[i]) {
      continue;
    }
    const auto &doc    = *ctx.docViews[i];
    std::uint32_t seen = 0;
    for (const auto &run : doc.pieces()) {
      if (!run.empty() && breakMarkerScroll != run.scroll) {
        pieces.push_back(ViewPiece{
            .span       = run,
            .targetId   = i,
            .textOffset = seen,
            .spanIndex  = 0,
            .kind       = LinkTargetKind::Document,
        });
      }
      seen += static_cast<std::uint32_t>(run.length);
    }
  }

  // 2. Collect pieces from manifold cells within radius
  for (std::size_t mIdx = 0; mIdx < ctx.manifoldViews.size(); ++mIdx) {
    if (nullptr == ctx.manifoldViews[mIdx]) {
      continue;
    }
    const auto &manifold = *ctx.manifoldViews[mIdx];
    const auto focus = (mIdx < ctx.manifoldFoci.size()) ? ctx.manifoldFoci[mIdx]
                                                        : zigzag::noCell;
    const auto allowed = manifold.cellsWithinRadius(focus, ctx.cellRadius);
    for (const auto cell : allowed) {
      const auto content = manifold.contentOf(cell);
      std::uint32_t seen = 0;
      for (std::size_t sIdx = 0; sIdx < content.size(); ++sIdx) {
        const auto &span = content[sIdx];
        if (!span.empty() && !isReservedScroll(span.scroll)) {
          pieces.push_back(ViewPiece{
              .span       = span,
              .targetId   = static_cast<std::uint32_t>(cell),
              .textOffset = seen,
              .spanIndex  = static_cast<std::uint16_t>(sIdx),
              .kind       = LinkTargetKind::ZigzagCell,
          });
        }
        seen += static_cast<std::uint32_t>(span.length);
      }
    }
  }

  if (pieces.size() < 2) {
    return;
  }

  // 3. Sort pieces by (scroll, start, kind, targetId)
  std::ranges::sort(pieces, [](const ViewPiece &a, const ViewPiece &b) {
    if (a.span.scroll != b.span.scroll) {
      return a.span.scroll < b.span.scroll;
    }
    if (a.span.start != b.span.start) {
      return a.span.start < b.span.start;
    }
    if (a.kind != b.kind) {
      return static_cast<std::uint8_t>(a.kind) <
             static_cast<std::uint8_t>(b.kind);
    }
    return a.targetId < b.targetId;
  });

  // 4. Pair sweeping with bucketed adjacent run merging
  std::map<std::pair<std::pair<std::uint8_t, std::uint32_t>,
                     std::pair<std::uint8_t, std::uint32_t>>,
           std::vector<TransclusionPair>>
      pairBuckets;

  for (std::size_t i = 0; i < pieces.size(); ++i) {
    const auto &pI = pieces[i];
    for (std::size_t j = i + 1; j < pieces.size(); ++j) {
      const auto &pJ = pieces[j];
      if (pJ.span.scroll != pI.span.scroll || pJ.span.start >= pI.span.end()) {
        break;
      }
      if (pI.kind == pJ.kind && pI.targetId == pJ.targetId) {
        continue;
      }

      bool iIsFrom = true;
      if (pI.kind != pJ.kind) {
        iIsFrom = (pI.kind == LinkTargetKind::Document);
      } else {
        iIsFrom = (pI.targetId < pJ.targetId);
      }
      const auto &pU = iIsFrom ? pI : pJ;
      const auto &pV = iIsFrom ? pJ : pI;

      const auto sharedStart = std::max(pU.span.start, pV.span.start);
      const auto sharedEnd   = std::min(pU.span.end(), pV.span.end());
      if (sharedStart >= sharedEnd) {
        continue;
      }
      const auto sharedLen = sharedEnd - sharedStart;

      const auto startU = pU.textOffset + static_cast<std::uint32_t>(
                                              sharedStart - pU.span.start);
      const auto endU   = startU + static_cast<std::uint32_t>(sharedLen);

      const auto startV = pV.textOffset + static_cast<std::uint32_t>(
                                              sharedStart - pV.span.start);
      const auto endV   = startV + static_cast<std::uint32_t>(sharedLen);

      const auto endPtU =
          (pU.kind == LinkTargetKind::Document)
              ? UniversalLinkEnd::forDocument(pU.targetId, startU, endU)
              : UniversalLinkEnd::forCell(
                    static_cast<zigzag::CellRef>(pU.targetId), startU, endU,
                    pU.spanIndex);

      const auto endPtV =
          (pV.kind == LinkTargetKind::Document)
              ? UniversalLinkEnd::forDocument(pV.targetId, startV, endV)
              : UniversalLinkEnd::forCell(
                    static_cast<zigzag::CellRef>(pV.targetId), startV, endV,
                    pV.spanIndex);

      const PrimediaSpan sharedSpan{
          .scroll = pU.span.scroll, .start = sharedStart, .length = sharedLen};
      const auto bucketKey = std::make_pair(
          std::make_pair(static_cast<std::uint8_t>(pU.kind), pU.targetId),
          std::make_pair(static_cast<std::uint8_t>(pV.kind), pV.targetId));
      auto &bucket = pairBuckets[bucketKey];

      if (!bucket.empty() && bucket.back().from.end == startU &&
          bucket.back().to.end == startV &&
          bucket.back().span.scroll == sharedSpan.scroll &&
          bucket.back().span.end() == sharedSpan.start) {
        bucket.back().from.end = endU;
        bucket.back().to.end   = endV;
        bucket.back().span.length += sharedSpan.length;
      } else {
        TransclusionPair tp{
            .from = endPtU,
            .to   = endPtV,
            .span = sharedSpan,
        };
        if (bucket.empty() || !(bucket.back() == tp)) {
          bucket.push_back(tp);
        }
      }
    }
  }

  for (auto &[key, bucket] : pairBuckets) {
    for (auto &tp : bucket) {
      pairs.push_back(std::move(tp));
    }
  }
}

void placeTransclusions(const std::vector<const Version *> &views,
                        std::vector<TransclusionPair> &pairs) {
  placeTransclusions(UniversalViewContext{.docViews = views}, pairs);
}

std::vector<TransclusionLoom>
detectTransclusionLooms(const UniversalViewContext &ctx,
                        const std::span<const TransclusionPair> pairs) {
  if (pairs.size() < 2 || ctx.manifoldViews.empty()) {
    return {};
  }

  struct Candidate {
    std::size_t pairIdx{0};
    std::uint32_t docIndex{0};
    std::uint32_t docStart{0};
    std::uint32_t docEnd{0};
    zigzag::CellRef cell{zigzag::noCell};
  };

  std::unordered_map<std::uint32_t, std::vector<Candidate>> byDoc;
  for (std::size_t i = 0; i < pairs.size(); ++i) {
    const auto &p = pairs[i];
    if (p.from.isDocument() && p.to.isCell()) {
      byDoc[p.from.doc].push_back(Candidate{
          .pairIdx  = i,
          .docIndex = p.from.doc,
          .docStart = p.from.start,
          .docEnd   = p.from.end,
          .cell     = p.to.cell(),
      });
    } else if (p.from.isCell() && p.to.isDocument()) {
      byDoc[p.to.doc].push_back(Candidate{
          .pairIdx  = i,
          .docIndex = p.to.doc,
          .docStart = p.to.start,
          .docEnd   = p.to.end,
          .cell     = p.from.cell(),
      });
    }
  }

  std::vector<TransclusionLoom> result;

  for (auto &[docIdx, candidates] : byDoc) {
    if (candidates.size() < 2) {
      continue;
    }

    std::ranges::sort(candidates, [](const Candidate &a, const Candidate &b) {
      if (a.docStart != b.docStart) {
        return a.docStart < b.docStart;
      }
      if (a.docEnd != b.docEnd) {
        return a.docEnd < b.docEnd;
      }
      return a.pairIdx < b.pairIdx;
    });

    std::vector<bool> used(candidates.size(), false);

    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (used[i]) {
        continue;
      }

      for (const auto *m : ctx.manifoldViews) {
        if (nullptr == m) {
          continue;
        }

        for (const auto dir :
             {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
          for (const auto &dimLink : m->dimensionsOf(candidates[i].cell)) {
            const auto dim = dimLink.dim;
            if (dim == zigzag::noCell) {
              continue;
            }

            std::vector<std::size_t> chainCandidateIndices;
            chainCandidateIndices.push_back(i);
            auto curCell = candidates[i].cell;

            for (std::size_t j = i + 1; j < candidates.size(); ++j) {
              if (used[j]) {
                continue;
              }
              const auto expectedNext = m->linked(curCell, dim, dir);
              if (expectedNext == zigzag::noCell || expectedNext == curCell) {
                break;
              }
              if (candidates[j].cell == expectedNext) {
                if (candidates[j].docStart >=
                    candidates[chainCandidateIndices.back()].docStart) {
                  chainCandidateIndices.push_back(j);
                  curCell = expectedNext;
                }
              }
            }

            if (chainCandidateIndices.size() >= 2) {
              TransclusionLoom loom;
              loom.docIndex  = docIdx;
              loom.dimension = dim;
              loom.posward   = (dir == zigzag::DimVector::POS);
              loom.docStartOffset =
                  candidates[chainCandidateIndices.front()].docStart;
              loom.docEndOffset =
                  candidates[chainCandidateIndices.back()].docEnd;
              loom.headCell = candidates[chainCandidateIndices.front()].cell;
              loom.tailCell = candidates[chainCandidateIndices.back()].cell;
              for (const auto cIdx : chainCandidateIndices) {
                loom.strandIndices.push_back(candidates[cIdx].pairIdx);
                used[cIdx] = true;
              }
              result.push_back(std::move(loom));
              break;
            }
          }
          if (used[i]) {
            break;
          }
        }
        if (used[i]) {
          break;
        }
      }
    }
  }

  return result;
}

std::uint32_t linkColour(const LinkType type, const ProminenceTier tier) {
  std::uint32_t rgb = 0xCFCFCF00U;
  switch (type) {
  case LinkType::Comment:
    rgb = 0x7FB2FF00U;
    break;
  case LinkType::Illustration:
    rgb = 0xFFC46B00U;
    break;
  case LinkType::Disagreement:
    rgb = 0xFF7A6B00U;
    break;
  case LinkType::Authorship:
    rgb = 0xB98CFF00U;
    break;
  case LinkType::Quotation:
    rgb = 0x7FE0A800U;
    break;
  case LinkType::Other:
    rgb = 0xCFCFCF00U;
    break;
  case LinkType::Format:
    // Not drawn as a highlighted passage at all in the end -- a format link
    // changes the glyphs themselves, which is a shaping concern rather than
    // the background-colour one this function answers -- but a case is kept
    // here so adding it did not leave this switch silently wrong about a
    // link type it does not know how to colour.
    rgb = 0xCFCFCF00U;
    break;
  }

  std::uint32_t alpha = 0xE0U;
  switch (tier) {
  case ProminenceTier::Author:
    alpha = 0xE0U;
    break;
  case ProminenceTier::Curated:
    alpha = 0xB0U;
    break;
  case ProminenceTier::Public:
    alpha = 0x60U;
    break;
  }
  return rgb | alpha;
}

std::uint32_t linkColourWithInstanceShift(const std::uint64_t linkId,
                                          const LinkType type,
                                          const ProminenceTier tier) {
  const auto base = linkColour(type, tier);
  if (0 == linkId) {
    return base;
  }
  const float r = static_cast<float>((base >> 24) & 0xFFU) / 255.0F;
  const float g = static_cast<float>((base >> 16) & 0xFFU) / 255.0F;
  const float b = static_cast<float>((base >> 8) & 0xFFU) / 255.0F;
  const auto a  = base & 0xFFU;

  const float maxVal = std::max({r, g, b});
  const float minVal = std::min({r, g, b});
  const float delta  = maxVal - minVal;

  float h = 0.0F;
  if (delta > 0.0001F) {
    if (maxVal == r) {
      h = std::fmod((g - b) / delta, 6.0F);
    } else if (maxVal == g) {
      h = ((b - r) / delta) + 2.0F;
    } else {
      h = ((r - g) / delta) + 4.0F;
    }
    h /= 6.0F;
    if (h < 0.0F) {
      h += 1.0F;
    }
  }
  const float s = maxVal > 0.0001F ? delta / maxVal : 0.0F;
  const float v = maxVal;

  // Deterministic micro-hue offset of +/- 7% based on golden ratio hash of
  // linkId
  const float hashFrac =
      static_cast<float>((linkId * 0x9E3779B97F4A7C15ULL) % 10000ULL) /
      10000.0F;
  const float hueShift = (hashFrac - 0.5F) * 0.14F;
  float newH           = std::fmod(h + hueShift + 1.0F, 1.0F);

  // Convert back to RGB
  const float c = v * s;
  const float x = c * (1.0F - std::abs(std::fmod(newH * 6.0F, 2.0F) - 1.0F));
  const float m = v - c;

  float newR        = 0.0F;
  float newG        = 0.0F;
  float newB        = 0.0F;
  const int hSector = static_cast<int>(newH * 6.0F) % 6;
  switch (hSector) {
  case 0:
    newR = c;
    newG = x;
    newB = 0.0F;
    break;
  case 1:
    newR = x;
    newG = c;
    newB = 0.0F;
    break;
  case 2:
    newR = 0.0F;
    newG = c;
    newB = x;
    break;
  case 3:
    newR = 0.0F;
    newG = x;
    newB = c;
    break;
  case 4:
    newR = x;
    newG = 0.0F;
    newB = c;
    break;
  case 5:
  default:
    newR = c;
    newG = 0.0F;
    newB = x;
    break;
  }

  const auto outR =
      static_cast<std::uint32_t>(std::clamp((newR + m) * 255.0F, 0.0F, 255.0F));
  const auto outG =
      static_cast<std::uint32_t>(std::clamp((newG + m) * 255.0F, 0.0F, 255.0F));
  const auto outB =
      static_cast<std::uint32_t>(std::clamp((newB + m) * 255.0F, 0.0F, 255.0F));

  return (outR << 24) | (outG << 16) | (outB << 8) | a;
}

float linkPhaseOffset(const std::uint64_t linkId) {
  return static_cast<float>((linkId * 0x517CC1B727220A95ULL) % 10000ULL) /
         10000.0F;
}

float linkZJitter(const std::uint64_t seed) {
  // A third multiplicative constant, distinct from the hue-shift and
  // phase-offset hashes above, so the three derived values don't correlate.
  const float hashFrac =
      static_cast<float>((seed * 0xBF58476D1CE4E5B9ULL) % 10000ULL) / 10000.0F;
  return (hashFrac * 2.0F) - 1.0F;
}

} // namespace xanadu
