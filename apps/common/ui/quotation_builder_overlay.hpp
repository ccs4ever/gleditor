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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

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
                          std::string fontName           = "Sans 10",
                          OpenStoresProvider openStores  = nullptr,
                          VersionCommitCallback onCommit = nullptr);
  ~QuotationBuilderOverlay() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // ModalInput
  [[nodiscard]] bool grabbing() const override { return visible_; }
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods) override;
  void textTyped(const std::string &utf8) override;
  [[nodiscard]] std::optional<gleditor::InputArea> textArea() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // a11y::Source
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return a11yRevision_;
  }

  void setVisible(bool visible);
  void toggle();
  [[nodiscard]] bool isVisible() const noexcept { return visible_; }
  void setActiveVersion(MicroversionId version) noexcept {
    activeVersion_ = version;
  }
  [[nodiscard]] MicroversionId activeVersion() const noexcept {
    return activeVersion_;
  }

  // Builder accessor for testing and programmatic configuration
  [[nodiscard]] QuotationBuilder &builder() noexcept { return builder_; }
  [[nodiscard]] const QuotationBuilder &builder() const noexcept {
    return builder_;
  }

  void selectStore(std::size_t index);
  void selectRootCell(std::size_t index);
  void setMode(Selector::Kind mode);
  void toggleCarriedDimension(zigzag::DimRef dim);
  void setVqlQuery(std::string query);
  void refreshSources();
  bool commitQuotation();

private:
  Store &localStore_;
  MicroversionId activeVersion_;
  RendererRef renderer_;
  SwarmCatalog *catalog_{nullptr};
  std::string fontName_;
  OpenStoresProvider openStoresProvider_;
  VersionCommitCallback onCommit_;

  std::unique_ptr<gleditor::Canvas> canvas_;

  bool visible_{false};
  std::uint64_t a11yRevision_{1};

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

  enum class ActiveInput : std::uint8_t { None, Query, Label, LocalDim };
  ActiveInput activeInput_{ActiveInput::None};

  void recomputePreview();
};

} // namespace xanadu

#endif // COMMON_UI_QUOTATION_BUILDER_OVERLAY_HPP
