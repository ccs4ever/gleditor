/**
 * @file hypertime_graph.hpp
 * @brief Interactive 2D Hypertime Branching DAG and Unlimited N-Way Visual Diff Engine.
 *
 * Replaces the 1D hypertime map with a full 2D topological branching graph:
 * - Circular microversion nodes with centered single-letter op indicators
 *   ('I', 'D', 'R', 'T', 'L', 'P', 'G').
 * - Dynamic unobstructed placement of author aliases avoiding connecting branch lines.
 * - Multi-selection for arbitrary N-way primedia address comparison without limits.
 * - Continuous horizontal time scrubber along the bottom.
 * - Floating comparative diff summary and quick [Quote into Head] transclusion.
 */
#ifndef XUDU_HYPERTIME_GRAPH_HPP
#define XUDU_HYPERTIME_GRAPH_HPP

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

#include "core/microversion.hpp"
#include "core/ops.hpp"
#include "core/store.hpp"

struct RenderState;

namespace xudu {

class Session;

/**
 * @class HypertimeGraph
 * @brief Interactive 2D Hypertime DAG and visual diff visualizer.
 */
class HypertimeGraph : public gleditor::FrameContributor,
                       public gleditor::PickObserver,
                       public gleditor::a11y::Source {
public:
  static constexpr std::uint32_t kTagScrubberThumb = 900U;
  static constexpr std::uint32_t kTagScrubberTrack = 901U;
  static constexpr std::uint32_t kTagQuoteButton   = 910U;
  static constexpr std::uint32_t kTagOpen3DButton  = 911U;
  static constexpr std::uint32_t kTagClearComp     = 912U;
  static constexpr std::uint32_t kTagNodeBase      = 1000U;

  HypertimeGraph(std::string aFontName, const Session &aSession);
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
    return builtAt;
  }

  // -- Visibility & Navigation ------------------------------------------------
  void setVisible(bool show) noexcept { visible_ = show; }
  void toggle() noexcept { visible_ = !visible_; }
  [[nodiscard]] bool isVisible() const noexcept { return visible_; }

  void setCurrent(const MicroversionId &id) { current_ = id; }
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
  void setQuoteHandler(
      std::function<void(const MicroversionId &sourceVer, std::uint32_t at,
                         std::uint32_t len)>
          aQuoter) {
    quoteHandler_ = std::move(aQuoter);
  }
  void setCompareHandler(
      std::function<void(const std::vector<MicroversionId> &)> aComparer) {
    compareHandler_ = std::move(aComparer);
  }

  // -- Multi-Selection for Comparison -----------------------------------------
  void toggleComparison(const MicroversionId &id);
  void clearComparison();
  [[nodiscard]] const std::vector<MicroversionId> &
  comparedVersions() const noexcept {
    return comparedVersions_;
  }

  /// Single-letter code corresponding to an OpKind ('I', 'D', 'R', 'T', 'L', 'P', 'G').
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
  const Session &session_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  bool visible_{false};
  MicroversionId current_;
  std::vector<MicroversionId> comparedVersions_;

  std::vector<GraphNode> nodes_;
  std::vector<GraphEdge> edges_;
  std::vector<MicroversionId> chronologicalOrder_;

  float panelX_{16.0F};
  float panelY_{40.0F};
  float panelW_{580.0F};
  float panelH_{420.0F};

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
  std::function<void(const std::vector<MicroversionId> &)> compareHandler_;

  std::uint64_t builtAt{0};
  std::uint64_t revision_{1};
};

/// Compatibility alias for the interactive hypertime visualizer.
using HypertimeMap = HypertimeGraph;

} // namespace xudu

#endif // XUDU_HYPERTIME_GRAPH_HPP
