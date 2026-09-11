/**
 * @file zigzag_visualizer.hpp
 * @brief Xanadu ZigZag multi-dimensional visualizer and navigator on gleditor.
 */
#ifndef ZIGZAG_VISUALIZER_HPP
#define ZIGZAG_VISUALIZER_HPP

#include "core/zz_xudu_projector.hpp"
#include "core/zzcore.hpp"
#include "core/zzstructure.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/beams.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/image_cache.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

namespace zigzag {

struct RenderStateCell {
  CellID id{};
  std::string text;
  std::string type;
  std::string mime_type;
  std::string media_path;
  bool is_image{false};
  bool is_clone{false};
  CellID clone_master_id{0};

  glm::vec3 current_pos{0.0F, 0.0F, 0.0F};
  glm::vec3 target_pos{0.0F, 0.0F, 0.0F};

  float current_alpha{0.0F};
  float target_alpha{1.0F};

  glm::vec3 base_color{0.7F, 0.7F, 0.75F};
};

struct DimensionVisual {
  glm::vec3 color{0.7F, 0.7F, 0.75F};
  float spacing{2.0F};
  std::string label;
};

struct SceneVisual {
  glm::vec3 background{0.05F, 0.05F, 0.07F};
  glm::vec3 focus_color{0.956F, 0.773F, 0.259F};
  float focus_scale{1.4F};
  float cell_radius{0.35F};
  float layout_speed{12.0F};
  float alpha_speed{8.0F};
  float border_thickness{2.0F};
  int neighborhood_radius{3};
};

class ZigzagVisualizer : public gleditor::FrameContributor,
                         public gleditor::PickObserver,
                         public gleditor::a11y::Source {
public:
  explicit ZigzagVisualizer(std::string aFontName);
  ~ZigzagVisualizer() override;

  ZigzagVisualizer(const ZigzagVisualizer &)            = delete;
  ZigzagVisualizer &operator=(const ZigzagVisualizer &) = delete;
  ZigzagVisualizer(ZigzagVisualizer &&)                 = delete;
  ZigzagVisualizer &operator=(ZigzagVisualizer &&)      = delete;

  // -- gleditor::FrameContributor -------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- gleditor::PickObserver -----------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- gleditor::a11y::Source -----------------------------------------------
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision_;
  }
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

  // -- ZigZag Actions -------------------------------------------------------
  void adoptDocument(ZzStructureDocument &&doc, std::string sourcePath);
  void populateFallbackStructure();

  void adoptXuduStore(const xanadu::Store &store,
                      const std::vector<xanadu::MicroversionId> &versions);
  void adoptXuduDocs(const std::vector<XuduDocInput> &docs,
                     const std::vector<xanadu::Link> &links = {});
  [[nodiscard]] ZzRasterResult
  rasterize(const DimID &primaryDim   = "d.doc",
            const DimID &secondaryDim = "d.transclude") const;
  [[nodiscard]] xanadu::LinkPackage
  exportAsLinkPackage(const xanadu::MutableKeys &keys,
                      const std::string &salt = "zigzag_slice",
                      std::int64_t sequence   = 1) const;

  void navigateFocus(const DimID &dimension, bool positive);
  void navigateFocusTo(CellID id);

  void swapDimensions(int axis1, int axis2);
  void cycleDimensions(bool forward = true);

  // -- Multi-View Modes (Cell Content View vs. Topology View) ---------------
  enum class ViewMode : std::uint8_t {
    CellContent =
        0, ///< Dynamic sizing, full content, close XYZ spring alignment
    Topology =
        1, ///< Partial/abbreviated content, fixed-size cells on rigid lattice
  };

  void setViewMode(ViewMode mode);
  [[nodiscard]] ViewMode viewMode() const { return view_mode_; }
  void toggleViewMode();

  // -- In-App Interactive Cell & Dimension Editing --------------------------
  CellID createCell(std::string text = "", std::string role = "text");
  bool insertConnectedCell(std::string text, const DimID &dimension,
                           bool positive = true);
  bool linkFocusAlong(const DimID &dimension, CellID targetId,
                      bool positive = true);
  bool unlinkFocusAlong(const DimID &dimension, bool positive = true);
  void updateFocusCellText(std::string text);
  bool saveStructureYaml(const std::string &filePath) const;

  [[nodiscard]] CellID focusCellId() const { return accursed_cell_focus_; }
  [[nodiscard]] const std::string &structureName() const {
    return structure_name_;
  }
  [[nodiscard]] const ViewAxisBinding &currentView() const {
    return current_view_;
  }
  [[nodiscard]] ZzStructureDocument document() const;

private:
  void rebuildActiveViewTopology();
  void updateCellPositions(float deltaTime);
  void invalidateAccessibility() { revision_++; }

  [[nodiscard]] const Cell *findCell(CellID id) const;
  [[nodiscard]] static LinkPairs linksOn(const Cell *cell,
                                         const DimID &dimension);
  [[nodiscard]] DimensionVisual dimensionVisual(const DimID &dimension) const;

  std::string fontName_;
  std::uint64_t revision_{1};

  std::string structure_name_;
  std::string current_slice_path_;
  std::unordered_map<CellID, Cell> space_;
  CellID accursed_cell_focus_{0};
  ViewAxisBinding current_view_;

  SceneVisual scene_;
  ViewMode view_mode_{ViewMode::CellContent};
  std::unordered_map<DimID, DimensionVisual> dimension_visuals_;

  std::unordered_map<CellID, RenderStateCell> visible_cells_;

  std::chrono::steady_clock::time_point last_frame_time_;

  std::unique_ptr<gleditor::Canvas> worldCanvas_;
  std::unique_ptr<gleditor::Canvas> hudCanvas_;
  std::unique_ptr<gleditor::Beams> beams_;
  std::unique_ptr<gleditor::ImageCache> imageCache_;
};

} // namespace zigzag

#endif // ZIGZAG_VISUALIZER_HPP
