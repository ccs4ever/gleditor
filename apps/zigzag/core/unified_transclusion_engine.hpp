/**
 * @file unified_transclusion_engine.hpp
 * @brief High-performance coordinator bridging Xudu storage, Zigzag topology,
 *        and Gleditor GPU text streaming.
 */
#ifndef ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP
#define ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/compact_op.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/compact_zzcell.hpp"
#include "common/xanadu/zigzag/manifold.hpp"
#include "common/xanadu/zigzag/zzstructure.hpp"
#include "gleditor/doc.hpp"
#include "gleditor/glyphcache/cache.hpp"
#include "gleditor/render/stream_buffer.hpp"
#include "gleditor/text/font.hpp"
#include "gleditor/text/layout.hpp"

namespace zigzag {

/**
 * @class UnifiedTransclusionEngine
 * @brief Coordinates incremental topological mapping from Xudu append-only
 *        operations spools into multidimensional ZigZag ranks, performing
 *        zero-copy text resolution and GPU instance buffer staging.
 */
class UnifiedTransclusionEngine {
public:
  explicit UnifiedTransclusionEngine(xanadu::Store &store);
  ~UnifiedTransclusionEngine() = default;

  UnifiedTransclusionEngine(const UnifiedTransclusionEngine &) = delete;
  UnifiedTransclusionEngine &
  operator=(const UnifiedTransclusionEngine &)                       = delete;
  UnifiedTransclusionEngine(UnifiedTransclusionEngine &&) noexcept   = default;
  UnifiedTransclusionEngine &operator=(UnifiedTransclusionEngine &&) = delete;

  /// What a cell carries that a CellSlot deliberately does not: how its content
  /// resolved, and why it might not have. R12's sketch calls this ColdCell and
  /// puts transcopyright and holes in it; `type` is here too because a cell's
  /// role is a rank in the stored model (see sliceToStore's d.role) and this is
  /// the render side's cache of it, not a second home for it.
  struct ColdCell {
    std::string type{"cell"};
    xanadu::ResolutionStatus resolutionStatus{
        xanadu::ResolutionStatus::VerifiedBytes};
    std::optional<xanadu::TranscopyrightDescriptor> transcopyrightInfo{};
    std::optional<xanadu::PublishedHoleRecord> holeRecord{};
  };

  // -- Topological Synchronization ------------------------------------------

  /**
   * @brief Fold operations recorded since the last call into the manifold.
   *
   * Structure operations make cells and links; every other kind is ignored,
   * because a cell is an operation that minted one. See the implementation for
   * what this replaced and why none of it could survive.
   */
  void syncIncremental();

  /// The structure map this engine reads. The model, not a cache of one.
  [[nodiscard]] const Manifold &manifold() const noexcept { return manifold_; }

  /// The state the engine's own minting appends to, which advances as it does.
  [[nodiscard]] const xanadu::MicroversionId &head() const noexcept {
    return head_;
  }

  [[nodiscard]] xanadu::Store &store() noexcept { return store_; }
  [[nodiscard]] const xanadu::Store &store() const noexcept { return store_; }

  /**
   * @brief The cell that *is* dimension @p name, minting it if the slice has
   *        none.
   *
   * A dimension is a cell (R2), so naming one is a lookup along the d.dims rank
   * rather than an enum value.
   */
  DimRef dimensionFor(std::string_view name);

  /**
   * @brief The dimension `d.meta-dims` is read along -- a sentinel, not a cell.
   *
   * R12 is explicit that `d.meta-dims` is "generated at runtime and stored in
   * no op": it *is* a cell's CSR run, read sideways. So it cannot be a minted
   * dimension cell, and this answers a reserved reference with `ephemeralBit`
   * set instead -- the bit that means "derived: no op backs this".
   *
   * That is what keeps every read path const and silent. This used to be
   * `dimensionFor("d.meta-dims")` reached through a `const_cast` from inside
   * `linked()`, so asking a cell for its neighbour could append two operations
   * to the document, from the render loop. `Manifold::applyStructure()` refuses
   * a link whose dimension is ephemeral, so the sentinel additionally cannot be
   * persisted by mistake.
   */
  [[nodiscard]] static constexpr DimRef metaDimension() noexcept {
    return ephemeralBit | 1U;
  }

  /// Link @p a to @p b along @p dim, by
  /// recording one Structure operation. noCell for @p b clears the link. The
  /// reciprocal edge is what the fold means by a link, not a second write.
  void linkCells(CellRef a, CellRef b, DimRef dim,
                 DimVector dir = DimVector::POS);
  void linkCells(CellRef a, CellRef b, DirectedDim target) {
    linkCells(a, b, target.dim, target.dir);
  }
  void linkCells(CellRef a, CellRef b, DimOrdinal dim,
                 DimVector dir = DimVector::POS);
  void linkCells(CellRef a, CellRef b, const DimID &dim,
                 DimVector dir = DimVector::POS);

