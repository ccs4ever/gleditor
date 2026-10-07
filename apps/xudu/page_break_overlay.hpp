/**
 * @file apps/xudu/page_break_overlay.hpp
 * @brief Inter-paragraph hover gap affordance and page break overlay.
 */
#ifndef XUDU_PAGE_BREAK_OVERLAY_HPP
#define XUDU_PAGE_BREAK_OVERLAY_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <glm/vec2.hpp>

#include "common/ui/page_break_presentation.hpp"
#include "common/xanadu/microversion.hpp"
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

namespace xanadu {

class Session;

/**
 * @class PageBreakOverlay
 * @brief Renders the inter-paragraph hover gap affordance: a dashed accent line
 *        and centered button "[+ Split to New Page (Ctrl+Ret)]".
 */
class PageBreakOverlay : public gleditor::FrameContributor,
                         public gleditor::PickObserver,
                         public gleditor::a11y::Source {
public:
  static constexpr std::uint32_t kTagPageBreakAffordance = 13001U;

  using SplitHandler =
      std::function<void(std::uint32_t docIndex, std::uint32_t charOffset)>;

  explicit PageBreakOverlay(Session &session, RendererRef renderer,
                            std::string fontName = {});
  ~PageBreakOverlay() override;

  // FrameContributor interface
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver interface
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void describe(gleditor::a11y::Builder &builder) override {
    presentation_.describe(builder);
  }
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return presentation_.accessibilityRevision();
  }
  bool performAction(std::uint64_t id, gleditor::a11y::Action action,
                     std::string_view value) override {
    return presentation_.performAction(id, action, value);
  }

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
  struct Target {
    std::uint32_t document{}, offset{};
    std::size_t store{}, operations{};
    std::uint64_t generation{};
    xanadu::MicroversionId version;
    bool operator==(const Target &) const = default;
  };
  PageBreakPresentation presentation_;
  std::optional<Target> target_;
  std::uint64_t targetRevision_{};
  std::uint64_t drawnRevision_{};
  std::optional<std::array<float, 3>> drawnGap_;
  struct BoundaryCache {
    std::optional<Target> stamp;
    std::vector<std::uint32_t> offsets;
  };
  std::vector<BoundaryCache> boundaries_;
  mutable std::mutex actionMutex_;
  std::optional<Target> pendingSplit_;
  void selectTarget(std::optional<Target>);
  void drainActions();
};

} // namespace xanadu

#endif // XUDU_PAGE_BREAK_OVERLAY_HPP
