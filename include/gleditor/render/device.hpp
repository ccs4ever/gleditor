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
 * only exceptions are textureLimits() and capabilities(), which are fixed after
 * initialisation. A device may use threads of its own internally -- see
 * DeviceCapabilities::parallelCommandRecording -- but that is invisible to
 * callers, who still make every call from the render thread.
 */
#ifndef GLEDITOR_RENDER_DEVICE_H
#define GLEDITOR_RENDER_DEVICE_H

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <gleditor/render/diagnostics.hpp>
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
   * @brief What this device can do beyond the common interface.
   *
   * Valid after initialize(): the answer depends on what the driver and the
   * hardware turned out to support, not only on which API was asked for.
   */
  [[nodiscard]] virtual DeviceCapabilities capabilities() const {
    return DeviceCapabilities{};
  }

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
   * @brief Resize @p buffer to @p bytes, preserving as much of its contents as
   *        still fits.
   *
   * Shrinking is as useful as growing: a document that has finished loading
   * knows exactly how many rows it uses, and the room reserved on the way there
   * is room nothing will ever write to. Bytes past the new size are gone, so a
   * caller must know they are unused.
   *
   * @return A handle to the resized buffer; the old handle is consumed.
   */
  virtual BufferHandle resizeBuffer(BufferHandle buffer, std::size_t bytes) = 0;

  /**
   * @brief Copy @p bytes within one buffer, from @p srcOffset to @p dstOffset.
   *
   * Device-side: the contents are already on the device and the host does not
   * have them, so moving a run of rows cannot go through a staging copy. The
   * ranges must not overlap.
   */
  virtual void copyBufferRange(BufferHandle buffer, std::size_t srcOffset,
                               std::size_t dstOffset, std::size_t bytes) = 0;

  /**
   * @brief Create a 2D array texture of @p size x @p size with @p layers
   *        layers.
   * @param levels Mip levels to allocate, 1 for none. Levels beyond the first
   *        hold nothing until generateMipmaps() is called.
   */
  virtual TextureHandle createTextureArray(int size, int layers,
                                           TextureFormat format,
                                           int levels = 1) = 0;
  virtual void destroyTexture(TextureHandle texture)       = 0;

  /**
   * @brief Rebuild @p texture's mip levels from what is in level zero.
   *
   * A no-op on a texture allocated with a single level. Callers upload however
   * many glyphs they like and then call this once, rather than once per upload:
   * the whole chain is rebuilt each time regardless of how much of level zero
   * moved.
   */
  virtual void generateMipmaps(TextureHandle texture) = 0;

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

  /// Bind the texture subsequent draws sample from -- the glyph atlas for
  /// text, or any other array texture a pipeline's fragment stage reads, such
  /// as the image cache's.
  virtual void bindAtlasTexture(TextureHandle texture) = 0;

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
   * @brief Draw a run of glyph batches with the currently bound pipeline and
   *        texture.
   *
   * Equivalent to calling drawGlyphs() once per batch, and that is exactly what
   * the default does. The difference is that the device is handed the whole run
   * at once, so a device whose capabilities() report parallelCommandRecording
   * may record it on several threads. The batches are executed in the order
   * given whether or not they were recorded in that order, so overlapping
   * geometry looks the same either way.
   *
   * Every batch in one call must be drawable with the state bound when the call
   * is made; a device may not re-read that state part way through.
   */
  virtual void drawGlyphBatches(const std::span<const GlyphBatch> batches) {
    for (const auto &batch : batches) {
      drawGlyphs(batch.uniforms, batch.vertices, batch.vertexByteOffset,
                 batch.instanceCount);
    }
  }

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
  virtual void requestPickingTag(int coordX, int coordY) = 0;

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

  // -- driver diagnostics ---------------------------------------------------

  /**
   * @brief Take the driver diagnostics recorded since the last call.
   *
   * Every message is logged by the device as it arrives; this is what lets the
   * application show them as well. Repeats of a message already reported are
   * dropped, so polling this every frame does not produce a message per frame.
   */
  virtual std::vector<Diagnostic> takeDiagnostics() = 0;

  /**
   * @brief Decide what a driver error does.
   *
   * Off (the default), an error is logged and handed to takeDiagnostics() like
   * any other message, and the frame continues: a driver that objects to
   * something the editor can survive should not close the editor. On, the
   * first error ends the next frame with an exception, which is what automated
   * runs want -- a test that renders through a driver reporting errors has not
   * proved anything, however plausible its output looks.
   */
  virtual void setStrictDiagnostics(bool strict) = 0;

  /**
   * @brief Render frames without showing them.
   *
   * For capturing a frame on a machine that can give a context but cannot put
   * one on a screen. A headless Windows runner is the case that forced this:
   * the frame is drawn correctly and the software driver then dies inside
   * SwapBuffers, so everything a capture needs has already happened by the
   * time the one call that cannot work is made.
   *
   * The captured colour target is unaffected -- drawing goes to an offscreen
   * target either way, and presenting only copies it to the window.
   *
   * A device that cannot honour this must say so rather than ignore it: a
   * caller that asked not to present and was presented to anyway is back where
   * it started, with no way to tell.
   */
  virtual void setPresentEnabled(bool enabled) = 0;
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
