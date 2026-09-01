/**
 * @file floating_toolbar_3d.hpp
 * @brief Floating 3D word-processing and operational controls hovering above
 *        the active document in 3D world space.
 */
#ifndef GLEDITOR_FLOATING_TOOLBAR_3D_HPP
#define GLEDITOR_FLOATING_TOOLBAR_3D_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
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
 * @class FloatingToolbar3D
 * @brief Floating 3D word-processing and operational controls hovering above
 *        the active document in 3D world space.
 */
class FloatingToolbar3D : public FrameContributor,
                          public PickObserver,
                          public a11y::Source {
public:
  enum class ButtonId : std::uint32_t {
    // Operational
    NewDoc = 100,
    OpenFile,
    SaveDoc,
    CloseDoc,
    OverviewTray,

    // Formatting
    Bold,
    Italic,
    Underline,
    Strikethrough,

    // Headings & Scale
    Heading1,
    Heading2,
    Heading3,
    FontDec,
    FontInc,

    // Paragraph Alignment
    AlignLeft,
    AlignCenter,
    AlignRight,

    // Structured blocks / lists
    ListBullet,
    ListNumbered,
    CodeBlock,
    QuoteBlock
  };

  FloatingToolbar3D(std::string aFontName = "Sans 10");
  ~FloatingToolbar3D() override;

  FloatingToolbar3D(const FloatingToolbar3D &)            = delete;
  FloatingToolbar3D &operator=(const FloatingToolbar3D &) = delete;
  FloatingToolbar3D(FloatingToolbar3D &&)                 = delete;
  FloatingToolbar3D &operator=(FloatingToolbar3D &&)      = delete;

  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;

  void drawFrame(FrameContext &ctx) override;
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  void describe(a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision;
  }
  bool performAction(std::uint64_t nodeId, a11y::Action action,
                     std::string_view value) override;

  /// Register action dispatcher callback
  void setActionHandler(
      std::function<void(ButtonId btn, std::uint32_t activeDocIndex)> handler) {
    actionHandler = std::move(handler);
  }

  void setActiveDocIndex(const std::uint32_t index) {
    activeDoc = index;
    revision++;
  }
  [[nodiscard]] std::uint32_t activeDocIndex() const { return activeDoc; }

  void setVisible(const bool show) {
    visible = show;
    revision++;
  }
  [[nodiscard]] bool isVisible() const { return visible; }

  // Toggle states for visual button highlights
  void setFormattingState(const bool bold, const bool italic,
                          const bool underline, const bool strike) {
    isBold      = bold;
    isItalic    = italic;
    isUnderline = underline;
    isStrike    = strike;
    revision++;
  }

  void setHeadingLevel(const int level) {
    headingLevel = level;
    revision++;
  }

private:
  std::string fontName;
  std::unique_ptr<Canvas> canvas;
  bool visible{true};
  std::uint32_t activeDoc{0};
  std::uint64_t revision{1};
  std::function<void(ButtonId, std::uint32_t)> actionHandler;

  bool isBold{false};
  bool isItalic{false};
  bool isUnderline{false};
  bool isStrike{false};
  int headingLevel{0};

  struct ButtonLayout {
    ButtonId id{};
    std::string label;
    std::string tooltip;
    float x{};
    float y{};
    float width{};
    float height{};
    bool active{};
    bool isSeparator{false};
  };

  std::vector<ButtonLayout> currentButtons;
};

} // namespace gleditor

#endif // GLEDITOR_FLOATING_TOOLBAR_3D_HPP