  void linkCells(CellRef a, CellRef b, DimRef dim, bool negward) {
    linkCells(a, b, dim, fromNegward(negward));
  }
  void linkCells(CellRef a, CellRef b, DimOrdinal dim, bool negward) {
    linkCells(a, b, dim, fromNegward(negward));
  }
  void linkCells(CellRef a, CellRef b, const DimID &dim, bool negward) {
    linkCells(a, b, dim, fromNegward(negward));
  }

  /// Sugar for linkCells(a, noCell, dim, DimVector::POS).
  void unlinkPositive(CellRef a, DimOrdinal dim);

  /// Mint a cell whose content is @p text, and answer the operation index that
  /// names it. Adding a cell is recording one now: there is nowhere for a cell
  /// with no operation behind it to live.
  CellRef addCell(std::string_view text);

  /// Restate @p cell's content as @p text, recording a SetValue operation.
  void updateCellText(CellRef cell, std::string_view text);

  void setCold(CellRef cell, ColdCell cold);
  [[nodiscard]] const ColdCell *coldOf(CellRef cell) const noexcept;

  [[nodiscard]] const CellSlot *findCell(CellRef cell) const noexcept;
  [[nodiscard]] std::size_t cellCount() const noexcept {
    return manifold_.cellCount();
  }

  // -- Invariant Verification -----------------------------------------------

  /**
   * @brief Validate that the entire cell space strictly conforms to the
   *        2-rank manifold invariant (at most 1 pos and 1 neg per dimension).
   */
  [[nodiscard]] bool
  validate2RankManifold(std::string *errorOut = nullptr) const;

  // -- Text & Spool Resolution ----------------------------------------------

  /**
   * @brief Resolve cell content via the underlying Store.
   */
  [[nodiscard]] std::string resolveCellText(CellRef cell) const;

  /**
   * @brief Resolve zero-copy string view if cell is backed by local primedia
   *        spool.
   */
  [[nodiscard]] std::string_view
  resolveLocalCellView(CellRef cell) const noexcept;

  // -- Topological Traversal & Meta-Dimensions -------------------------------

  /**
   * @brief Every dimension @p cell has active connections on, including
   *        d.meta-dims itself.
   */
  [[nodiscard]] std::vector<DimRef> metaDimensionsOf(CellRef cell) const;

  /**
   * @brief Neighbor of @p from along @p dim. Seamlessly resolves stored
   *        manifold links, ephemeral d.meta-dims ranks, and d.clone
   * projections.
   */
  [[nodiscard]] CellRef linked(CellRef from, DimRef dim,
                               DimVector dir = DimVector::POS) const;
  [[nodiscard]] CellRef linked(CellRef from, DirectedDim target) const {
    return linked(from, target.dim, target.dir);
  }
  [[nodiscard]] CellRef linked(CellRef from, DimRef dim, bool negward) const {
    return linked(from, dim, fromNegward(negward));
  }

  /**
   * @brief The clone master of @p cell along @p cloneDim. Resolves ephemeral
   *        d.meta-dims clone cells to their real dimension cells on d.dims.
   */
  [[nodiscard]] CellRef cloneMaster(CellRef cell, DimRef cloneDim) const;

  /**
   * @brief Whether @p cell is protected from deletion (e.g. home, d.dims,
   *        dimension cells on d.dims, ephemeral cells).
   */
  [[nodiscard]] bool isProtected(CellRef cell) const;

  // -- Conversion & Compatibility -------------------------------------------
  //
  // toZzStructureDocument() and loadFromZzStructureDocument() were here, and
  // migration step 19 deleted them. The YAML document is a transfer format now,
  // not a second cell space: sliceToStore() in zz_xudu_projector mints one as
  // Structure operations and storeToSlice() reads a Manifold back out, both
  // tested against the real sample slice. Ingesting a document by filling in
  // cells that no operation minted is exactly the arrangement the convergence
  // exists to remove -- and it was the last thing keeping
  // CompactZZCell::ephemeralText alive.

  // -- Zero-Copy GPU Render Staging -----------------------------------------

  struct RenderSliceRequest {
    CellRef focusCellId{1};
    DimID axisX{"d.1"};
    DimID axisY{"d.2"};
    DimID axisZ{"d.3"};
    int radiusX{3};
    int radiusY{3};
    int radiusZ{1};
  };

  struct RenderInstanceBatch {
    std::vector<Doc::VBORow> rows;
    std::size_t instanceCount{0};
  };

  /**
   * @brief Layout and stage visible cells in the neighborhood of @p req into
   *        Doc::VBORow instance quads.
   */
  [[nodiscard]] RenderInstanceBatch
  stageVisibleCells(const RenderSliceRequest &req,
                    const gleditor::text::FontFacePtr &font,
                    gleditor::GlyphCache &glyphCache);

  /**
   * @brief Stage visible instances directly into a persistent mapped
   *        StreamBufferGL ring buffer.
   * @return The offset in bytes inside the ring buffer.
   */
  [[nodiscard]] std::size_t stageIntoStreamBuffer(
      const RenderSliceRequest &req, const gleditor::text::FontFacePtr &font,
      gleditor::GlyphCache &glyphCache, render::IStreamBuffer &streamBuffer);

  /// How many shaped pages to keep. A staging pass visits the cells inside
  /// the request radius, so this only needs to outlast a couple of frames'
  /// worth of neighbourhood to stop the churn. It is not a document cache.
  ///
  /// A shaped page of a short paragraph measures about 5 KB, nearly all of it
  /// the per-glyph vector, so this ceiling is roughly 2.5 MiB and scales with
  /// how much text a cell holds. Bounded rather than generous on purpose: an
  /// unbounded cache is not a cache.
  static constexpr std::size_t kShapingCacheCapacity = 512;

  struct ShapingCacheStats {
    std::size_t entries{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t evictions{};
  };
  [[nodiscard]] ShapingCacheStats shapingCacheStats() const noexcept;

  /// Drop every shaped page. The cache keys on the FontFace address, and an
  /// address can be reused after a font is released and another loaded, so
  /// anything swapping fonts under the engine has to say so.
  void clearShapingCache() noexcept;

private:
  /// Mint the two genesis cells if this store has none, so that a dimension has
  /// a d.dims rank to go on.
  void ensureSliceBegun();

  /**
   * @brief Shaped output for @p text, from cache when it is there.
   *
   * Shaping is what a staging pass actually spends its time on: 14.3
   * microseconds per cell measured, against roughly 1 microsecond for the
   * whole neighbourhood traversal. stageVisibleCells redid it for every
   * visited cell on every call.
   *
   * Keyed on the text rather than on the cell id, because a cell's text
   * changes and its id does not. The hash is for lookup and the text is kept
   * beside it so a hit is confirmed by comparison -- an unchecked collision
   * here would draw one cell's words in another's place, which is a worse
   * failure than being slow.
   */
  [[nodiscard]] const PageShaping &
  shapedPage(std::string_view text, const gleditor::text::FontFacePtr &font,
             const gleditor::text::LayoutOptions &opts);

  /// Everything layoutPage's output depends on. If a field is added to
  /// LayoutOptions that changes the result, it belongs here too -- otherwise
  /// the cache starts answering a question it was not asked.
  struct ShapingKey {
    std::string text;
    const gleditor::text::FontFace *font{nullptr};
    float maxWidthPx{};
    float maxHeightPx{};
    bool singleParagraph{};
    bool ellipsize{};
    std::vector<gleditor::DecoratedRange> decoratedRanges;
    std::vector<gleditor::LayoutBox> boxes;
    std::vector<gleditor::BlockStyleRange> blockStyles;
    gleditor::PageSize page;

    [[nodiscard]] bool operator==(const ShapingKey &) const = default;
  };
  struct ShapingKeyHash {
    [[nodiscard]] std::size_t operator()(const ShapingKey &k) const noexcept;
  };
  struct ShapingEntry {
    PageShaping shaping;
    std::uint64_t lastUsedTick{};
  };

  xanadu::Store &store_;
  std::uint32_t lastSyncedOpIndex_{0};
  xanadu::MicroversionId head_;

  /// The model. What used to be here instead: a CellID space of its own with
  /// `nextCellId_` handing out names, `cells_` holding a CompactZZCell each,
  /// `opIndexToCell_` mapping an operation to the cell it invented,
  /// `spanToMasterCell_` plus `longestMasterSpan_` finding a d.transclude
  /// master by span, and `transcludeRankTail_` remembering where a rank ended.
  /// All of it is gone: a CellRef *is* an operation index, so the mapping is
  /// the identity and needs no table, and the ranks are operations rather than
  /// something this class derives and then has to keep in step.
  ///
  /// The bounded Version cache went with it. It existed so that
  /// buildCellFromOp() could resolve a Transclude operation's source span
  /// without replaying an ancestral path per transclusion -- a real fix to a
  /// real quadratic (migration step 3), for work that is no longer done here.
  Manifold manifold_;

  /// Per-cell facts the manifold does not hold, keyed by CellRef.
  std::unordered_map<CellRef, ColdCell> cold_;

  std::unordered_map<ShapingKey, ShapingEntry, ShapingKeyHash> shapingCache_;
  std::uint64_t shapingTick_{0};
  std::uint64_t shapingHits_{0};
  std::uint64_t shapingMisses_{0};
  std::uint64_t shapingEvictions_{0};

  struct EphemeralMetaDimSlot {
    CellRef parentCell{noCell};
    DimRef dimension{noCell};
    std::size_t index{0};
    std::size_t totalCount{0};
  };

  CellRef getOrCreateEphemeralCell(CellRef parent, std::size_t index,
                                   DimRef dim, std::size_t total) const;

  mutable std::unordered_map<CellRef, EphemeralMetaDimSlot> ephemeralSlots_;
  mutable std::map<std::pair<CellRef, std::size_t>, CellRef>
      ephemeralByParentAndIndex_;
  /// Above the references metaDimension() reserves, so a derived cell can never
  /// be mistaken for the derived dimension.
  mutable std::uint32_t nextEphemeralId_{16};
  mutable CellSlot ephemeralCellSlotDummy_{};
};

} // namespace zigzag

#endif // ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP
