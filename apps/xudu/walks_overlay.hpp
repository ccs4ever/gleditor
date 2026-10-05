#ifndef XUDU_WALKS_OVERLAY_HPP
#define XUDU_WALKS_OVERLAY_HPP

#include "xudu/link_panel_overlay.hpp"
#include <gleditor/modal_input.hpp>
#include <map>
#include <mutex>

namespace xudu {

/// A preview cursor is deliberately separate from the navigator's current
/// visit.
class WalksOverlay final : public gleditor::FrameContributor,
                           public gleditor::ModalInput,
                           public gleditor::a11y::Source {
public:
  WalksOverlay(LinkContext &context, RendererRef renderer)
      : context_(context), renderer_(std::move(renderer)) {}
  void open();
  void setConfig(const xanadu::LinkPanelConfig &config);
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &pipeline) override;
  void drawFrame(gleditor::FrameContext &frame) override;
  bool grabbing() const override;
  bool keyPressed(gleditor::Key key, gleditor::KeyMods mods) override;
  void textTyped(const std::string &text) override;
  bool pointerPressed(int x, int y) override;
  bool permitsCommand(std::string_view name) const override {
    return name == xanadu::settings::kKeymapQuit;
  }
  std::optional<gleditor::InputArea> textArea() const override;
  void describe(gleditor::a11y::Builder &builder) override;
  std::uint64_t accessibilityRevision() const override;
  bool performAction(std::uint64_t id, gleditor::a11y::Action action,
                     std::string_view value) override;

private:
  enum Control : std::uint64_t {
    Previous = 1,
    Next,
    Restore,
    Reference,
    Edit,
    Save,
    Close
  };
  static constexpr std::uint64_t kNote = 20;
  static constexpr std::uint64_t kRow  = 100;
  void activate(std::uint64_t control);
  void move(int delta);
  LinkContext &context_;
  RendererRef renderer_;
  mutable std::mutex mutex_;
  bool visible_{};
  bool editing_{};
  std::vector<xanadu::Visit> visits_;
  std::size_t chosen_{};
  std::size_t first_{};
  std::uint64_t focus_{};
  std::string note_, status_;
  std::vector<std::string> preview_;
  std::map<std::uint64_t, std::string> labels_;
  std::map<std::uint64_t, gleditor::InputArea> areas_;
  xanadu::LinkPanelConfig config_;
  std::unique_ptr<gleditor::Canvas> canvas_;
  render::RenderDevice *device_{};
  render::PipelineDesc pipeline_;
  std::uint64_t revision_{1}, built_{}, contextRevision_{};
  int width_{}, height_{};
};
} // namespace xudu
#endif
