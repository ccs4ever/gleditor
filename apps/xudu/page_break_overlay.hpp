/**
 * @file apps/xudu/page_break_overlay.hpp
 * @brief Inter-paragraph hover gap affordance and page break overlay.
 */
#ifndef XUDU_PAGE_BREAK_OVERLAY_HPP
#define XUDU_PAGE_BREAK_OVERLAY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

namespace xudu {

class Session;

/**
 * @class PageBreakOverlay
 * @brief Renders the inter-paragraph hover gap affordance: a dashed accent line
 *        and centered button "[+ Split to New Page (Ctrl+Ret)]".
 */
class PageBreakOverlay : public gleditor::FrameContributor,
                         public gleditor::PickObserver {
public:
  static constexpr std::uint32_t kTagPageBreakAffordance = 13001U;

  using SplitHandler =
      std::function<void(std::uint32_t docIndex, std::uint32_t charOffset)>;

  explicit PageBreakOverlay(Session &session, RendererRef renderer,
                            std::string fontName = "Sans 10");
  ~PageBreakOverlay() override;

  // FrameContributor interface
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver interface
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void setOnSplit(SplitHandler handler) { onSplit_ = std::move(handler); }

  void setSampleForceVisible(const bool force, const std::size_t docIdx = 0,
                             const std::size_t gapOffset = 0) {
    sampleForceVisible_ = force;
    sampleDocIdx_       = docIdx;
    sampleGapOffset_    = gapOffset;
  }

  [[nodiscard]] bool isHovering() const noexcept { return hasHover_; }
  [[nodiscard]] std::uint32_t hoverCharOffset() const noexcept {
    return hoverCharOffset_;
  }

private:
  Session &session_;
  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  SplitHandler onSplit_;

  bool sampleForceVisible_{false};
  std::size_t sampleDocIdx_{0};
  std::size_t sampleGapOffset_{0};

  bool hasHover_{false};
  std::uint32_t hoverDoc_{0};
  std::uint32_t hoverCharOffset_{0};
  glm::vec2 buttonMin_{0.0F, 0.0F};
  glm::vec2 buttonMax_{0.0F, 0.0F};
};

} // namespace xudu

#endif // XUDU_PAGE_BREAK_OVERLAY_HPP
