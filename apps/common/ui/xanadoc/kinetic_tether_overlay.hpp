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

#include "common/xanadu/system_docs.hpp"
#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/curve_ribbons.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/ui/world_panel.hpp>

#include "common/xanadu/kinetic_tether.hpp"

namespace xanadu {
/**
 * @class KineticTetherOverlay
 * @brief Renders the luminous Hookean spring curve, floating blueprint quad,
 *        and corner bracket accents.
 */
class KineticTetherOverlay : public gleditor::FrameContributor,
                             public gleditor::a11y::Source {
public:
  explicit KineticTetherOverlay(KineticTetherEngine &engine,
                                std::string fontName = {});
  ~KineticTetherOverlay() override;

  // FrameContributor interface
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  void setConfig(const WorldCardConfig &);
  void setTetherConfig(const QuotationTetherConfig &);
  void describe(gleditor::a11y::Builder &) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t, gleditor::a11y::Action,
                     std::string_view) override;
  [[nodiscard]] std::vector<std::shared_ptr<const gleditor::ui::WidgetScene>>
  snapshots() const;

private:
  struct Presentation;
  std::unique_ptr<Presentation> presentation_;
  void drawTether(const glm::vec2 &from, const glm::vec2 &pointer,
                  const gleditor::ui::UiMetrics &metrics);

  KineticTetherEngine &engine_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  std::unique_ptr<gleditor::CurveRibbons> ribbons_;
};

} // namespace xanadu

#endif // XUDU_KINETIC_TETHER_OVERLAY_HPP
