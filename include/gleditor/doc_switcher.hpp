/**
 * @file doc_switcher.hpp
 * @brief UI component to switch between and close open documents.
 */
#ifndef GLEDITOR_DOC_SWITCHER_H
#define GLEDITOR_DOC_SWITCHER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>
#include <gleditor/text/shaping_cache.hpp>
#include <gleditor/ui/layout.hpp>

struct RenderState;
class Doc;

namespace render {
class RenderDevice;
}

namespace gleditor {

/**
 * @class DocumentSwitcher
 * @brief A top tab bar drawn in screen coordinates to switch between and close
 *        open documents independently.
 */
class DocumentSwitcher : public FrameContributor,
                         public PickObserver,
                         public a11y::Source {
public:
  DocumentSwitcher(std::string aFontName = {});
  ~DocumentSwitcher() override;

  DocumentSwitcher(const DocumentSwitcher &)            = delete;
  DocumentSwitcher &operator=(const DocumentSwitcher &) = delete;
  DocumentSwitcher(DocumentSwitcher &&)                 = delete;
  DocumentSwitcher &operator=(DocumentSwitcher &&)      = delete;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;

  void drawFrame(FrameContext &ctx) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void describe(a11y::Builder &into) override;
  bool performAction(std::uint64_t nodeId, a11y::Action action,
                     std::string_view value) override;
  [[nodiscard]] const ui::LayoutResult &layout() const { return layoutResult; }
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    const std::scoped_lock lock(guard);
    return revision;
  }

  static constexpr std::uint32_t kManagerTag = 0xFFFDU;
  static constexpr std::uint32_t kNewDocTag  = 0xFFFEU;

  void setCloseHandler(std::function<void(std::uint32_t docIndex)> handler) {
    const std::scoped_lock lock(guard);
    closeHandler = std::move(handler);
  }

  void setSelectHandler(std::function<void(std::uint32_t docIndex)> handler) {
    const std::scoped_lock lock(guard);
    selectHandler = std::move(handler);
  }

  void setNewDocHandler(std::function<void()> handler) {
    const std::scoped_lock lock(guard);
    newDocHandler = std::move(handler);
  }

  void setManagerHandler(std::function<void()> handler) {
    const std::scoped_lock lock(guard);
    managerHandler = std::move(handler);
  }

  void setVisible(const bool show) {
    const std::scoped_lock lock(guard);
    visible = show;
    revision++;
  }
  [[nodiscard]] bool isVisible() const {
    const std::scoped_lock lock(guard);
    return visible;
  }

  void setActiveDocIndex(const std::uint32_t index) {
    const std::scoped_lock lock(guard);
    activeIndex = index;
    revision++;
  }
  [[nodiscard]] std::uint32_t activeDocIndex() const {
    const std::scoped_lock lock(guard);
    return activeIndex;
  }

private:
  mutable std::mutex guard;
  std::string fontName;
  std::string resolvedFontName;
  std::unique_ptr<Canvas> canvas;
  render::RenderDevice *device{};
  std::optional<render::PipelineDesc> pipeline;
  text::ShapingCache shaping;
  ui::UiMetrics metrics;
  ui::Theme theme;
  ui::LayoutResult layoutResult;
  std::uint64_t builtRevision{};
  std::size_t knownDocCount{};
  std::vector<const Doc *> documents;
  std::uint32_t pickScope{};
  enum class PickAction : std::uint8_t { Select, Close, NewDocument, Manager };
  struct DocumentPickBinding {
    std::weak_ptr<Doc> document;
    PickAction action{};
  };
  std::unordered_map<std::uint32_t, DocumentPickBinding> documentPickBindings;
  std::uint32_t nextPickBindingId{1};
  std::shared_ptr<const std::vector<std::uint32_t>> pickTargets;
  std::vector<std::shared_ptr<const std::vector<std::uint32_t>>> pickSnapshots;
  bool visible{true};
  std::uint32_t activeIndex{0};
  std::uint64_t revision{1};
  std::function<void(std::uint32_t)> closeHandler;
  std::function<void(std::uint32_t)> selectHandler;
  std::function<void()> newDocHandler;
  std::function<void()> managerHandler;

  struct TabInfo {
    std::uint32_t docIndex{};
    std::string name;
    float x{};
    float y{};
    float width{};
    float height{};
    bool active{};
    std::string sourceName;
    std::uint32_t selectBinding{};
    std::uint32_t closeBinding{};
  };
  std::vector<TabInfo> currentTabs;
  void rebuild(FrameContext &ctx, const ui::UiMetrics &nextMetrics);
  std::uint32_t bindDocument(const std::shared_ptr<Doc> &document,
                             PickAction action);
  void releasePickSnapshots();
  bool dispatch(std::uint32_t tag, std::size_t documentCount);
};

} // namespace gleditor

#endif // GLEDITOR_DOC_SWITCHER_H
