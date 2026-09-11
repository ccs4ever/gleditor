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
#include <unordered_map>
#include <vector>

#include "common/xanadu/link_package.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/scroll.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"

namespace zigzag {

/// Input document description for Xudu -> Zigzag projection.
struct XuduDocInput {
  std::string name;
  std::string text;
  xanadu::MicroversionId version;
  std::vector<xanadu::PrimediaSpan> spans;
};

/// Configuration options for projecting Xudu documents into Zigzag space.
struct XuduProjectorOptions {
  DimID doc_dimension{"d.doc"};
  DimID transclusion_dimension{"d.transclude"};
  DimID link_dimension{"d.link"};
  DimID version_dimension{"d.version"};
  DimID clone_dimension{"d.clone"};
  bool split_by_paragraphs{true};
};

/// Result of linearizing / rasterizing a Zigzag space into a readable text
/// stream.
struct ZzRasterResult {
  std::string text;
  std::vector<CellID> cell_sequence;
  std::vector<std::size_t> line_breaks;
};

/**
 * @brief Project a collection of Xudu documents and their xanalinks into a
 *        multidimensional Zigzag structure.
 *
 * Each document span / paragraph becomes a discrete Cell.
 * - @p doc_dimension links sequential spans within each document.
 * - @p transclusion_dimension links cells sharing overlapping primedia spans.
 * - @p link_dimension links xanalink endpoints across documents.
 */
[[nodiscard]] ZzStructureDocument
projectXuduToZigzag(const std::vector<XuduDocInput> &docs,
                    const std::vector<xanadu::Link> &links,
                    const XuduProjectorOptions &opts = {});

/**
 * @brief Project a Xudu Store and its active microversions into a Zigzag
 * structure.
 */
[[nodiscard]] ZzStructureDocument
projectStoreToZigzag(const xanadu::Store &store,
                     const std::vector<xanadu::MicroversionId> &versions,
                     const XuduProjectorOptions &opts = {});

/**
 * @brief Linearize / rasterize a Zigzag manifold into a continuous text stream
 *        suitable for Xanadoc editing or reading.
 *
 * Traverses cells starting from @p startCell (or document focus if 0) along
 * @p primaryDim (e.g. lines/sentences) and optionally @p secondaryDim (e.g.
 * paragraphs).
 */
[[nodiscard]] ZzRasterResult rasterizeZzStructure(
    const ZzStructureDocument &doc, const DimID &primaryDim = "d.doc",
    const DimID &secondaryDim = "d.transclude", CellID startCell = 0);

/**
 * @brief Convert a Zigzag structure document into a signed, standalone
 * xanadu::LinkPackage.
 *
 * Dimensional connections are encoded as typed Xanalinks (LinkType::Dimension)
 * with owner "dim:<dimension>".
 */
[[nodiscard]] xanadu::LinkPackage zzStructureToLinkPackage(
    const ZzStructureDocument &doc, const xanadu::MutableKeys &keys,
    const std::string &salt = "zigzag_slice", std::int64_t sequence = 1);

/**
 * @brief Convert a slice Store and its Manifold into a signed, standalone
 *        xanadu::LinkPackage using real primedia spans from the store.
 */
[[nodiscard]] xanadu::LinkPackage
storeToLinkPackage(const xanadu::Store &store, const Manifold &manifold,
                   const xanadu::MutableKeys &keys,
                   const std::string &salt = "zigzag_slice",
                   std::int64_t sequence = 1, const std::string &title = "");

/**
 * @brief Convert a xanadu::LinkPackage containing dimensional links back into a
 *        ZzStructureDocument.
 */
[[nodiscard]] ZzStructureDocument
linkPackageToZzStructure(const xanadu::LinkPackage &pkg);

// -- a slice is a store ------------------------------------------------------
//
// Migration step 20. A YAML slice stops being a *model* and becomes a transfer
// format: sliceToStore() mints it as OpKind::Structure operations, and the
// Manifold folded out of those operations is what anything downstream reads.
// storeToSlice() is the other direction, for export and for round-trip tests.
//
// No slice anywhere has to keep loading as YAML -- the sample and system slices
// are regenerated as stores -- so this is a conversion rather than a
// compatibility layer, and nothing here is obliged to preserve a shape the
// convergence is removing.

/// What a slice became once it was minted: the state its last operation
/// produced, and the mapping from the YAML's own ids to cell references.
struct SlicedStore {
  xanadu::MicroversionId version;
  /// YAML cell id -> CellRef, which is the index of the operation that minted
  /// the cell. The YAML's ids do not survive: a cell's identity is now its name
  /// in hypertime. See R4.
  std::unordered_map<CellID, CellRef> cells;
  /// Dimension name -> the cell that *is* that dimension (R2).
  std::unordered_map<DimID, DimRef> dimensions;
  CellRef focus{noCell};
};

/**
 * @brief Mint @p doc into @p store as Structure operations.
 *
 * Calls Store::sliceGenesis() first when the store has no home cell yet, so
 * that the d.dims rank exists before a dimension is minted onto it.
 *
 * A cell holding a number or a flag is minted as a scalar cell (R6), so it
 * carries canonical bits as well as a rendering. A cell's `role`, `mime_type`
 * and `media_path` have no field to live in any more and become **cells on
 * their own ranks** -- `d.role`, `d.mime`, `d.media` -- which is R13's answer
 * for metadata and what a zzstructure is for. storeToSlice() reads them back
 * off those ranks, so a round trip keeps them.
 *
 * Only posward links are emitted, plus any negward link the document does not
 * express reciprocally: the fold maintains both ends of an edge, so emitting a
 * link twice restates the same edge rather than adding anything.
 *
 * Deterministic: cells and dimensions are minted in sorted order, so one
 * document always produces one operation sequence. A conversion that depended
 * on hash iteration order would write a different store every run, and no
 * fixture could be regenerated.
 */
[[nodiscard]] SlicedStore
sliceToStore(const ZzStructureDocument &doc, xanadu::Store &store,
             const xanadu::MicroversionId &parent = {});

/**
 * @brief The YAML document a manifold describes, for export.
 *
 * The inverse of sliceToStore() up to cell ids: a CellRef is an operation index
 * and is used directly as the YAML id, so a round trip renumbers cells rather
 * than restoring whatever numbers the original file used.
 */
[[nodiscard]] ZzStructureDocument storeToSlice(const xanadu::Store &store,
                                               const Manifold &manifold,
                                               CellRef focus = noCell);

/**
 * @brief Validate that a Zigzag structure strictly satisfies the 2-rank
 * manifold invariant (at most 1 positive and 1 negative link per dimension per
 * cell).
 */
[[nodiscard]] bool validate2RankManifold(const ZzStructureDocument &doc,
                                         std::string *errorOut = nullptr);

/**
 * @brief Verify a Zigzag Slice's declared author against a verified Merkle
 *        identity ledger root.
 */
[[nodiscard]] bool
verifySliceAuthor(const ZzStructureDocument &doc,
                  const xanadu::MerkleLedger &ledger,
                  const std::array<std::uint8_t, 32> &expectedRoot,
                  std::string *errorOut = nullptr);

} // namespace zigzag

#endif // ZIGZAG_XUDU_PROJECTOR_HPP
