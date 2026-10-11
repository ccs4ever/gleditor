/**
 * @file apps/common/ui/xanadoc/branch_convergence_overlay.hpp
 * @brief Branch Convergence Overlay UI coordinating dual 3D document layout,
 *        comparative optical beams, and Convergence Action Palette.
 *
 * Vortex Rationale:
 * BranchConvergenceOverlay is implemented in C++ because it interfaces
 * directly with the gleditor 3D rendering pipeline, FrameContributor,
 * PickObserver, SpanDecorator, font shaping cache, and hardware device states
 * which require native performance and direct memory access to render
 * buffers. The higher-level reconciliation actions, workflows, and keybindings
 * dispatch through the Vortex runtime via system://keymap.
 */
#ifndef COMMON_UI_XANADOC_BRANCH_CONVERGENCE_OVERLAY_HPP
#define COMMON_UI_XANADOC_BRANCH_CONVERGENCE_OVERLAY_HPP

#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/span_decorator.hpp>
#include <gleditor/ui/overlay.hpp>
#include <gleditor/ui/widgets.hpp>

#include "common/xanadu/microversion.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"

namespace zigzag::vortex {
class VortexHost;
} // namespace zigzag::vortex

namespace xanadu {

class Session;
class Views;
class LinkBeams;

namespace ui {
class HypertimeGraph;
} // namespace ui

/**
 * @class BranchConvergenceOverlay
 * @brief Interactive visual overlay guiding authors through branch
 * reconciliation, side-by-side comparative inspection, and structured
 * synthesis.
 */
class BranchConvergenceOverlay : public gleditor::FrameContributor,
                                 public gleditor::PickObserver,
                                 public gleditor::SpanDecorator,
                                 public gleditor::a11y::Source {
public:
  // Optical beam and highlight colours
  static constexpr std::uint32_t kIdentityGoldColour    = 0xFFD700FFU;
  static constexpr std::uint32_t kIdentityGoldHighlight = 0xFFD70060U;
  static constexpr std::uint32_t kMintHighlightColour   = 0x3EB489C0U;

  // Widget Identifiers
  static constexpr gleditor::ui::WidgetId kIdBannerPanel      = 21001U;
  static constexpr gleditor::ui::WidgetId kIdBannerLabel      = 21002U;
  static constexpr gleditor::ui::WidgetId kIdBannerReviewBtn  = 21003U;
  static constexpr gleditor::ui::WidgetId kIdBannerDismissBtn = 21004U;

  static constexpr gleditor::ui::WidgetId kIdPalettePanel            = 21101U;
  static constexpr gleditor::ui::WidgetId kIdPaletteTitle            = 21102U;
  static constexpr gleditor::ui::WidgetId kIdPaletteAdoptBtn         = 21103U;
  static constexpr gleditor::ui::WidgetId kIdPaletteRetainBtn        = 21104U;
  static constexpr gleditor::ui::WidgetId kIdPaletteSynthBtn         = 21105U;
  static constexpr gleditor::ui::WidgetId kIdPaletteDismiss          = 21106U;
  static constexpr gleditor::ui::WidgetId kIdPaletteEncryptCheckbox  = 21107U;
  static constexpr gleditor::ui::WidgetId kIdPaletteExportPackageBtn = 21108U;

  BranchConvergenceOverlay();
  explicit BranchConvergenceOverlay(Store *store, std::string fontName = {});
  ~BranchConvergenceOverlay() override;

  // -- Setters (chaining `this`) ---------------------------------------------
  BranchConvergenceOverlay *setStore(Store *store) noexcept;
  BranchConvergenceOverlay *setViews(Views *views) noexcept;
  BranchConvergenceOverlay *
  setHypertimeGraph(ui::HypertimeGraph *graph) noexcept;
  BranchConvergenceOverlay *setLinkBeams(LinkBeams *beams) noexcept;
  BranchConvergenceOverlay *setSession(Session *session) noexcept;
  BranchConvergenceOverlay *setRenderer(RendererRef renderer) noexcept;

  BranchConvergenceOverlay *setDeviceId(std::string_view deviceId);
  BranchConvergenceOverlay *setActiveHead(const MicroversionId &activeHead);
  BranchConvergenceOverlay *setPeerHead(const MicroversionId &peerHead);

  BranchConvergenceOverlay *setBannerVisible(bool visible) noexcept;
  BranchConvergenceOverlay *setPaletteVisible(bool visible) noexcept;
  BranchConvergenceOverlay *setConvergenceActive(bool active) noexcept;

  BranchConvergenceOverlay *setEncryptPackage(bool enabled) noexcept;
  [[nodiscard]] bool isEncryptPackage() const noexcept {
    return encryptPackage_;
  }
  BranchConvergenceOverlay *
  setPackagePassphrase(std::string passphrase) noexcept;
  [[nodiscard]] const std::string &packagePassphrase() const noexcept {
    return packagePassphrase_;
  }

  // -- Detection & Notification Banner ---------------------------------------
  BranchConvergenceOverlay *
  detectConcurrentEdits(std::string_view deviceId,
                        const MicroversionId &branchVersion,
                        const MicroversionId &activeHead);

  [[nodiscard]] std::string bannerText() const;
  [[nodiscard]] std::string fullBannerNotification() const;

  // -- Query State -----------------------------------------------------------
  [[nodiscard]] bool isBannerVisible() const noexcept { return bannerVisible_; }
  [[nodiscard]] bool isPaletteVisible() const noexcept {
    return paletteVisible_;
  }
  [[nodiscard]] bool isConvergenceActive() const noexcept {
    return convergenceActive_;
  }
  [[nodiscard]] const std::string &deviceId() const noexcept {
    return deviceId_;
  }
  [[nodiscard]] const MicroversionId &activeHead() const noexcept {
    return activeHead_;
  }
  [[nodiscard]] const MicroversionId &peerHead() const noexcept {
    return peerHead_;
  }
  [[nodiscard]] const std::optional<MultiVersionDiffResult> &
  diffResult() const noexcept {
    return diffResult_;
  }

  // -- Structured Convergence Actions ----------------------------------------
  /**
   * @brief Activates comparative 3D layout, arranges branches side-by-side,
   *        and brings up the Convergence Action Palette.
   */
  std::expected<void, std::string> reviewAndConverge();

  /**
   * @brief Adopt Selected Span (Ctrl+Shift+A): transcludes highlighted passage
   *        from peer branch into active head via Store::transclude().
   */
  std::expected<MicroversionId, std::string>
  adoptSelectedSpan(std::optional<PrimediaSpan> span       = std::nullopt,
                    std::optional<std::uint32_t> targetPos = std::nullopt);

  /**
   * @brief Retain Both as Named Editions (Ctrl+Shift+E): attaches author
   *        aliases to both branch tips.
   */
  std::expected<void, std::string>
  retainBothAsNamedEditions(std::string_view activeAlias = {},
                            std::string_view peerAlias   = {});

  /**
   * @brief Synthesize New Edition (Ctrl+Shift+S): advances document to
   *        synthesized microversion (State 7).
   */
  std::expected<MicroversionId, std::string>
  synthesizeNewEdition(std::string_view editionName = {});

  /**
   * @brief Dispatches a named action string (e.g. from keymap or UI click).
   */
  std::expected<void, std::string> dispatchAction(std::string_view action);

  // -- Vortex Keymap Registration --------------------------------------------
  static void registerVortexActions(zigzag::vortex::VortexHost &vHost,
                                    Store *keymapStore = nullptr);
  BranchConvergenceOverlay *bindVortexHost(zigzag::vortex::VortexHost &vHost,
                                           Store *keymapStore = nullptr);

  // -- Layout & Presentation Inspection --------------------------------------
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepare(const gleditor::ui::UiMetrics &metrics,
          const gleditor::ui::Theme &theme);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  bannerScene() const;
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  paletteScene() const;

  // -- FrameContributor ------------------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- PickObserver ----------------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- SpanDecorator ---------------------------------------------------------
  void decorate(const Doc &doc, std::vector<gleditor::SpanStyle> &out) override;

  // -- a11y::Source ----------------------------------------------------------
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t id, gleditor::a11y::Action action,
                     std::string_view value) override;

private:
  void rebuildBannerModel(const gleditor::ui::UiMetrics &metrics);
  void rebuildPaletteModel(const gleditor::ui::UiMetrics &metrics);
  void ensureDiffUpToDate();

  Store *store_{nullptr};
  Views *views_{nullptr};
  ui::HypertimeGraph *hypertimeGraph_{nullptr};
  LinkBeams *beams_{nullptr};
  Session *session_{nullptr};
  RendererRef renderer_{};
  std::string fontName_;

  std::string deviceId_{"peer"};
  MicroversionId activeHead_;
  MicroversionId peerHead_;

  bool bannerVisible_{false};
  bool paletteVisible_{false};
  bool convergenceActive_{false};
  bool encryptPackage_{false};
  std::string packagePassphrase_{};

  std::optional<MultiVersionDiffResult> diffResult_;

  std::unique_ptr<gleditor::ui::ScreenOverlay> bannerOverlay_;
  std::unique_ptr<gleditor::ui::ScreenOverlay> paletteOverlay_;

  mutable std::mutex guard_;
  std::uint64_t revision_{1};
};

} // namespace xanadu

#endif // COMMON_UI_XANADOC_BRANCH_CONVERGENCE_OVERLAY_HPP
