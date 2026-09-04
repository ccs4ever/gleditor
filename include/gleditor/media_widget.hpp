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

  /**
   * @brief Load @p resource as before, then constrain playback to
   *        [@p fragment.start, @p fragment.end()) of the @p containerLength
   *        -byte file it was cut from.
   *
   * @p resource must be the *whole* container's bytes, not just the
   * fragment -- a byte range in the middle of a media file carries no
   * header of its own for a decoder to make sense of. The time range that
   * constrains playback to just the fragment cannot be computed until
   * MediaPlayer::durationSeconds() is known, which for LibVLC is only true
   * once its own asynchronous metadata parsing has finished; this records
   * @p fragment and applies it the first time drawFrame() observes a
   * duration, rather than failing silently the way calling setTimeRange()
   * with a still-zero duration would.
   *
   * A no-op fragment (@p fragment.length >= @p containerLength, the common
   * case of a span that already covers its whole container) behaves exactly
   * like load(): nothing is deferred and no range is ever applied.
   */
  bool loadFragment(MediaResourcePtr resource, const ByteRange &fragment,
                    std::uint64_t containerLength);

  // -- Document Attachment & Positioning --------------------------------------
  void attachToDocument(std::shared_ptr<Doc> aDoc, std::uint32_t byteOffset);
  void attachToPage(std::shared_ptr<Doc> aDoc, std::uint32_t pageIndex, float x,
                    float y);
  void detachFromDocument();
  [[nodiscard]] bool isDocumentAttached() const { return doc_ != nullptr; }

  void setWorldPosition(const glm::vec3 &worldPos);
  void setScreenPosition(float x, float y);
  [[nodiscard]] glm::vec3 worldPosition() const { return worldPos_; }

  /**
   * @brief This widget's own rectangle, as an anchor a beam can use
   *        directly -- pageIndex/x/y of its centre and its own height, in
   *        the same page-pixel-space convention Doc::anchorFor() returns.
   *
   * @p doc and @p docOffset are the caller's way of asking "is this the
   * widget for that span" without a separate accessor exposing doc_/
   * docOffset_ directly -- the same shape as ImageOverlay::rectFor(), so a
   * caller checking a list of widgets and a list of image placements for
   * whichever one matches a link's offset does it the same way for both.
   *
   * Shares bottomLeftOf() with drawFrame() rather than recomputing the
   * position formula a second time, which is what keeps the two from
   * drifting into disagreement the way this widget's own two anchor
   * branches once did before Phase 3 unified them.
   *
   * @return nullopt when @p doc/@p docOffset do not match this widget, when
   *         it was attached via attachToPage() rather than
   *         attachToDocument() (an explicit-page widget has no meaningful
   *         docOffset_ to match against), or when its page is not built yet.
   */
  [[nodiscard]] std::optional<Doc::Anchor>
  rectFor(const Doc &doc, std::uint32_t docOffset) const;

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

  /**
   * @brief Chrome around the video viewport: the title bar, transport
   *        buttons, seek bar and margins drawFrame() draws outside it.
   *
   * Exposed so a caller sizing a widget from a desired video *viewport*
   * size -- fitting a decoded frame's aspect ratio to a page's text width,
   * say -- can compute the whole card's setSize() from it without
   * duplicating drawFrame()'s own layout numbers and drifting from them.
   */
  static constexpr float chromeWidthPx  = 24.0F;
  static constexpr float chromeHeightPx = 88.0F;

  /// A widget sized before a real aspect is known -- or a document's own
  /// placeholder space, reserved before any widget exists at all -- has
  /// nothing else to go on. Same value MediaPlayer::aspectRatio() itself
  /// falls back to, so the two agree until a real frame arrives.
  static constexpr float defaultAspect = MediaPlayer::defaultAspect;

private:
  /// This widget's bottom-left corner in its own page's pixel space, and
  /// which page -- the one formula drawFrame() (to build a world transform)
  /// and rectFor() (to build a beam anchor) both derive from, so the two
  /// cannot drift into disagreeing about where the widget actually is.
  /// nullopt when not attached to a document, or (for an attachToDocument()
  /// widget) when docOffset_'s page is not built yet.
  struct Corner {
    std::uint32_t pageIndex{};
    float x{};
    float y{};
  };
  [[nodiscard]] std::optional<Corner> bottomLeftOf() const;

  /// Upload the player's latest decoded frame to videoTexture_, if a new one
  /// has arrived since the last call. Reallocates the texture only when the
  /// frame's own size changes, not every frame.
  void updateVideoTexture();

  /// Apply pendingFragment_ as a time range once player_->durationSeconds()
  /// is known, and forget it -- called once per drawFrame() so a fragment
  /// requested before LibVLC has finished parsing the container's metadata
  /// still gets constrained as soon as it can be.
  void applyPendingFragment();

  /// player_->play(), plus setting awaitingPlaybackStart_ -- the one place
  /// playback is started, so picked() and performAction() cannot start it
  /// one way and forget the flag the other. See awaitingPlaybackStart_'s own
  /// comment for why the flag exists at all.
  void startPlayback();

  std::string fontName_;
  std::shared_ptr<MediaPlayer> player_;
  std::unique_ptr<Canvas> canvas_;

  /// Set by loadFragment() when its fragment is a real sub-range of the
  /// container, cleared once applyPendingFragment() has translated it into a
  /// time range and handed it to player_. std::nullopt otherwise -- for a
  /// widget showing a whole file, which is the common case and needs no
  /// deferred step at all.
  std::optional<ByteRange> pendingFragment_;
  std::uint64_t pendingContainerLength_{0};

  /// Set by startPlayback(), cleared the first time drawFrame() observes
  /// state() leave Stopped or a frame become available. LibVLC's
  /// play() call (libvlc_media_player_play()) is asynchronous: state() can
  /// still report Stopped for a real stretch of wall-clock time after
  /// play() returns, before its own internal thread has processed the
  /// request at all. busy() treating only Playing/Buffering (and, since
  /// this flag was added, Opening) as busy left a window right after
  /// play() where none of those were true yet -- an automation harness
  /// polling busy() to decide when to capture a screenshot would see false
  /// and capture before the video had genuinely started, which is what this
  /// flag closes.
  bool awaitingPlaybackStart_{false};

  /// Set in deviceReady(), which is also where canvas_ is built; kept so the
  /// video texture can be (re)created and updated later, in drawFrame(),
  /// rather than only at the one moment a pipeline is stood up.
  render::RenderDevice *device_{nullptr};
  /// Square, sized to max(frame width, frame height): createTextureArray()
  /// only makes square textures, the same constraint the glyph and image
  /// atlases work within, so a frame is uploaded into its top-left corner and
  /// addressed by a UV rect rather than the whole texture.
  render::TextureHandle videoTexture_{};
  int videoFrameWidth_{0};
  int videoFrameHeight_{0};

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
