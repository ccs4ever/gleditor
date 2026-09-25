/**
 * @file overview_overlay.hpp
 * @brief The whole scene condensed into a corner, with the camera's view
 *        outlined on it.
 *
 * The camera frames for reading (LayoutConfig::readableTextPx), so most of an
 * arrangement of documents is off screen at any moment. This panel shows all
 * of it at once: every page of every open document, the rectangle the camera
 * is showing, and marks for what the reader has chosen -- the selected link's
 * places and the focused ZigZag card. A click on it moves the camera there,
 * keeping the zoom.
 */
#ifndef XUDU_OVERVIEW_OVERLAY_HPP
#define XUDU_OVERVIEW_OVERLAY_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <glm/ext/vector_float2.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/state.hpp>

#include "common/xanadu/system_docs.hpp"
#include "xudu/core/framing.hpp"

namespace xudu {

class OverviewOverlay : public gleditor::FrameContributor,
                        public gleditor::PickObserver {
public:
  /// The panel's picking tag.
  static constexpr std::uint32_t kTagOverview = 19000U;

  /// World points to mark, appended to the vector given; called only when
  /// the panel is rebuilt.
  using MarkSource =
      std::function<void(const RenderState &state, std::vector<glm::vec3> &)>;
  /// Changes whenever the marks would, so the panel knows to rebuild.
  using MarkRevision = std::function<std::uint64_t()>;

  explicit OverviewOverlay(AppStateRef state) noexcept
      : state(std::move(state)) {}

  void setConfig(const xanadu::OverviewConfig &next);
  void setMarkSource(MarkSource source, MarkRevision revision) {
    markSource   = std::move(source);
    markRevision = std::move(revision);
  }
  void toggle() { visibleOverride = !isVisible(); }

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

private:
  struct Stamp {
    glm::vec3 camera{};
    float fov{};
    int width{};
    int height{};
    std::size_t documents{};
    std::size_t pages{};
    std::uint64_t marks{};
    std::uint64_t config{};
    bool visible{};
    bool operator==(const Stamp &) const = default;
  };

  [[nodiscard]] bool isVisible() const noexcept {
    return visibleOverride.value_or(config.visible);
  }
  void rebuild(gleditor::FrameContext &ctx, const Stamp &stamp);

  AppStateRef state;
  xanadu::OverviewConfig config;
  std::uint64_t configRevision{1};
  /// The toggle action's choice, which outlasts a config reload.
  std::optional<bool> visibleOverride;
  MarkSource markSource;
  MarkRevision markRevision;

  std::unique_ptr<gleditor::Canvas> canvas;
  std::optional<Stamp> builtFor;
  /// Kept between rebuilds so a rebuild reuses their storage.
  std::vector<std::pair<glm::vec2, glm::vec2>> pages;
  std::vector<glm::vec3> marks;
  /// The last fit and panel rectangle, for mapping a click back to the world.
  std::optional<xanadu::OverviewFit> lastFit;
  glm::vec2 panelMin{};
  glm::vec2 panelSize{};
  int screenHeight{};
};

} // namespace xudu

#endif // XUDU_OVERVIEW_OVERLAY_HPP
