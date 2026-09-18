/**
 * @file zigzag_visualizer.hpp
 * @brief Xanadu ZigZag multi-dimensional visualizer and navigator on gleditor.
 */
#ifndef ZIGZAG_VISUALIZER_HPP
#define ZIGZAG_VISUALIZER_HPP

#include "common/xanadu/link_layout.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vortex/vortex_host.hpp"
#include "common/xanadu/zigzag/presentation_surface.hpp"
#include "core/manifold.hpp"
#include "core/unified_transclusion_engine.hpp"
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
#include <gleditor/layout_box.hpp>
#include <gleditor/modal_input.hpp>
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
  std::vector<gleditor::DecoratedRange> decorated_ranges;
  std::vector<gleditor::BlockStyleRange> block_styles;
};

/// Measured presentation geometry shared by drawing, rank layout, and bridge
/// anchors. It is cached per visible cell and refreshed only when presentation
/// content or mode changes, never once per frame.
struct CellLayoutMetrics {
  float width{};
  float height{};
  float labelWidthLimit{};
  float titleTop{};
  float labelTop{};
  float badgeTop{};
  float labelLineHeight{};
  std::string idText;
  std::string badgeText;
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
                         public gleditor::a11y::Source,
                         public gleditor::ModalInput,
                         public xanadu::ZigzagPresentationSurface {
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

  // -- gleditor::ModalInput -------------------------------------------------
  [[nodiscard]] bool grabbing() const override;
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods) override;
  void textTyped(const std::string &utf8) override;
  [[nodiscard]] std::optional<gleditor::InputArea> textArea() const override;

  // -- ZigZag Actions -------------------------------------------------------
  void adoptDocument(ZzStructureDocument &&doc, std::string sourcePath,
                     const std::unordered_map<CellID, XuduProjectionProvenance>
                         *origins = nullptr);
  void populateFallbackStructure();

  void adoptXuduStore(const xanadu::Store &store,
                      const std::vector<xanadu::MicroversionId> &versions);
  void bindXuduStore(xanadu::Store &store,
                     const xanadu::MicroversionId &version);
  void adoptXuduDocs(const std::vector<XuduDocInput> &docs,
                     const std::vector<xanadu::Link> &links = {});
  [[nodiscard]] ZzRasterResult
  rasterize(const DimID &primaryDim   = "d.doc",
            const DimID &secondaryDim = "d.transclude") const;
  [[nodiscard]] xanadu::LinkPackage
  exportAsLinkPackage(const xanadu::MutableKeys &keys,
                      const std::string &salt = "zigzag_slice",
                      std::int64_t sequence   = 1) const;

  void navigateFocus(const DimID &dimension, DimVector dir = DimVector::POS);
  void navigateFocus(const DimID &dimension, bool positive) {
    navigateFocus(dimension, positive ? DimVector::POS : DimVector::NEG);
  }
  void navigateFocusTo(CellID id);

  void swapDimensions(int axis1, int axis2);
  void cycleDimensions(bool forward = true);

  /// Identity of the persistent store whose cells this view is drawing.
  [[nodiscard]] std::string documentId() const;
  [[nodiscard]] std::string documentVersion() const;

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

  // -- Dimension Bundles ---------------------------------------------------
  using DimensionBundle = zigzag::DimensionBundle;

  void setDimensionBundle(DimensionBundle bundle);
  void cycleDimensionBundle(bool forward = true);
  [[nodiscard]] DimensionBundle dimensionBundle() const noexcept {
    return dimension_bundle_;
  }
  static std::string dimensionBundleName(DimensionBundle bundle) {
    return zigzag::dimensionBundleName(bundle);
  }
  static ViewAxisBinding dimensionBundleAxes(DimensionBundle bundle) {
    return zigzag::dimensionBundleAxes(bundle);
  }

  // -- Vortex Runtime & UI Integration --------------------------------------
  void attachVortexHost(std::shared_ptr<vortex::VortexHost> host);
  [[nodiscard]] std::shared_ptr<vortex::VortexHost> vortexHost() noexcept;
  void ensureVortexHost();
  bool dispatchAction(std::string_view actionName);

