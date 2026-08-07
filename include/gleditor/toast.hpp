/**
 * @file toast.hpp
 * @brief Transient notifications drawn over the document view.
 *
 * A toast reports something the user should see but must not be interrupted
 * by: a driver diagnostic, for instance. It appears in the corner of the
 * window, stays for a few seconds and disappears on its own.
 *
 * It is drawn through the same glyph pipeline documents use, with two
 * differences. The transform is an orthographic projection in pixels rather
 * than the document camera, which is possible because the transform is a
 * per-draw uniform. And the pipeline it binds has depth testing off, so the
 * overlay is on top by virtue of being submitted last -- see
 * PipelineDesc::depthTest for why a Z value would not do.
 */
#ifndef GLEDITOR_TOAST_H
#define GLEDITOR_TOAST_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_float4x4.hpp>

#include <gleditor/buffer_pool.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/types.hpp>

struct RenderState;

namespace render {
class RenderDevice;
}

/**
 * @class ToastOverlay
 * @brief A stack of expiring notifications drawn in window pixel coordinates.
 *
 * Every method must be called on the render thread: posting rasterises text
 * through the shared glyph cache and writes device memory.
 */
class ToastOverlay {
public:
  using Clock = std::chrono::steady_clock;

  /// How long a toast stays on screen once posted.
  static constexpr std::chrono::seconds lifetime{8};
  /// Toasts shown at once. Older ones are dropped when a new one arrives, so a
  /// burst of driver messages does not fill the window.
  static constexpr std::size_t maxVisible = 4;

  /**
   * @param aDevice Device the vertex storage lives on. Not owned; must outlive
   *        the overlay.
   * @param aFontName Pango font description the text is laid out with.
   */
  ToastOverlay(render::RenderDevice *aDevice, std::string aFontName);
  ~ToastOverlay();

  ToastOverlay(const ToastOverlay &)            = delete;
  ToastOverlay &operator=(const ToastOverlay &) = delete;
  ToastOverlay(ToastOverlay &&)                 = delete;
  ToastOverlay &operator=(ToastOverlay &&)      = delete;

  /// Build the pipeline the overlay draws with. Call once, after the device is
  /// initialised.
  void createPipeline(const render::PipelineDesc &documentDesc);

  /**
   * @brief Show a message.
   *
   * The text is laid out and uploaded here rather than at draw time, so a
   * message arriving during a frame costs nothing on the frames that display
   * it.
   */
  void post(render::DiagnosticSeverity severity, std::string_view message,
            RenderState &state);

  /// Drop toasts whose lifetime has run out.
  void expire(Clock::time_point now);

  [[nodiscard]] bool empty() const { return toasts.empty(); }
  [[nodiscard]] std::size_t size() const { return toasts.size(); }

  /**
   * @brief Draw every live toast.
   *
   * Call last in the frame, after the documents: the overlay pipeline does not
   * depth test, so submission order is what puts it on top.
   */
  void draw(RenderState &state, int screenWidth, int screenHeight);

private:
  /// One notification: its rows in the shared pool, and when it dies.
  struct Toast {
    BufferPool::Allocation backing{};
    std::uint32_t instanceCount{};
    /// Size of the panel in pixels, needed to stack the next one above it.
    float width{};
    float height{};
    Clock::time_point postedAt;
    Clock::time_point expiresAt;
  };

public:
  /**
   * @brief Alpha a toast posted at @p postedAt and expiring at @p expiresAt
   *        draws at @p now.
   *
   * Derived from the clock rather than driven by the shared timeline, because
   * a toast's whole life is already a function of the clock -- it is posted,
   * it sits there, it expires -- and putting it on the timeline would make the
   * render loop consider a notification unfinished business for eight seconds,
   * which is the one thing a screenshot must not wait for.
   *
   * Eased at both ends, so the ramp starts and stops smoothly instead of
   * switching linear motion on and off.
   */
  [[nodiscard]] static float fadeFactor(Clock::time_point postedAt,
                                        Clock::time_point expiresAt,
                                        Clock::time_point now);

  /**
   * @brief Whether any notification is still fading in.
   *
   * The render loop counts this as unfinished work, which is what stops a
   * capture landing on a half-faded panel and makes two backends rendering the
   * same scene comparable. Only the fade in: it is bounded by a fraction of a
   * second, where waiting for a fade out would mean waiting out the whole
   * lifetime.
   */
  [[nodiscard]] bool fadingIn(Clock::time_point now) const;

private:

  /// Pixels between the panel edge and the text, and between stacked panels.
  static constexpr float padding = 8.0F;
  static constexpr float gap     = 6.0F;
  /// Pixels from the bottom left corner of the window to the first panel.
  static constexpr float marginX = 12.0F;
  static constexpr float marginY = 12.0F;

  /// Rows the pool starts with. A handful of short messages fit without a grow.
  static constexpr std::uint32_t initialPoolRows = 1U << 12U;

  void dropOldest();

  render::RenderDevice *device;
  std::string fontName;
  std::unique_ptr<BufferPool> pool;
  render::PipelineHandle pipeline{};
  std::vector<Toast> toasts;
};

#endif // GLEDITOR_TOAST_H
// vi: set sw=2 sts=2 ts=2 et:
