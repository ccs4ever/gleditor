/**
 * @file media_widget.hpp
 * @brief Document-embedded and overlay interactive media UI element.
 *
 * Provides a self-contained interactive media player element (with Play, Pause,
 * Stop, Volume, Seek Bar, Time Readout, and Video Viewport) that can be
 * embedded directly onto a document page in 3D world space or positioned as a
 * 2D overlay.
 */
#ifndef GLEDITOR_MEDIA_WIDGET_H
#define GLEDITOR_MEDIA_WIDGET_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>
#include <glm/ext/vector_float3.hpp>

#include <gleditor/a11y/tree.hpp>
#include <gleditor/canvas.hpp>
#include <gleditor/doc.hpp>
#include <gleditor/frame_contributor.hpp>
#include <gleditor/media.hpp>
#include <gleditor/pick_observer.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace gleditor {

/**
 * @class MediaWidget
 * @brief An interactive visual media player element supporting document
 *        embedding, picking hit-tests, and accessibility reporting.
 */
class MediaWidget : public FrameContributor,
                    public PickObserver,
                    public a11y::Source {
public:
  explicit MediaWidget(std::string aFontName                = "Sans 10",
                       std::shared_ptr<MediaPlayer> aPlayer = nullptr);
  ~MediaWidget() override;

  MediaWidget(const MediaWidget &)            = delete;
  MediaWidget &operator=(const MediaWidget &) = delete;
  MediaWidget(MediaWidget &&)                 = delete;
  MediaWidget &operator=(MediaWidget &&)      = delete;

  // -- Media Player & Resource ------------------------------------------------
  void setPlayer(std::shared_ptr<MediaPlayer> aPlayer);
  [[nodiscard]] std::shared_ptr<MediaPlayer> player() const { return player_; }

  bool load(MediaResourcePtr resource);

  // -- Document Attachment & Positioning --------------------------------------
  void attachToDocument(std::shared_ptr<Doc> aDoc, std::uint32_t byteOffset);
  void attachToPage(std::shared_ptr<Doc> aDoc, std::uint32_t pageIndex, float x,
                    float y);
  void detachFromDocument();
  [[nodiscard]] bool isDocumentAttached() const { return doc_ != nullptr; }

  void setWorldPosition(const glm::vec3 &worldPos);
  void setScreenPosition(float x, float y);
  [[nodiscard]] glm::vec3 worldPosition() const { return worldPos_; }

  void setSize(float width, float height);
  [[nodiscard]] float width() const { return width_; }
  [[nodiscard]] float height() const { return height_; }

  void setVisible(bool visible);
  [[nodiscard]] bool isVisible() const { return visible_; }

  void setTitle(std::string title);
  [[nodiscard]] const std::string &title() const { return title_; }

  // -- FrameContributor -------------------------------------------------------
  void deviceReady(render::RenderDevice &device,
                   const render::PipelineDesc &documentPipeline) override;
  void drawFrame(FrameContext &ctx) override;
  [[nodiscard]] bool busy() const override;

  // -- PickObserver -----------------------------------------------------------
  [[nodiscard]] bool picked(const render::PickingResult &pick,
                            RenderState &state) override;

  // -- a11y::Source -----------------------------------------------------------
  void describe(a11y::Builder &into) override;
  [[nodiscard]] std::uint64_t accessibilityRevision() const override {
    return revision_;
  }
  bool performAction(std::uint64_t nodeId, a11y::Action action,
                     std::string_view value) override;

  // Picking sub-tag offsets relative to tagBase
  static constexpr std::uint32_t tagPlay     = 1U;
  static constexpr std::uint32_t tagPause    = 2U;
  static constexpr std::uint32_t tagStop     = 3U;
  static constexpr std::uint32_t tagVolume   = 4U;
  static constexpr std::uint32_t tagSeekBase = 100U;
  static constexpr std::uint32_t tagSeekMax  = 1100U;

private:
  std::string fontName_;
  std::shared_ptr<MediaPlayer> player_;
  std::unique_ptr<Canvas> canvas_;

  std::shared_ptr<Doc> doc_;
  std::uint32_t docOffset_{0};
  std::uint32_t pageIndex_{0};
  bool explicitPage_{false};
  float pageX_{0.0F};
  float pageY_{0.0F};

  glm::vec3 worldPos_{0.0F, 0.0F, 0.0F};
  float screenX_{50.0F};
  float screenY_{50.0F};
  bool screenSpace_{false};

  float width_{360.0F};
  float height_{140.0F};
  bool visible_{true};
  std::string title_;
  std::uint64_t revision_{1};
  std::uint32_t tagBase_{0x8000U};
};

using MediaWidgetPtr = std::shared_ptr<MediaWidget>;

} // namespace gleditor

#endif // GLEDITOR_MEDIA_WIDGET_H