  // -- Opcode & Library Palette HUD -----------------------------------------
  void togglePalette();
  void setPaletteVisible(bool visible);
  [[nodiscard]] bool isPaletteVisible() const noexcept {
    return paletteVisible_;
  }
  void paletteNext();
  void palettePrev();
  bool paletteCloneSelectedToFocus();
  bool paletteTranslateVQL(std::string_view query = {});
  void setPaletteFilter(std::string filter);
  void paletteInputText(std::string_view text);
  void paletteBackspace();
  [[nodiscard]] const std::string &paletteFilter() const noexcept {
    return paletteFilter_;
  }
  [[nodiscard]] std::size_t paletteSelectedIndex() const noexcept {
    return paletteSelectedIndex_;
  }
  [[nodiscard]] std::vector<std::string> paletteItems() const;

  // -- VQL Translation & Active Chain Attachment ----------------------------
  /**
   * @brief Translates a VQL query into Vortex opcodes (using VQLCompiler) and
   *        attaches the resulting opcode graph to the current focus cell along
   *        the designated dimension.
   *
   * @param vqlQuery The VQL query text (e.g. "/d.1/d.2" or "let $x := /d.1
   * return $x").
   * @param attachDim The dimension along which to link the entry opcode
   * (defaults to "d.spin").
   * @param spawnCursor Whether to spawn an execution cursor on the entry
   * opcode.
   * @return true if compilation, promotion, and attachment succeeded.
   */
  bool translateVQLAndAttachToFocus(std::string_view vqlQuery,
                                    std::string_view attachDim = "d.spin",
                                    bool spawnCursor           = false);

  /// Validates and compiles a VQL query string, returning compilation metadata
  /// and disassembly.
  [[nodiscard]] xanadu::vql::CompilationResult
  compileVQL(std::string_view vqlQuery) const;

  // -- VQL Command Omnibar --------------------------------------------------
  void toggleCommandBar();
  void setCommandBarVisible(bool visible);
  [[nodiscard]] bool isCommandBarVisible() const noexcept {
    return commandBarVisible_;
  }
  void commandBarInputChar(char ch);
  void commandBarInputText(std::string_view text);
  void commandBarBackspace();
  void commandBarClear();
  void setCommandBarText(std::string text);
  [[nodiscard]] const std::string &commandBarText() const noexcept {
    return commandBarText_;
  }
  [[nodiscard]] const std::string &commandBarFeedback() const noexcept {
    return commandBarFeedback_;
  }
  [[nodiscard]] bool commandBarFeedbackIsError() const noexcept {
    return commandBarFeedbackIsError_;
  }

  /// Evaluates the contents of the Command Omnibar according to parsed intent:
  /// - starts with '/' or '##' -> quick path navigation
  /// - starts with ':macro'    -> macro definition & persistence
  /// - script/weave expression -> execute and promote/navigate
  bool executeCommandBar();

  /// Quick path navigation relative to focus (e.g. "/d.1/d.2", "/-d.2[0]")
  bool navigateVQL(std::string_view pathExpr);

  /// One-time script execution
  vortex::VortexHost::ScriptResult executeVQLScript(std::string_view script);

  /// Define and persist a named macro into the sovereign keymap store
  bool defineMacro(std::string_view name, std::string_view vqlExpr,
                   std::string_view keyBinding = {});

  /// Apply one validated system-slice snapshot between frames. The store is
  /// never consulted while drawing.
  void setPresentationConfig(xanadu::ZigzagPresentationConfig config);
  [[nodiscard]] const xanadu::ZigzagPresentationConfig &
  presentationConfig() const noexcept {
    return presentation_config_;
  }

  // -- Dual-Continuum Harmonic Depth Tiering --------------------------------
  void setDepthTier(float baseDepthZ, float opacityMultiplier = 1.0F);
  [[nodiscard]] float depthTier() const noexcept { return depth_tier_; }
  [[nodiscard]] float depthTierOpacity() const noexcept {
    return depth_tier_opacity_;
  }

  /// Place this presentation beside its host document without changing the
  /// manifold's intrinsic neighbourhood coordinates.
  void setPresentationOrigin(glm::vec3 origin);
  /// Resolve the exact host-page transform once per frame. A null result keeps
  /// the surface hidden while its host page has not been built yet.
  using PresentationTransformResolver =
      std::function<std::optional<glm::mat4>()>;
  void
  setPresentationTransformResolver(PresentationTransformResolver resolver) {
    presentationTransformResolver_ = std::move(resolver);
  }
  using PresentationOriginResolver = std::function<std::optional<glm::vec3>()>;
  void setPresentationOriginResolver(PresentationOriginResolver resolver) {
    presentationOriginResolver_ = std::move(resolver);
  }
  [[nodiscard]] glm::vec3 presentationOrigin() const noexcept {
    return presentation_origin_;
  }

