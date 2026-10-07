/**
 * @file quotation_builder_overlay.hpp
 * @brief Interactive Quotation Builder Overlay with navigable preview (§5.10).
 *
 * Provides a shared modal UI for xudu, xuzz, and zigzag to visually select
 * foreign stores, root cells, and dimension mappings, or enter custom VQL
 * queries, with a real-time navigable 2D preview before committing.
 */
#ifndef COMMON_UI_QUOTATION_BUILDER_OVERLAY_HPP
#define COMMON_UI_QUOTATION_BUILDER_OVERLAY_HPP

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/xanadu/quotation_builder.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm_catalog.hpp"

namespace xanadu {

/**
 * @class QuotationBuilderOverlay
 * @brief Interactive modal dialog for building and previewing structure
 * quotations across Xudu, Xuzz, and Zigzag.
 */
class QuotationBuilderOverlay : public gleditor::FrameContributor,
                                public gleditor::ModalInput,
                                public gleditor::PickObserver,
                                public gleditor::a11y::Source {
public:
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }

  using VersionCommitCallback = std::function<void(
      MicroversionId newVersion, zigzag::CellRef quotationCell)>;
  using OpenStoresProvider    = std::function<std::vector<Store *>()>;

  static constexpr std::uint32_t kTagClose       = 25001U;
  static constexpr std::uint32_t kTagModeRank    = 25002U;
  static constexpr std::uint32_t kTagModeClosure = 25003U;
  static constexpr std::uint32_t kTagModeQuery   = 25004U;
  static constexpr std::uint32_t kTagDirToggle   = 25005U;
  static constexpr std::uint32_t kTagCommit      = 25006U;
  static constexpr std::uint32_t kTagCancel      = 25007U;
  static constexpr std::uint32_t kTagInputQuery  = 25010U;
  static constexpr std::uint32_t kTagInputLabel  = 25011U;
  static constexpr std::uint32_t kTagInputDim    = 25012U;
  static constexpr std::uint32_t kTagStorePrev   = 25020U;
  static constexpr std::uint32_t kTagStoreNext   = 25021U;
  static constexpr std::uint32_t kTagRootPrev    = 25022U;
  static constexpr std::uint32_t kTagRootNext    = 25023U;

  static constexpr std::uint32_t kTagStoreBase   = 25100U;
  static constexpr std::uint32_t kTagRootBase    = 25200U;
  static constexpr std::uint32_t kTagDimBase     = 25300U;
  static constexpr std::uint32_t kTagPreviewBase = 25500U;

  QuotationBuilderOverlay(Store &localStore, MicroversionId activeVersion,
                          RendererRef renderer, SwarmCatalog *catalog = nullptr,
                          std::string fontName           = {},
                          OpenStoresProvider openStores  = nullptr,
                          VersionCommitCallback onCommit = nullptr);
  ~QuotationBuilderOverlay() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // ModalInput
  [[nodiscard]] bool grabbing() const override { return visible_.load(); }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods) override;
  void textTyped(const std::string &utf8) override;
  [[nodiscard]] std::optional<gleditor::InputArea> textArea() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // a11y::Source
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return a11yRevision_.load() + overlay_.accessibilityRevision();
  }

  bool performAction(std::uint64_t, gleditor::a11y::Action,
                     std::string_view) override;
  std::shared_ptr<const gleditor::ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override;
  bool activateNode(std::uint32_t) override;
  bool pointerEvent(const gleditor::ui::PointerEvent &) override;
  void focusChanged(bool) override;
  std::optional<gleditor::InputArea> pointerArea() const override;
  QuotationBuilderOverlay *setConfig(const ModalPresentationConfig &);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepare(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &);
  [[nodiscard]] gleditor::text::ShapingCache::Stats shapingStats() const {
    return overlay_.shapingStats();
  }
  QuotationBuilderOverlay *setVisible(bool visible);
  QuotationBuilderOverlay *toggle();
  [[nodiscard]] bool isVisible() const noexcept { return visible_.load(); }
  QuotationBuilderOverlay *setActiveVersion(MicroversionId version) noexcept {
    const std::scoped_lock lock(guard_);
    if (activeVersion_ != version) changed(true);
    activeVersion_ = version;
    return this;
  }
  [[nodiscard]] MicroversionId activeVersion() const noexcept {
    const std::scoped_lock lock(guard_);
    return activeVersion_;
  }

  // Builder accessor for testing and programmatic configuration
  [[nodiscard]] QuotationBuilder &builder() noexcept { return builder_; }
  [[nodiscard]] const QuotationBuilder &builder() const noexcept {
    return builder_;
  }

  QuotationBuilderOverlay *selectStore(std::size_t index);
  QuotationBuilderOverlay *selectRootCell(std::size_t index);
  QuotationBuilderOverlay *setMode(Selector::Kind mode);
  QuotationBuilderOverlay *toggleCarriedDimension(zigzag::DimRef dim);
  QuotationBuilderOverlay *setVqlQuery(std::string query);
  QuotationBuilderOverlay *refreshSources();
  bool commitQuotation();

