/**
 * @file wireframe_hull.hpp
 * @brief Progressive 3D streaming wireframe hull for documents materializing
 * from BitTorrent swarms.
 */
#ifndef XUDU_WIREFRAME_HULL_HPP
#define XUDU_WIREFRAME_HULL_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/xanadu/system_docs.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/world_panel.hpp>

#include "common/xanadu/transcopyright_logic.hpp"

namespace xanadu {
/**
 * @class WireframeHullOverlay
 * @brief FrameContributor that renders an ethereal pulsing wireframe hull and
 * streaming progress telemetry over documents arriving from the swarm.
 */
class WireframeHullOverlay : public gleditor::FrameContributor,
                             public gleditor::a11y::Source {
public:
  WireframeHullOverlay(RendererRef renderer, std::string fontName = {});
  ~WireframeHullOverlay() override;

  WireframeHullOverlay(const WireframeHullOverlay &)            = delete;
  WireframeHullOverlay &operator=(const WireframeHullOverlay &) = delete;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  /// Register or start tracking a document currently fetching pieces
  void startLoading(std::size_t docIndex, std::string title,
                    std::string infoHash, std::uint32_t totalPieces = 16);

  /// Update piece count for a loading document
  void updateProgress(std::size_t docIndex, std::uint32_t piecesFetched);

  /// Mark loading complete and initiate smooth dissolution
  void finishLoading(std::size_t docIndex);

  /// Whether a specific document is currently loading
  [[nodiscard]] bool isLoading(std::size_t docIndex) const noexcept;

  /// All currently tracked loading documents
  [[nodiscard]] const std::vector<WireframeProgress> &
  loadingDocs() const noexcept {
    return loadingDocs_;
  }

  void setTelemetry(std::string key, std::string title, std::string status,
                    std::optional<float> fraction);
  void clearTelemetry(std::string_view key);
  void setConfig(const WorldCardConfig &);
  void describe(gleditor::a11y::Builder &) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t, gleditor::a11y::Action,
                     std::string_view) override;
  [[nodiscard]] std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>
  snapshots() const;

private:
  struct Presentation;
  std::unique_ptr<Presentation> presentation_;
  struct DissolvingHull {
    std::size_t docIndex{0};
    float opacity{1.0F};
  };

  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;

  std::vector<WireframeProgress> loadingDocs_;
  std::vector<DissolvingHull> dissolvingHulls_;
};

} // namespace xanadu

#endif // XUDU_WIREFRAME_HULL_HPP
