/**
 * @file transcopyright_overlay.hpp
 * @brief Retained paywall badges, withheld labels and unlock animations.
 */
#ifndef XUDU_TRANSCOPYRIGHT_OVERLAY_HPP
#define XUDU_TRANSCOPYRIGHT_OVERLAY_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/renderer.hpp>
#include <gleditor/text/shaping_cache.hpp>

#include "common/xanadu/transcopyright_logic.hpp"

namespace xanadu {
class Session;

class TranscopyrightOverlay : public gleditor::FrameContributor,
                              public gleditor::PickObserver,
                              public gleditor::a11y::Source {
public:
  static constexpr std::uint32_t kTagUnlockBase = 16000U;
  using UnlockCallback = std::function<void(std::size_t storeIndex,
                                            const xanadu::PrimediaSpan &span)>;
  using HoleSource =
      std::function<std::vector<xanadu::HoleSpanInfo>(const RenderState &)>;
  using HoleRevision = std::function<std::uint64_t()>;
  using Projector    = std::function<std::optional<gleditor::ui::Rect>(
      const xanadu::HoleSpanInfo &, const gleditor::FrameContext &)>;

  TranscopyrightOverlay(Session &session, RendererRef renderer,
                        std::string fontName = {});
  /// Adapts a domain snapshot provider; no store or renderer is required.
  TranscopyrightOverlay(HoleSource source, HoleRevision revision,
                        Projector projector = {}, std::string fontName = {});
  ~TranscopyrightOverlay() override;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;
  void setUnlockCallback(UnlockCallback cb) { onUnlock_ = std::move(cb); }
  void notifyUnlocked(std::size_t docIndex, const xanadu::PrimediaSpan &span,
                      std::uint64_t cost);

  void describe(gleditor::a11y::Builder &) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t, gleditor::a11y::Action,
                     std::string_view) override;

  struct VisibleBadge {
    std::uint32_t tagId{};
    std::size_t docIndex{}, storeIndex{};
    xanadu::PrimediaSpan span{};
    float screenX{}, screenY{}, width{}, height{};
    std::string text;
    bool isLocked{};
    std::uint32_t color{};
  };
  [[nodiscard]] const std::vector<VisibleBadge> &activeBadges() const noexcept {
    return activeBadges_;
  }

private:
  struct Target {
    std::size_t docIndex{}, storeIndex{};
    xanadu::PrimediaSpan span{};
    bool operator==(const Target &) const = default;
  };
  struct PendingAction {
    std::uint32_t id{};
    Target target;
  };
  struct Entry {
    xanadu::HoleSpanInfo hole;
    std::string label;
    std::uint32_t id{};
    float intrinsicWidth{};
    std::optional<gleditor::ui::Rect> box;
    const Doc *document{};
    std::uint64_t edits{};
    std::size_t pages{}, builtPages{};
    std::optional<Doc::Anchor> first, last;
  };
  struct AccessibleBadge {
    std::uint32_t id{};
    Target target;
    std::string label;
    gleditor::a11y::Rect bounds;
    bool locked{};
  };
  struct UncurlingAnim {
    float progress{};
    gleditor::ui::Rect box;
  };
  [[nodiscard]] std::optional<gleditor::ui::Rect>
  project(Entry &, const gleditor::FrameContext &);
  void refreshHoles(const RenderState &);
  void releaseSnapshots();
  void rebuild(gleditor::FrameContext &);

  HoleSource source_;
  HoleRevision sourceRevision_;
  Projector projector_;
  std::string fontName_, drawnFont_;
  UnlockCallback onUnlock_;
  std::shared_ptr<TranscopyrightOverlay *> lifetime_;
  render::RenderDevice *device_{};
  render::PipelineDesc pipeline_;
  std::unique_ptr<gleditor::Canvas> canvas_, animationCanvas_;
  gleditor::text::ShapingCache shaping_;
  gleditor::text::FontFacePtr font_;
  gleditor::ui::UiMetrics metrics_;
  gleditor::ui::Theme theme_;
  bool built_{}, holesDirty_{true};
  std::uint64_t holesRevision_{};
  std::vector<Entry> entries_;
  std::vector<VisibleBadge> activeBadges_;
  std::vector<UncurlingAnim> uncurlingAnims_;
  std::atomic<bool> animating_{};
  RenderState *pickState_{};
  std::uint32_t pickScope_{}, nextId_{kTagUnlockBase};
  std::shared_ptr<const std::vector<std::uint32_t>> pickTargets_;
  std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> snapshots_;
  std::unordered_map<std::uint32_t, Target> targets_;
  std::atomic<std::shared_ptr<const std::vector<AccessibleBadge>>> accessible_;
  std::atomic<std::uint64_t> revision_{1};
  mutable std::mutex actionMutex_;
  std::vector<PendingAction> pendingActions_;
};
} // namespace xanadu
#endif
