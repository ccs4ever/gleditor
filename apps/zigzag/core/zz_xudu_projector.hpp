/**
 * @file zz_xudu_projector.hpp
 * @brief Bidirectional projection and rasterization between Xudu (Xanadocs /
 * Xanalinks) and Project Xanadu Zigzag (Multidimensional cell space).
 */
#ifndef ZIGZAG_XUDU_PROJECTOR_HPP
#define ZIGZAG_XUDU_PROJECTOR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xudu/core/link_package.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/ops.hpp"
#include "xudu/core/scroll.hpp"
#include "xudu/core/store.hpp"
#include "zzstructure.hpp"

namespace zigzag {

/// Input document description for Xudu -> Zigzag projection.
struct XuduDocInput {
  std::string name;
  std::string text;
  xudu::MicroversionId version;
  std::vector<xudu::PrimediaSpan> spans;
};

/// Configuration options for projecting Xudu documents into Zigzag space.
struct XuduProjectorOptions {
  DimID doc_dimension{"d.doc"};
  DimID transclusion_dimension{"d.transclude"};
  DimID link_dimension{"d.link"};
  DimID version_dimension{"d.version"};
  bool split_by_paragraphs{true};
};

/// Result of linearizing / rasterizing a Zigzag space into a readable text stream.
struct ZzRasterResult {
  std::string text;
  std::vector<CellID> cell_sequence;
  std::vector<std::size_t> line_breaks;
};

/**
 * @brief Project a collection of Xudu documents and their xanalinks into a
 *        multidimensional Zigzag structure.
 *
 * Each document span / paragraph becomes a discrete zzCell.
 * - @p doc_dimension links sequential spans within each document.
 * - @p transclusion_dimension links cells sharing overlapping primedia spans.
 * - @p link_dimension links xanalink endpoints across documents.
 */
[[nodiscard]] ZzStructureDocument
projectXuduToZigzag(const std::vector<XuduDocInput> &docs,
                    const std::vector<xudu::Link> &links,
                    const XuduProjectorOptions &opts = {});

/**
 * @brief Project a Xudu Store and its active microversions into a Zigzag structure.
 */
[[nodiscard]] ZzStructureDocument
projectStoreToZigzag(const xudu::Store &store,
                     const std::vector<xudu::MicroversionId> &versions,
                     const XuduProjectorOptions &opts = {});

/**
 * @brief Linearize / rasterize a Zigzag manifold into a continuous text stream
 *        suitable for Xanadoc editing or reading.
 *
 * Traverses cells starting from @p startCell (or document focus if 0) along
 * @p primaryDim (e.g. lines/sentences) and optionally @p secondaryDim (e.g. paragraphs).
 */
[[nodiscard]] ZzRasterResult
rasterizeZzStructure(const ZzStructureDocument &doc,
                     const DimID &primaryDim   = "d.doc",
                     const DimID &secondaryDim = "d.transclude",
                     CellID startCell          = 0);

/**
 * @brief Convert a Zigzag structure document into a signed, standalone xudu::LinkPackage.
 *
 * Dimensional connections are encoded as typed Xanalinks (LinkType::Dimension)
 * with owner "dim:<dimension>".
 */
[[nodiscard]] xudu::LinkPackage
zzStructureToLinkPackage(const ZzStructureDocument &doc,
                         const xudu::MutableKeys &keys,
                         const std::string &salt = "zigzag_slice",
                         std::int64_t sequence   = 1);

/**
 * @brief Convert a xudu::LinkPackage containing dimensional links back into a
 *        ZzStructureDocument.
 */
[[nodiscard]] ZzStructureDocument
linkPackageToZzStructure(const xudu::LinkPackage &pkg);

/**
 * @brief Validate that a Zigzag structure strictly satisfies the 2-rank manifold
 *        invariant (at most 1 positive and 1 negative link per dimension per cell).
 */
[[nodiscard]] bool
validate2RankManifold(const ZzStructureDocument &doc,
                      std::string *errorOut = nullptr);

} // namespace zigzag

#endif // ZIGZAG_XUDU_PROJECTOR_HPP