  // -- In-App Interactive Cell & Dimension Editing --------------------------
  CellID createCell(std::string text = "", std::string role = "text");
  bool insertConnectedCell(std::string text, const DimID &dimension,
                           DimVector dir = DimVector::POS);
  bool insertConnectedCell(std::string text, const DimID &dimension,
                           bool positive) {
    return insertConnectedCell(std::move(text), dimension,
                               positive ? DimVector::POS : DimVector::NEG);
  }
  bool linkFocusAlong(const DimID &dimension, CellID targetId,
                      DimVector dir = DimVector::POS);
  bool linkFocusAlong(const DimID &dimension, CellID targetId, bool positive) {
    return linkFocusAlong(dimension, targetId,
                          positive ? DimVector::POS : DimVector::NEG);
  }
  bool unlinkFocusAlong(const DimID &dimension, DimVector dir = DimVector::POS);
  bool unlinkFocusAlong(const DimID &dimension, bool positive) {
    return unlinkFocusAlong(dimension,
                            positive ? DimVector::POS : DimVector::NEG);
  }
  bool deleteFocusCell();
  void updateFocusCellText(std::string text);
  bool saveStore(const std::string &filePath = {}) const;
  bool saveStructureYaml(const std::string &filePath) const;

  [[nodiscard]] bool isProtected(CellRef id) const;

  /// The cell that is dimension @p name, or noCell -- **without minting one.**
  ///
  /// Not UnifiedTransclusionEngine::dimensionFor(), which mints a dimension it
  /// cannot find. Reading, navigating and drawing must never record an
  /// operation: "only a user-generated update persists; navigation never does"
  /// (design R8), and drawing a frame is less than navigation. Minting belongs
  /// to the verbs a person invokes -- inserting a cell, making a link.
  [[nodiscard]] DimRef dimensionRef(const DimID &name) const;
  [[nodiscard]] CellID focusCellId() const { return accursed_cell_focus_; }
  [[nodiscard]] std::optional<xanadu::CellAnchor>
  cellAnchor(CellRef cell) const override;

  // -- Embedded presentation surface ---------------------------------------
  [[nodiscard]] const Manifold &manifold() const noexcept override {
    return engine_->manifold();
  }
  [[nodiscard]] CellRef focusCell() const noexcept override {
    return static_cast<CellRef>(accursed_cell_focus_);
  }
  void focusCell(const CellRef cell) override {
    navigateFocusTo(static_cast<CellID>(cell));
  }
  void activateCell(CellRef cell) override;
  void setCellActivationCallback(
      xanadu::ZigzagPresentationSurface::CellActivationCallback callback)
      override {
    cellActivationCallback_ = std::move(callback);
  }
  [[nodiscard]] int cellRadius() const noexcept override {
    return scene_.neighborhood_radius;
  }
  void setCellRadius(int radius) noexcept override;

  [[nodiscard]] bool isCellLocked(CellRef cell) const noexcept override {
    return engine_ ? engine_->isCellLocked(cell) : false;
  }
  [[nodiscard]] std::optional<xanadu::TranscopyrightDescriptor>
  cellRoyalty(CellRef cell) const noexcept override {
    return engine_ ? engine_->cellRoyalty(cell) : std::nullopt;
  }
  bool unlockCell(CellRef cell) override;

  [[nodiscard]] std::uint64_t bridgeRevision() const noexcept override {
    return revision_;
  }
  void setBridgeInvalidationCallback(
      xanadu::ZigzagPresentationSurface::InvalidationCallback callback)
      override {
    bridgeInvalidationCallback_ = std::move(callback);
  }
  [[nodiscard]] gleditor::FrameContributor *
  frameContributor() noexcept override {
    return this;
  }
  [[nodiscard]] gleditor::PickObserver *pickObserver() noexcept override {
    return this;
  }
  [[nodiscard]] gleditor::a11y::Source *
  accessibilitySource() noexcept override {
    return this;
  }