private:
  Store &localStore_;
  MicroversionId activeVersion_;
  RendererRef renderer_;
  SwarmCatalog *catalog_{nullptr};
  std::string fontName_;
  OpenStoresProvider openStoresProvider_;
  VersionCommitCallback onCommit_;

  gleditor::ui::ScreenOverlay overlay_;
  mutable std::recursive_mutex guard_;
  std::atomic<bool> visible_{false};
  std::atomic<std::uint64_t> a11yRevision_{1};
  ModalPresentationConfig config_;
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme sourceTheme_, theme_;
  bool dirty_{true};
  std::size_t page_{};
  float scrollPx_{}, rowHeight_{}, listHeight_{};
  std::uint64_t epoch_{1}, semanticRevision_{1};
  gleditor::ui::WidgetId nextId_{1024};
  std::array<gleditor::ui::WidgetId, 4> tabIds_{};
  gleditor::ui::WidgetId closeId_{};
  std::array<gleditor::ui::WidgetId, 3> fieldIds_{};
  std::array<gleditor::ui::TextField, 3> fields_{};
  std::size_t observedLocalOps_{}, observedTargetOps_{};
  void observeStores();
  struct Action {
    std::string name, value;
    std::size_t index{};
    std::uint64_t epoch{}, semantic{};
    zigzag::CellRef ref{zigzag::noCell};
  };
  std::unordered_map<gleditor::ui::WidgetId, Action> actions_;
  std::unordered_map<std::string, gleditor::ui::WidgetId> contentIds_;
  gleditor::ui::WidgetId contentId(std::string_view,
                                   zigzag::CellRef = zigzag::noCell);
  std::vector<Action> listActions_, pending_;
  void changed(bool semantic = false);
  void queue(const gleditor::ui::WidgetAction &);
  void drain();
  void scroll(float);
  gleditor::ui::WidgetId allocate();

  QuotationBuilder builder_;

  // Stores and candidates
  std::vector<std::string> foreignStores_;
  std::size_t selectedStoreIndex_{0};
  Store *selectedTargetStore_{nullptr};

  std::vector<std::pair<zigzag::CellRef, std::string>> candidateRootCells_;
  std::size_t selectedRootCellIndex_{0};

  std::vector<std::pair<zigzag::DimRef, std::string>> availableForeignDims_;
  std::unordered_set<zigzag::DimRef> selectedCarriedDims_;

  std::size_t selectedRankDimIndex_{0};
  zigzag::DimVector rankDir_{zigzag::DimVector::POS};

  std::string vqlQueryText_{"##/d.vars"};
  std::string localDimName_{"d.vars"};
  std::string labelText_{"Quotation"};

  void recomputePreview();
};

} // namespace xanadu

#endif // COMMON_UI_QUOTATION_BUILDER_OVERLAY_HPP
