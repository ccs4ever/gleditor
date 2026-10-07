/**
 * @file swarm_telescope_overlay.hpp
 * @brief 3D Swarm Telescope discovery overlay with topic swarms and FTS5
 * search.
 */
#ifndef XUDU_SWARM_TELESCOPE_OVERLAY_HPP
#define XUDU_SWARM_TELESCOPE_OVERLAY_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/modal_input.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/overlay.hpp>

#include "common/xanadu/swarm_catalog.hpp"
#include "common/xanadu/system_docs.hpp"

namespace xudu {
using namespace ::xanadu;

/**
 * @class SwarmTelescopeOverlay
 * @brief 3-column discovery deck and search engine for swarm publications.
 */
class SwarmTelescopeOverlay : public gleditor::FrameContributor,
                              public gleditor::PickObserver,
                              public gleditor::ModalInput,
                              public gleditor::a11y::Source {
public:
  bool pointerPick(const render::PickingResult &pick,
                   RenderState &state) override {
    return picked(pick, state);
  }

  static constexpr std::uint32_t kTagTelescopeClose  = 14001U;
  static constexpr std::uint32_t kTagTabRecent       = 14002U;
  static constexpr std::uint32_t kTagTabAuthors      = 14003U;
  static constexpr std::uint32_t kTagTabTopics       = 14004U;
  static constexpr std::uint32_t kTagSummonButton    = 14005U;
  static constexpr std::uint32_t kTagSearchBar       = 14006U;
  static constexpr std::uint32_t kTagCategoryBase    = 14100U;
  static constexpr std::uint32_t kTagPublicationBase = 14300U;

  using SummonHandler = std::function<void(const PublicationEntry &)>;

  explicit SwarmTelescopeOverlay(SwarmCatalog &catalog, RendererRef renderer,
                                 std::string fontName = {});
  ~SwarmTelescopeOverlay() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  [[nodiscard]] bool grabbing() const override;
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods) override;
  void textTyped(const std::string &utf8) override;
  [[nodiscard]] std::optional<gleditor::InputArea> textArea() const override;
  void describe(gleditor::a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t nodeId, gleditor::a11y::Action action,
                     std::string_view value) override;

  void setVisible(bool visible);
  void toggle();
  [[nodiscard]] bool isVisible() const noexcept;

  void setSearchQuery(std::string_view query);
  [[nodiscard]] std::string searchQuery() const;

  void selectCategory(CatalogCategory cat);
  void selectItem(std::size_t index);

  void setOnSummon(SummonHandler handler);
  void setSampleForceVisible(bool force);

  void setConfig(const ModalPresentationConfig &);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  prepare(const gleditor::ui::UiMetrics &, const gleditor::ui::Theme &);
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  preparePresentation(const gleditor::ui::UiMetrics &metrics,
                      const gleditor::ui::Theme &theme) {
    return prepare(metrics, theme);
  }
  [[nodiscard]] std::shared_ptr<const gleditor::ui::WidgetScene>
  snapshot() const {
    return overlay_.snapshot();
  }
  [[nodiscard]] gleditor::text::ShapingCache::Stats shapingStats() const {
    return overlay_.shapingStats();
  }
  std::shared_ptr<const gleditor::ui::LayoutResult>
  focusLayout() const override;
  void focusedNodeChanged(std::uint32_t) override;
  bool activateNode(std::uint32_t) override;
  bool pointerEvent(const gleditor::ui::PointerEvent &) override;
  void focusChanged(bool) override;
  std::optional<gleditor::InputArea> pointerArea() const override;

private:
  void refreshSearch();
  void changed();
  void queue(const gleditor::ui::WidgetAction &);
  void drain();
  void scroll(float, std::uint32_t);

  SwarmCatalog &catalog_;
  RendererRef renderer_;
  std::string fontName_;
  gleditor::ui::ScreenOverlay overlay_;
  SummonHandler onSummon_;
  mutable std::recursive_mutex guard_;
  std::uint64_t revision_{1}, generation_{1};
  bool visible_{false}, sampleForceVisible_{false}, dirty_{true};
  CatalogCategory activeCategory_{CatalogCategory::TopicSwarms};
  std::string searchQuery_;
  std::size_t searchCaret_{};
  std::vector<SearchResult> currentResults_;
  std::vector<std::string> channels_;
  std::size_t selectedResultIndex_{0}, page_{};
  ModalPresentationConfig config_{860, 560, .95F, .95F};
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme sourceTheme_, theme_;
  gleditor::text::ShapingCache measurements_;
  gleditor::ui::WidgetId nextId_{1024}, searchId_{}, closeId_{}, refreshId_{},
      summonId_{}, categoryId_{}, pageId_{};
  std::array<float, 3> scrollPx_{}, viewportHeight_{}, listRowHeight_{};
  float rowHeight_{};
  std::uint32_t focusedId_{};
  std::array<gleditor::ui::WidgetId, 3> categoryTabIds_{}, pageTabIds_{};
  std::unordered_map<std::string, gleditor::ui::WidgetId> rowIdentities_;
  struct Pending {
    std::string action, value;
    std::size_t item{};
    std::uint64_t generation{};
    std::optional<PublicationEntry> entry;
  };
  std::vector<Pending> pending_;
};

} // namespace xudu

#endif // XUDU_SWARM_TELESCOPE_OVERLAY_HPP