  /// How many operations this slice has recorded. The document's size in
  /// hypertime, and what a test watches to catch an edit that records more
  /// operations than it changed anything with -- a spool is append-only, so a
  /// dead operation is permanent.
  [[nodiscard]] std::size_t operationCount() const;
  [[nodiscard]] const std::string &structureName() const {
    return structure_name_;
  }
  [[nodiscard]] const ViewAxisBinding &currentView() const {
    return current_view_;
  }
  [[nodiscard]] ZzStructureDocument document() const;

  [[nodiscard]] UnifiedTransclusionEngine *engine() const noexcept {
    return engine_.get();
  }
  [[nodiscard]] xanadu::Store *store() const noexcept { return store_; }
  [[nodiscard]] const std::unordered_map<CellID, RenderStateCell> &
  visibleCells() const noexcept {
    return visible_cells_;
  }

private:
  struct CellInfo {
    CellRef id{0};
    std::string text;
    std::string role;
    std::string mime_type;
    std::string media_path;
    bool is_image{false};
    bool is_clone{false};
    CellRef clone_master_id{0};
  };

  void rebuildActiveViewTopology();
  void updateCellPositions(float deltaTime);
  void invalidateAccessibility() {
    ++revision_;
    if (bridgeInvalidationCallback_) {
      bridgeInvalidationCallback_(revision_);
    }
  }

  [[nodiscard]] CellInfo inspectCell(CellRef id) const;
  [[nodiscard]] DimensionVisual dimensionVisual(const DimID &dimension) const;
  [[nodiscard]] CellLayoutMetrics measureCellLayout(const RenderStateCell &cell,
                                                    bool isFocus) const;
  [[nodiscard]] const CellLayoutMetrics &
  cellLayout(CellID id, const RenderStateCell &cell, bool isFocus) const;
  void refreshCellLayouts();

  std::string fontName_;
  std::uint64_t revision_{1};
  xanadu::ZigzagPresentationSurface::InvalidationCallback
      bridgeInvalidationCallback_;

  std::string structure_name_;
  std::string current_slice_path_;
  std::unique_ptr<xanadu::Store> ownedStore_;
  xanadu::Store *store_{nullptr};
  std::unique_ptr<UnifiedTransclusionEngine> engine_;
  CellID accursed_cell_focus_{0};
  ViewAxisBinding current_view_;

  SceneVisual scene_;
  ViewMode view_mode_{ViewMode::CellContent};
  glm::vec3 presentation_origin_{0.0F, 0.0F, 0.0F};
  glm::mat4 presentation_transform_{1.0F};
  PresentationTransformResolver presentationTransformResolver_;
  PresentationOriginResolver presentationOriginResolver_;
  float depth_tier_{0.0F};
  float depth_tier_opacity_{1.0F};
  xanadu::ZigzagPresentationConfig presentation_config_{};
  std::unordered_map<DimID, DimensionVisual> dimension_visuals_;

  std::unordered_map<CellID, RenderStateCell> visible_cells_;
  mutable std::unordered_map<CellID, CellLayoutMetrics> cell_layouts_;
  bool cell_layouts_dirty_{true};

  std::chrono::steady_clock::time_point last_frame_time_;

  std::unique_ptr<gleditor::Canvas> worldCanvas_;
  std::unique_ptr<gleditor::Canvas> hudCanvas_;
  std::unique_ptr<gleditor::Beams> beams_;
  std::unique_ptr<gleditor::ImageCache> imageCache_;
  std::unordered_map<CellRef, std::shared_ptr<const render::PickSemanticTarget>>
      pickTargets_;
  std::string pickTargetVersion_;
  std::unordered_map<CellRef, XuduProjectionProvenance> sourceOrigins_;

  DimensionBundle dimension_bundle_{DimensionBundle::Custom};
  std::shared_ptr<vortex::VortexHost> vortex_host_{nullptr};
  xanadu::ZigzagPresentationSurface::CellActivationCallback
      cellActivationCallback_;

  bool paletteVisible_{false};
  std::size_t paletteSelectedIndex_{0};
  std::string paletteFilter_;

  bool commandBarVisible_{false};
  std::string commandBarText_;
  std::string commandBarFeedback_;
  bool commandBarFeedbackIsError_{false};
};

} // namespace zigzag

#endif // ZIGZAG_VISUALIZER_HPP
