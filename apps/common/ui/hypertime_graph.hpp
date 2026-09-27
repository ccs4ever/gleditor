/**
 * @file hypertime_graph.hpp
 * @brief Interactive 2D Hypertime Branching DAG and Unlimited N-Way Visual Diff
 * Engine.
 *
 * Replaces the 1D hypertime map with a full 2D topological branching graph:
 * - Circular microversion nodes with centered single-letter op indicators
 *   ('I', 'D', 'R', 'T', 'L', 'P', 'G').
 * - Dynamic unobstructed placement of author aliases avoiding connecting branch
 * lines.
 * - Multi-selection for arbitrary N-way primedia address comparison without
 * limits.
 * - Continuous horizontal time scrubber along the bottom.
 * - Floating comparative diff summary and quick [Quote into Head] transclusion.
 */
#ifndef XANADU_UI_HYPERTIME_GRAPH_HPP
#define XANADU_UI_HYPERTIME_GRAPH_HPP

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/store.hpp"

struct RenderState;

namespace xanadu::ui {

/**
 * @class HypertimeGraph
 * @brief Interactive 2D Hypertime DAG and visual diff visualizer.
 */
class HypertimeGraph : public gleditor::FrameContributor,
                       public gleditor::PickObserver,
                       public gleditor::a11y::Source {
public:
  static constexpr std::uint32_t kTagScrubberThumb   = 900U;
  static constexpr std::uint32_t kTagScrubberTrack   = 901U;
  static constexpr std::uint32_t kTagQuoteButton     = 910U;
  static constexpr std::uint32_t kTagOpen3DButton    = 911U;
  static constexpr std::uint32_t kTagClearComp       = 912U;
  static constexpr std::uint32_t kTagOnionSkinButton = 913U;
  static constexpr std::uint32_t kTagAnnotateButton  = 914U;
  static constexpr std::uint32_t kTagNodeBase        = 1000U;

  HypertimeGraph(std::string aFontName,
                 std::function<const Store &(std::size_t)> storeAt,
                 std::function<std::uint64_t()> generation);
  ~HypertimeGraph() override;

  HypertimeGraph(const HypertimeGraph &)            = delete;
  HypertimeGraph &operator=(const HypertimeGraph &) = delete;
  HypertimeGraph(HypertimeGraph &&)                 = delete;
  HypertimeGraph &operator=(HypertimeGraph &&)      = delete;

  // -- FrameContributor -------------------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override { return false; }

  // -- PickObserver -----------------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- a11y::Source -----------------------------------------------------------
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision_;
  }

  // -- Visibility & Navigation ------------------------------------------------
  void setVisible(bool show) noexcept {
    if (visible_ == show) return;
    visible_ = show;
    revision_++;
  }
  void toggle() noexcept {
    visible_ = !visible_;
    revision_++;
  }
  [[nodiscard]] bool isVisible() const noexcept { return visible_; }
  void scroll(float horizontal, float vertical, bool zoom, bool shift,
              float pointerX, float pointerY);

  void setCurrent(const MicroversionId &id) {
    if (current_ == id) return;
    current_ = id;
    revision_++;
  }
  void invalidate() {
    nodes_.clear();
    revision_++;
  }
  void setStoreIndex(std::size_t index) {
    if (storeIndex_ == index) return;
    storeIndex_ = index;
    nodes_.clear();
    comparedVersions_.clear();
    selectedOperation_.reset();
    diffNeedsUpdate_ = true;
    revision_++;
  }
  [[nodiscard]] std::size_t storeIndex() const noexcept { return storeIndex_; }
  [[nodiscard]] const std::optional<MicroversionId> &
  selectedOperation() const noexcept {
    return selectedOperation_;
  }
  [[nodiscard]] const MicroversionId &current() const noexcept {
    return current_;
  }

