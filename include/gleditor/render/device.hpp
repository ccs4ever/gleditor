/**
 * @file device.hpp
 * @brief The rendering interface the rest of the application is written
 *        against.
 *
 * A RenderDevice owns the connection to a graphics API and every resource
 * created through it. Application code never sees an API handle: it creates
 * buffers and textures, describes a pipeline once, and then submits frames.
 *
 * Threading: a device is created and used entirely on the render thread. The
 * only exception is textureLimits(), which is fixed after initialisation.
 */
#ifndef GLEDITOR_RENDER_DEVICE_H
#define GLEDITOR_RENDER_DEVICE_H

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include <gleditor/render/types.hpp>

struct AutoSDLWindow;

namespace render {

/**
 * @class RenderDevice
 * @brief Abstract graphics device.
 */
class RenderDevice {
public:
  virtual ~RenderDevice() = default;

  RenderDevice()                                = default;
  RenderDevice(const RenderDevice &)            = delete;
  RenderDevice &operator=(const RenderDevice &) = delete;
  RenderDevice(RenderDevice &&)                 = delete;
  RenderDevice &operator=(RenderDevice &&)      = delete;

  /// Which API this device drives.
  [[nodiscard]] virtual Backend backend() const = 0;

  /**
   * @brief Bring the device up against an already-created window.
   *
   * The window must have been created with the flags backendWindowFlags()
   * reports for this backend.
   */
  virtual void initialize(AutoSDLWindow &window) = 0;

  /// Release every device resource. Safe to call more than once.
  virtual void shutdown() = 0;

  /// React to a new drawable size in pixels.
  virtual void resize(int width, int height) = 0;

  // -- resources ------------------------------------------------------------

  /// Allocate an uninitialised buffer of @p bytes.
  virtual BufferHandle createBuffer(BufferKind kind, std::size_t bytes) = 0;
  virtual void destroyBuffer(BufferHandle buffer)                       = 0;

  /// Overwrite a byte range of @p buffer.
  virtual void updateBuffer(BufferHandle buffer, std::size_t offset,
                            std::span<const std::byte> data) = 0;

  /**
   * @brief Grow @p buffer to @p bytes, preserving its existing contents.
   * @return A handle to the larger buffer; the old handle is consumed.
   */
  virtual BufferHandle growBuffer(BufferHandle buffer, std::size_t bytes) = 0;

  /// Create a 2D array texture of @p size x @p size with @p layers layers.
  virtual TextureHandle createTextureArray(int size, int layers,
                                           TextureFormat format) = 0;
  virtual void destroyTexture(TextureHandle texture)             = 0;

  /**
   * @brief Upload a tightly packed sub-rectangle into one array layer.
   * @param data Exactly `width * height` bytes for TextureFormat::R8.
   */
  virtual void updateTextureLayer(TextureHandle texture, int layer, int xOffset,
                                  int yOffset, int width, int height,
                                  std::span<const std::byte> data) = 0;

  /// Atlas sizing limits, valid after initialize().
  [[nodiscard]] virtual TextureLimits textureLimits() const = 0;

  /// Build the glyph pipeline. Called once, after initialize().
  virtual PipelineHandle createPipeline(const PipelineDesc &desc) = 0;

  // -- frame submission -----------------------------------------------------

  /**
   * @brief Start recording a frame.
   * @return false when the frame must be skipped, for instance because the
   *         swapchain is being rebuilt. Callers must not submit draws or call
   *         endFrame() in that case.
   */
  virtual bool beginFrame() = 0;

  /// Finish the frame and present it.
  virtual void endFrame() = 0;

  /// Select the pipeline subsequent draws use.
  virtual void bindPipeline(PipelineHandle pipeline) = 0;

  /// Set the camera uniforms for this frame. Call after beginFrame().
  virtual void setFrameUniforms(const FrameUniforms &uniforms) = 0;

  /// Bind the glyph atlas subsequent draws sample from.
  virtual void bindGlyphTexture(TextureHandle texture) = 0;

  /// Replace the highlight range table.
  virtual void setHighlights(std::span<const HighlightRange> ranges) = 0;

  /**
   * @brief Draw @p instanceCount glyph quads.
   *
   * Per-instance attributes are read from @p vertices starting at
   * @p vertexByteOffset, using the layout the bound pipeline was created with.
   * Each instance expands to a four-vertex triangle strip in the shader.
   */
  virtual void drawGlyphs(const DrawUniforms &uniforms, BufferHandle vertices,
                          std::size_t vertexByteOffset,
                          std::uint32_t instanceCount) = 0;

  /**
   * @brief Queue a read of the picking target at one pixel.
   *
   * Call inside a frame, after the draws whose result should be sampled.
   * Coordinates are in pixels from the top left of the drawable, the same
   * convention as window and mouse coordinates.
   *
   * The read is asynchronous: it does not stall the pipeline and the answer is
   * not available on return. Poll takePickingTag() on later frames. Requests
   * beyond the number the device can have outstanding are dropped rather than
   * queued, since a stale pick is worth less than a live frame.
   */
  virtual void requestPickingTag(int x, int y) = 0;

  /**
   * @brief Collect a picking result whose readback has completed.
   * @return The oldest finished result, or nullopt if none is ready yet.
   */
  virtual std::optional<PickingResult> takePickingTag() = 0;

  /**
   * @brief Read the colour target back to host memory.
   *
   * Call after endFrame(); the colour target keeps its contents until the next
   * frame clears it. Beyond being a screenshot facility, this is what makes the
   * backends comparable: the same document rendered through each one can be
   * diffed pixel by pixel.
   */
  virtual FrameImage captureColorTarget() = 0;

  /**
   * @brief Block until the device is idle.
   *
   * Required before destroying resources that in-flight frames may still be
   * reading.
   */
  virtual void waitIdle() = 0;
};

/// SDL window creation flags a given backend requires.
std::uint64_t backendWindowFlags(Backend backend);

/**
 * @brief Apply the SDL GL attributes a backend needs.
 *
 * Must run before the window is created; a no-op for backends that do not go
 * through SDL's GL context management.
 */
void configureBackendWindowAttributes(Backend backend);

/// True when @p backend was compiled into this binary.
bool backendCompiledIn(Backend backend);

/// Construct a device for @p backend. Throws std::runtime_error if the backend
/// was not compiled in.
std::unique_ptr<RenderDevice> createDevice(Backend backend);

} // namespace render

#endif // GLEDITOR_RENDER_DEVICE_H
// vi: set sw=2 sts=2 ts=2 et:
