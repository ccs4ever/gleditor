/**
 * @file doc_switcher.hpp
 * @brief UI component to switch between and close open documents.
 */
#ifndef GLEDITOR_DOC_SWITCHER_H
#define GLEDITOR_DOC_SWITCHER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

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
  DocumentSwitcher(std::string aFontName = "Sans 10");
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
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision;
  }

  void setCloseHandler(std::function<void(std::uint32_t docIndex)> handler) {
    closeHandler = std::move(handler);
  }

  void setSelectHandler(std::function<void(std::uint32_t docIndex)> handler) {
    selectHandler = std::move(handler);
  }

  void setVisible(const bool show) {
    visible = show;
    revision++;
  }
  [[nodiscard]] bool isVisible() const { return visible; }

  void setActiveDocIndex(const std::uint32_t index) {
    activeIndex = index;
    revision++;
  }
  [[nodiscard]] std::uint32_t activeDocIndex() const { return activeIndex; }

private:
  std::string fontName;
  std::unique_ptr<Canvas> canvas;
  bool visible{true};
  std::uint32_t activeIndex{0};
  std::uint64_t revision{1};
  std::function<void(std::uint32_t)> closeHandler;
  std::function<void(std::uint32_t)> selectHandler;

  struct TabInfo {
    std::uint32_t docIndex{};
    std::string name;
    float x{};
    float y{};
    float width{};
    float height{};
    bool active{};
  };
  std::vector<TabInfo> currentTabs;
};

} // namespace gleditor

#endif // GLEDITOR_DOC_SWITCHER_H
