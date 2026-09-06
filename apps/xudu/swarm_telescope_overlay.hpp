/**
 * @file swarm_telescope_overlay.hpp
 * @brief 3D Swarm Telescope discovery overlay with topic swarms and FTS5
 * search.
 */
#ifndef XUDU_SWARM_TELESCOPE_OVERLAY_HPP
#define XUDU_SWARM_TELESCOPE_OVERLAY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "core/swarm_catalog.hpp"

namespace xudu {

/**
 * @class SwarmTelescopeOverlay
 * @brief 3-column discovery deck and search engine for swarm publications.
 */
class SwarmTelescopeOverlay : public gleditor::FrameContributor,
                              public gleditor::PickObserver {
public:
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
                                 std::string fontName = "Sans 10");
  ~SwarmTelescopeOverlay() override;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void setVisible(bool visible);
  void toggle();
  [[nodiscard]] bool isVisible() const noexcept;

  void setSearchQuery(std::string_view query);
  [[nodiscard]] std::string searchQuery() const;

  void selectCategory(CatalogCategory cat);
  void selectItem(std::size_t index);

  void setOnSummon(SummonHandler handler) { onSummon_ = std::move(handler); }

  void setSampleForceVisible(bool force) {
    sampleForceVisible_ = force;
    if (force) {
      visible_ = true;
    }
  }

private:
  void refreshSearch();

  SwarmCatalog &catalog_;
  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  SummonHandler onSummon_;

  bool visible_{false};
  bool sampleForceVisible_{false};
  CatalogCategory activeCategory_{CatalogCategory::TopicSwarms};
  std::string searchQuery_{"#hypertext author:nelson"};
  std::vector<SearchResult> currentResults_;
  std::size_t selectedResultIndex_{0};

  float width_{880.0F};
  float height_{560.0F};
};

} // namespace xudu

#endif // XUDU_SWARM_TELESCOPE_OVERLAY_HPP