  // -- Callbacks --------------------------------------------------------------
  void setGoer(std::function<void(const MicroversionId &)> aGoer) {
    goer_ = std::move(aGoer);
  }
  void setScrubHandler(std::function<void(const MicroversionId &)> aScrubber) {
    scrubHandler_ = std::move(aScrubber);
  }
  void setQuoteHandler(std::function<void(const MicroversionId &sourceVer,
                                          std::uint32_t at, std::uint32_t len)>
                           aQuoter) {
    quoteHandler_ = std::move(aQuoter);
  }
  void setAnnotateHandler(std::function<void(const MicroversionId &)> handler) {
    annotateHandler_ = std::move(handler);
  }
  void setCompareHandler(
      std::function<void(const std::vector<MicroversionId> &)> aComparer) {
    compareHandler_ = std::move(aComparer);
  }
  void setOnionSkinHandler(
      std::function<void(const std::vector<MicroversionId> &)> aOnionHandler) {
    onionSkinHandler_ = std::move(aOnionHandler);
  }

  // -- Multi-Selection for Comparison -----------------------------------------
  void toggleComparison(const MicroversionId &id);
  void clearComparison();
  [[nodiscard]] const std::vector<MicroversionId> &
  comparedVersions() const noexcept {
    return comparedVersions_;
  }

  /// Single-letter code corresponding to an OpKind ('I', 'D', 'R', 'T', 'L',
  /// 'P', 'G').
  [[nodiscard]] static char opKindLetter(std::optional<OpKind> kind) noexcept;

  // -- Geometry Helper --------------------------------------------------------
  static void drawDisc(gleditor::Canvas &canvas, float cX, float cY,
                       float radius, std::uint32_t fillCol,
                       std::uint32_t borderCol = 0, float borderWidth = 0.0F,
                       std::size_t slices = 32);

  static void drawPolyline(gleditor::Canvas &canvas, float fromX, float fromY,
                           float toX, float toY, float thickness,
                           std::uint32_t colour);

private:
  struct GraphNode {
    MicroversionId id;
    float x{0.0F};
    float y{0.0F};
    float radius{14.0F};
    std::size_t depth{0};
    std::size_t lane{0};
    char opLetter{'G'};
    std::string alias;
    float aliasX{0.0F};
    float aliasY{0.0F};
    float aliasW{0.0F};
    float aliasH{0.0F};
  };

  struct GraphEdge {
    std::size_t fromIdx{0};
    std::size_t toIdx{0};
    bool isBranch{false};
    std::size_t lane{0};
  };

  void layout(RenderState &state, float screenW, float screenH);
  void computeUnobstructedAliasPosition(GraphNode &node,
                                        const std::vector<GraphEdge> &allEdges,
                                        float panelTop, float panelBottom);

  std::string fontName_;
  std::function<const Store &(std::size_t)> storeAt_;
  std::function<std::uint64_t()> generation_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  std::size_t storeIndex_{0};
  bool visible_{false};
  MicroversionId current_;
  std::optional<MicroversionId> selectedOperation_;
  std::vector<MicroversionId> comparedVersions_;

  std::vector<GraphNode> nodes_;
  std::vector<GraphEdge> edges_;
  std::vector<MicroversionId> chronologicalOrder_;

  float panelX_{16.0F};
  float panelY_{40.0F};
  float panelW_{580.0F};
  float panelH_{420.0F};
  float panX_{0.0F};
  float panY_{0.0F};
  float zoom_{1.0F};
  float laidOutWidth_{0.0F};
  float laidOutHeight_{0.0F};

  float scrubberTrackX_{0.0F};
  float scrubberTrackY_{0.0F};
  float scrubberTrackW_{0.0F};
  float scrubberThumbX_{0.0F};

  MultiVersionDiffResult diffResult_;
  bool diffNeedsUpdate_{false};

  std::function<void(const MicroversionId &)> goer_;
  std::function<void(const MicroversionId &)> scrubHandler_;
  std::function<void(const MicroversionId &, std::uint32_t, std::uint32_t)>
      quoteHandler_;
  std::function<void(const MicroversionId &)> annotateHandler_;
  std::function<void(const std::vector<MicroversionId> &)> compareHandler_;
  std::function<void(const std::vector<MicroversionId> &)> onionSkinHandler_;

  std::uint64_t builtAt{0};
  std::uint64_t revision_{1};
};

} // namespace xanadu::ui

#endif // XANADU_UI_HYPERTIME_GRAPH_HPP
