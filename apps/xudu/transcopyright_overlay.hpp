/**
 * @file transcopyright_overlay.hpp
 * @brief 3D and 2D overlay rendering Nelsonian Transcopyright paywall badges,
 * obsidian redaction quads, and interactive 1-click unlocks.
 */
#ifndef XUDU_TRANSCOPYRIGHT_OVERLAY_HPP
#define XUDU_TRANSCOPYRIGHT_OVERLAY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>

#include "core/transcopyright_logic.hpp"
#include "session.hpp"

namespace xudu {

/**
 * @class TranscopyrightOverlay
 * @brief Renders interactive Transcopyright paywalls, withheld redactions, and
 * animated uncurling transitions.
 */
class TranscopyrightOverlay : public gleditor::FrameContributor,
                              public gleditor::PickObserver {
public:
  static constexpr std::uint32_t kTagUnlockBase = 16000U;

  using UnlockCallback =
      std::function<void(std::size_t storeIndex, const PrimediaSpan &span)>;

  TranscopyrightOverlay(Session &session, RendererRef renderer,
                        std::string fontName = "Sans 10");
  ~TranscopyrightOverlay() override;

  TranscopyrightOverlay(const TranscopyrightOverlay &)            = delete;
  TranscopyrightOverlay &operator=(const TranscopyrightOverlay &) = delete;

  // FrameContributor
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // PickObserver
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void setUnlockCallback(UnlockCallback cb) { onUnlock_ = std::move(cb); }

  /// Trigger golden uncurling animation when a span unlocks
  void notifyUnlocked(std::size_t docIndex, const PrimediaSpan &span,
                      std::uint64_t cost);

  /// Currently visible badges registered for picking in this frame
  struct VisibleBadge {
    std::uint32_t tagId{0};
    std::size_t docIndex{0};
    std::size_t storeIndex{0};
    PrimediaSpan span{};
    float screenX{0.0F};
    float screenY{0.0F};
    float width{0.0F};
    float height{0.0F};
    std::string text;
    bool isLocked{false};
    std::uint32_t color{0};
  };

  [[nodiscard]] const std::vector<VisibleBadge> &activeBadges() const noexcept {
    return activeBadges_;
  }

private:
  struct UncurlingAnim {
    std::size_t docIndex{0};
    PrimediaSpan span{};
    float progress{0.0F}; // 0.0 -> 1.0
    float screenX{0.0F};
    float screenY{0.0F};
    float width{0.0F};
    float height{0.0F};
  };

  Session &session_;
  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  UnlockCallback onUnlock_;

  std::vector<VisibleBadge> activeBadges_;
  std::vector<UncurlingAnim> uncurlingAnims_;
  std::optional<std::uint32_t> hoveredTag_{std::nullopt};
};

} // namespace xudu

#endif // XUDU_TRANSCOPYRIGHT_OVERLAY_HPP
