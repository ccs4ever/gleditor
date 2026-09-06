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

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>

#include "core/transcopyright_logic.hpp"

namespace xudu {

/**
 * @class WireframeHullOverlay
 * @brief FrameContributor that renders an ethereal pulsing wireframe hull and
 * streaming progress telemetry over documents arriving from the swarm.
 */
class WireframeHullOverlay : public gleditor::FrameContributor {
public:
  WireframeHullOverlay(RendererRef renderer, std::string fontName = "Sans 10");
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
  [[nodiscard]] const std::vector<WireframeProgress> &loadingDocs() const noexcept {
    return loadingDocs_;
  }

private:
  struct DissolvingHull {
    std::size_t docIndex{0};
    float opacity{1.0F};
  };

  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;

  std::vector<WireframeProgress> loadingDocs_;
  std::vector<DissolvingHull> dissolvingHulls_;
  float shimmerPhase_{0.0F};
};

} // namespace xudu

#endif // XUDU_WIREFRAME_HULL_HPP
