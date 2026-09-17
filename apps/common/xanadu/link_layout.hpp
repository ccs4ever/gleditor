/**
 * @file link_layout.hpp
 * @brief Which links run between which of the documents on screen.
 *
 * A link attaches to content rather than to a position: "a link to any portion
 * is present on all manifestations". So where its two ends have come to rest is
 * not something the link knows -- it is a question about what the open versions
 * are made of, and the answer changes when somebody quotes a passage into a
 * document the link has never heard of.
 *
 * That question is decidable from the store alone, which is why it is here
 * rather than beside the drawing. Given versions and links, these are the
 * connections; turning one into a ribbon between two points in space is the
 * display's business and needs a device, and none of the rules do.
 */
#ifndef XUDU_LINK_LAYOUT_H
#define XUDU_LINK_LAYOUT_H

#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "common/xanadu/universal_link_endpoint.hpp"
#include "ops.hpp"
#include "spool.hpp"
#include "version.hpp"

namespace zigzag {
class Manifold;
} // namespace zigzag

namespace xanadu {

/// One end of a link (re-exported UniversalLinkEnd).
using LinkEnd = UniversalLinkEnd;

/// A link whose two ends both landed in open views (re-exported
/// UniversalLinkedPair).
using LinkedPair = UniversalLinkedPair;

/// A link with one end in an open view and the other in none (re-exported
/// UniversalHalfLink).
using HalfLink = UniversalHalfLink;

/// An emergent transclusion pair (re-exported UniversalTransclusionPair).
using TransclusionPair = UniversalTransclusionPair;

/// High-density 32-byte transclusion pair.
using CompactTransclusion = CompactTransclusionPair;

/**
 * @struct CellAnchor
 * @brief World-space anchor and geometry for a multidimensional Zigzag cell.
 *
 * Provides decoupled geometric resolution for LinkBeams and morphic butterfly
 * ribbons without requiring direct dependencies on ZigzagVisualizer.
 */
struct CellAnchor {
  glm::vec3 position{0.0F, 0.0F, 0.0F}; ///< 3D center in world space
  float width{180.0F};                  ///< Visual quad width
  float height{60.0F};                  ///< Visual quad height
  float lineHeight{14.0F};              ///< Line height within cell
  glm::vec3 normal{0.0F, 0.0F, 1.0F};   ///< Surface normal vector
};

/**
 * @struct UniversalViewContext
 * @brief Unified cross-domain viewing context encompassing 2D Xanadocs and
 *        multidimensional Zigzag manifolds.
 */
struct UniversalViewContext {
  std::vector<const Version *> docViews;
  std::vector<const zigzag::Manifold *> manifoldViews;
  std::vector<zigzag::CellRef>
      manifoldFoci;  ///< Optional focus cell per manifold view (0 = home)
  int cellRadius{3}; ///< Active spatial bounding radius (-1 for unbounded)
};

/**
 * @struct TransclusionLoom
 * @brief Bundled laminar stream of adjacent transclusions along a Zigzag
 * dimension.
 *
 * When consecutive spans of a Xanadoc are transcluded into cells along a single
 * Zigzag dimension rank (e.g. an outline along d.sequence), they form a
 * continuous golden loom rather than criss-crossing separate ribbons.
 */
struct TransclusionLoom {
  std::uint32_t docIndex{0};
  zigzag::DimRef dimension{zigzag::noCell};
  bool posward{true};
  std::vector<std::size_t>
      strandIndices; ///< indices into transclusion pair/strand array
  std::uint32_t docStartOffset{0};
  std::uint32_t docEndOffset{0};
  zigzag::CellRef headCell{zigzag::noCell};
  zigzag::CellRef tailCell{zigzag::noCell};
};

/**
 * @brief Discover emergent transclusions across open documents and cells in
 *        @p ctx.
 */
void placeTransclusions(const UniversalViewContext &ctx,
                        std::vector<TransclusionPair> &pairs);

/**
 * @brief Detect and cluster contiguous transclusions between document passages
 *        and cells along a single manifold dimension rank.
 */
[[nodiscard]] std::vector<TransclusionLoom>
detectTransclusionLooms(const UniversalViewContext &ctx,
                        std::span<const TransclusionPair> pairs);

/**
 * @brief Discover emergent transclusions (shared primedia spans) between open
 *        @p views.
 */
void placeTransclusions(const std::vector<const Version *> &views,
                        std::vector<TransclusionPair> &pairs);

/**
 * @brief Sort @p links into the ones that run between open views in @p ctx
 *        and the ones that run off them.
 */
void placeLinks(const std::map<std::uint64_t, Link> &links,
                const UniversalViewContext &ctx,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving);

/**
 * @brief Sort @p links into the ones that run between @p views and the ones
 *        that run off them.
 *
 * At most one place per document per side. A link end quoted twice into one
 * document is still one relation, and reporting it twice would claim something
 * the link does not say; the extent handed back covers every occurrence, so
 * whatever is drawn lands within what the link points at.
 *
 * Links with both ends in one document are reported as neither: they are
 * shown by shading both passages, and a connection drawn between them would
 * run back across the text it connects.
 *
 * @param views One version per open document, in document order. A null entry
 *        is a document that has nothing to say yet.
 */
void placeLinks(const std::map<std::uint64_t, Link> &links,
                const std::vector<const Version *> &views,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving);

/**
 * @brief Colour a link of @p type and @p tier is shown in, as packed RGBA8.
 *
 * Types are told apart by colour rather than by a label. Opacity reflects the
 * prominence tier (Author > Curated > Public).
 */
[[nodiscard]] std::uint32_t
linkColour(LinkType type, ProminenceTier tier = ProminenceTier::Author);

/**
 * @brief Colour for a specific link instance, applying a subtle deterministic
 *        micro-hue shift so distinct links of the same type are easily told
 * apart while spans of the same link remain strictly identical in colour.
 */
[[nodiscard]] std::uint32_t
linkColourWithInstanceShift(std::uint64_t linkId, LinkType type,
                            ProminenceTier tier = ProminenceTier::Author);

/**
 * @brief Deterministic temporal phase offset for traveling photonic waves.
 *        Spans of the same link share identical phase (pulsing in synchrony),
 *        while separate links pulse asynchronously.
 */
[[nodiscard]] float linkPhaseOffset(std::uint64_t linkId);

/**
 * @brief Deterministic bipolar nudge in [-1, 1] for breaking depth ties
 *        between beams that happen to cross in screen space.
 *
 * Two beams routed between the same pair of documents share almost the same
 * Z, which a depth test resolves as a near-tie that antialiasing noise can
 * flip either way, differing between runs and between renderer backends. A
 * per-beam nudge derived from @p seed (typically a link id, or a tag id for
 * beams with no link of their own) spreads crossing beams a little apart in
 * Z without needing to know how many of them cross, or where: the same seed
 * always nudges the same way, so a beam's depth is stable frame to frame and
 * backend to backend, while different seeds are unlikely to collide.
 */
[[nodiscard]] float linkZJitter(std::uint64_t seed);

} // namespace xanadu

#endif // XUDU_LINK_LAYOUT_H
