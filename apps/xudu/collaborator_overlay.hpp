/**
 * @file apps/xudu/collaborator_overlay.hpp
 * @brief 3D multi-author remote carets, selection highlights, and nameplate
 * badges.
 */
#ifndef XUDU_COLLABORATOR_OVERLAY_HPP
#define XUDU_COLLABORATOR_OVERLAY_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <glm/vec2.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/renderer.hpp>

namespace xudu {

class Session;

/**
 * @class CollaboratorCaretOverlay
 * @brief Renders glowing multi-author remote carets, author nameplates, and
 *        selection highlights in 3D world space at 120 FPS.
 */
class CollaboratorCaretOverlay : public gleditor::FrameContributor {
public:
  explicit CollaboratorCaretOverlay(Session &session, RendererRef renderer,
                                    std::string fontName = "Sans 9");
  ~CollaboratorCaretOverlay() override;

  // FrameContributor interface
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  void setEnabled(const bool enabled) noexcept { enabled_ = enabled; }
  [[nodiscard]] bool isEnabled() const noexcept { return enabled_; }

private:
  struct VisualState {
    glm::vec2 bottom{0.0F, 0.0F};
    glm::vec2 top{0.0F, 0.0F};
    float alpha{1.0F};
    bool initialized{false};
  };

  Session &session_;
  RendererRef renderer_;
  std::string fontName_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  bool enabled_{true};

  std::chrono::steady_clock::time_point lastFrameTime_{
      std::chrono::steady_clock::now()};
  std::map<std::string, VisualState> visuals_;

  std::uint32_t lastBroadcastDoc_{0};
  std::uint32_t lastBroadcastOffset_{0};
  std::uint32_t lastBroadcastSelLen_{0};
  bool lastCaretActive_{false};
};

} // namespace xudu

#endif // XUDU_COLLABORATOR_OVERLAY_HPP
