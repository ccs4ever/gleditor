/**
 * @file apps/xudu/kinetic_tether_overlay.hpp
 * @brief Visual overlay for Hookean spring tether and floating blueprint quad.
 */
#ifndef XUDU_KINETIC_TETHER_OVERLAY_HPP
#define XUDU_KINETIC_TETHER_OVERLAY_HPP

#include <cstdint>
#include <memory>
#include <string>

#include <glm/vec2.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>

#include "core/kinetic_tether.hpp"

namespace xudu {

/**
 * @class KineticTetherOverlay
 * @brief Renders the luminous Hookean spring curve, floating blueprint quad,
 *        and corner bracket accents.
 */
class KineticTetherOverlay : public gleditor::FrameContributor {
public:
  explicit KineticTetherOverlay(KineticTetherEngine &engine,
                                std::string fontName = "Sans 10");
  ~KineticTetherOverlay() override;

  // FrameContributor interface
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

private:
  void drawTether(gleditor::Canvas &canvas, RenderState &state,
                  const glm::vec2 &p0, const glm::vec2 &p1, bool detached);
  void drawBlueprintQuad(gleditor::Canvas &canvas, RenderState &state,
                         const glm::vec2 &pos, bool detached);

  KineticTetherEngine &engine_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
};

} // namespace xudu

#endif // XUDU_KINETIC_TETHER_OVERLAY_HPP
