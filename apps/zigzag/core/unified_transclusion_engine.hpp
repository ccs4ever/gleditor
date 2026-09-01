/**
 * @file unified_transclusion_engine.hpp
 * @brief High-performance coordinator bridging Xudu storage, Zigzag topology,
 *        and Gleditor GPU text streaming.
 */
#ifndef ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP
#define ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "compact_zzcell.hpp"
#include "gleditor/doc.hpp"
#include "gleditor/glyphcache/cache.hpp"
#include "gleditor/render/gl/stream_buffer.hpp"
#include "gleditor/text/font.hpp"
#include "gleditor/text/layout.hpp"
#include "xudu/core/compact_op.hpp"
#include "xudu/core/microversion.hpp"
#include "xudu/core/store.hpp"
#include "zigzag/core/zzstructure.hpp"

namespace zigzag {

/**
 * @struct SpanLess
 * @brief Strict weak ordering for PrimediaSpan keys in std::map.
 */
struct SpanLess {
  bool operator()(const xudu::PrimediaSpan &a,
                  const xudu::PrimediaSpan &b) const noexcept {
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
  explicit UnifiedTransclusionEngine(xudu::Store &store);
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

  /**
   * @brief Convert current engine cell space to a standard ZzStructureDocument.
   */
  [[nodiscard]] ZzStructureDocument
  toZzStructureDocument(CellID focus = 0) const;

  /**
   * @brief Ingest an existing ZzStructureDocument into CompactZZCell space.
   */
  void loadFromZzStructureDocument(const ZzStructureDocument &doc);

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
  [[nodiscard]] std::size_t
  stageIntoStreamBuffer(const RenderSliceRequest &req,
                        const gleditor::text::FontFacePtr &font,
                        gleditor::GlyphCache &glyphCache,
                        render::gl::StreamBufferGL &streamBuffer);

private:
  void buildCellFromOp(std::uint32_t opIndex, const xudu::CompactOpNode &node);

  xudu::Store &store_;
  std::uint32_t lastSyncedOpIndex_{0};
  CellID nextCellId_{1};

  std::unordered_map<CellID, CompactZZCell> cells_;
  std::unordered_map<std::uint32_t, CellID> opIndexToCell_;
  std::map<xudu::PrimediaSpan, CellID, SpanLess> spanToMasterCell_;
};

} // namespace zigzag

#endif // ZIGZAG_UNIFIED_TRANSCLUSION_ENGINE_HPP
