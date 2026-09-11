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
#include "common/xanadu/zigzag/zzstructure.hpp"
#include "gleditor/doc.hpp"
#include "gleditor/glyphcache/cache.hpp"
#include "gleditor/render/stream_buffer.hpp"
#include "gleditor/text/font.hpp"
#include "gleditor/text/layout.hpp"

namespace zigzag {

/**
 * @struct SpanLess
 * @brief Strict weak ordering for PrimediaSpan keys in std::map.
 */
struct SpanLess {
  bool operator()(const xanadu::PrimediaSpan &a,
                  const xanadu::PrimediaSpan &b) const noexcept {
    if (a.scroll != b.scroll) {
      return a.scroll < b.scroll;
    }
    if (a.start != b.start) {
      return a.start < b.start;
    }
    return a.length < b.length;
  }
};

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

  // -- Topological Synchronization ------------------------------------------

  /**
   * @brief Incrementally ingest newly recorded operations from the store's ops
   *        spool into CompactZZCell nodes.
   */
  void syncIncremental();

  /**
   * @brief Link two cells symmetrically along a standard or custom dimension.
   */
  void linkCells(CellID a, CellID b, DimOrdinal dim);
  void linkCells(CellID a, CellID b, const DimID &dim);

  /**
   * @brief Unlink a cell's positive direction along a dimension.
   */
  void unlinkPositive(CellID a, DimOrdinal dim);

  /**
   * @brief Add a synthetic or standalone cell not directly generated from an
   * op.
   */
  CellID addCell(CompactZZCell cell);

  [[nodiscard]] const CompactZZCell *findCell(CellID id) const noexcept;
  [[nodiscard]] CompactZZCell *findCell(CellID id) noexcept;
  [[nodiscard]] std::size_t cellCount() const noexcept { return cells_.size(); }
  [[nodiscard]] CellID cellForOp(std::uint32_t opIndex) const noexcept;

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
  [[nodiscard]] std::string resolveCellText(CellID id) const;

  /**
   * @brief Resolve zero-copy string view if cell is backed by local primedia
   *        spool.
   */
  [[nodiscard]] std::string_view resolveLocalCellView(CellID id) const noexcept;

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
    CellID focusCellId{1};
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
  void buildCellFromOp(std::uint32_t opIndex,
                       const xanadu::CompactOpNode &node);

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
  CellID nextCellId_{1};

  std::unordered_map<CellID, CompactZZCell> cells_;
  std::unordered_map<std::uint32_t, CellID> opIndexToCell_;
  std::map<xanadu::PrimediaSpan, CellID, SpanLess> spanToMasterCell_;

  /// The longest master span recorded on each scroll. What makes the search
  /// for an overlapping master a bounded window rather than a scan: no entry
  /// starting more than this far before a span can reach into it, so the
  /// walk can begin at that bound instead of at the scroll's first entry.
  std::unordered_map<xanadu::ScrollId, std::uint64_t> longestMasterSpan_;

  /// Where a d.transclude rank currently ends, keyed by whichever cell the
  /// walk started from. A rank is only ever extended at its tail here, so a
  /// remembered tail is still on the rank and walking on from it reaches the
  /// same end as walking from the head -- it just skips everything already
  /// walked. Without it, joining the nth cell to a rank costs n steps.
  std::unordered_map<CellID, CellID> transcludeRankTail_;

  /// The versions rebuilt to resolve a Transclude op's source span, keyed by
  /// the source's op index. Every rebuild replays a whole ancestral path, so
  /// doing one per transclusion is quadratic in the size of the document --
  /// and consecutive transclusions usually name the same handful of sources.
  ///
  /// Least-recently-used, bounded: a Version holds a piece table for the
  /// whole document, so this trades a fixed amount of memory for the replay,
  /// and an unbounded cache would hold every intermediate state of the spool.
  static constexpr std::size_t versionCacheCapacity = 32;
  mutable std::list<std::pair<std::uint32_t, xanadu::Version>> versionCache_;
  mutable std::unordered_map<
      std::uint32_t,
      std::list<std::pair<std::uint32_t, xanadu::Version>>::iterator>
      versionCacheIndex_;

  /// The version @p sourceOpIndex produces, rebuilt only if it is not already
  /// held.
  [[nodiscard]] const xanadu::Version &
  versionForOp(std::uint32_t sourceOpIndex) const;

  std::unordered_map<ShapingKey, ShapingEntry, ShapingKeyHash> shapingCache_;
  std::uint64_t shapingTick_{0};
  std::uint64_t shapingHits_{0};
  std::uint64_t shapingMisses_{0};
  std::uint64_t shapingEvictions_{0};
};

} // namespace zigzag

#endif // ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP
