/**
 * @file device_gl.hpp
 * @brief RenderDevice implementation for desktop OpenGL and OpenGL ES.
 *
 * One class serves both: the entry points the renderer uses exist in OpenGL
 * 3.3 core and OpenGL ES 3.0 alike, and the only real divergence -- the GLSL
 * dialect -- is handled by the shader preamble. The active profile is recorded
 * so that the few places where the APIs genuinely differ can branch on it.
 */
#ifndef GLEDITOR_RENDER_GL_DEVICE_H
#define GLEDITOR_RENDER_GL_DEVICE_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <gleditor/render/device.hpp>
#include <gleditor/render/diagnostics.hpp>
#include <gleditor/render/gl/gl_api.hpp>
#include <gleditor/render/gl/stream_buffer.hpp>

namespace render::gl {

/**
 * @class DeviceGL
 * @brief OpenGL / OpenGL ES device.
 */
class DeviceGL final : public RenderDevice {
public:
  explicit DeviceGL(Backend backend);
  ~DeviceGL() override;

  [[nodiscard]] Backend backend() const override { return backendKind; }

  /**
   * @brief No parallel recording.
   *
   * A GL context is current on exactly one thread, and every entry point that
   * records work goes through it. Making a second thread current would first
   * have to release it from this one, which serialises rather than overlaps.
   * Shared contexts share objects, not a command stream, so they do not help
   * either.
   */
  [[nodiscard]] DeviceCapabilities capabilities() const override {
    return DeviceCapabilities{};
  }

  void initialize(AutoSDLWindow &window) override;
  void shutdown() override;
  void resize(int width, int height) override;

  BufferHandle createBuffer(BufferKind kind, std::size_t bytes) override;
  void destroyBuffer(BufferHandle buffer) override;
  void updateBuffer(BufferHandle buffer, std::size_t offset,
                    std::span<const std::byte> data) override;
  BufferHandle resizeBuffer(BufferHandle buffer, std::size_t bytes) override;
  void copyBufferRange(BufferHandle buffer, std::size_t srcOffset,
                       std::size_t dstOffset, std::size_t bytes) override;

  TextureHandle createTextureArray(int size, int layers, TextureFormat format,
                                   int levels) override;
  void destroyTexture(TextureHandle texture) override;
  void generateMipmaps(TextureHandle texture) override;
  void updateTextureLayer(TextureHandle texture, int layer, int xOffset,
                          int yOffset, int width, int height,
                          std::span<const std::byte> data) override;
  [[nodiscard]] TextureLimits textureLimits() const override { return limits; }

  PipelineHandle createPipeline(const PipelineDesc &desc) override;

  bool beginFrame() override;
  void endFrame() override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindGlyphTexture(TextureHandle texture) override;
  void setHighlights(std::span<const HighlightRange> ranges) override;
  void drawGlyphs(const DrawUniforms &uniforms, BufferHandle vertices,
                  std::size_t vertexByteOffset,
                  std::uint32_t instanceCount) override;
  void requestPickingTag(int x, int y) override;
  std::optional<PickingResult> takePickingTag() override;
  FrameImage captureColorTarget() override;
  void waitIdle() override;

  std::vector<Diagnostic> takeDiagnostics() override {
    return diagnostics.drain();
  }
  void setStrictDiagnostics(bool strict) override {
    diagnostics.setStrict(strict);
  }
  void setPresentEnabled(const bool enabled) override { present = enabled; }

private:
  /// Whether endFrame() swaps the window. See RenderDevice::setPresentEnabled.
  bool present{true};

  /// Picking reads outstanding at once. Two lets a read issued this frame land
  /// while the next frame is already being drawn, which is the whole point of
  /// reading through a pixel buffer object.
  static constexpr std::size_t pickingSlots = 2;

  /**
   * @brief One asynchronous picking read in flight.
   *
   * glReadPixels into a bound pixel pack buffer returns immediately and the
   * transfer completes on the GPU's own schedule; the fence is what says when
   * the buffer's contents are actually there. Reading without one would either
   * stall the pipeline or hand back the previous frame's bytes.
   */
  struct PickingSlot {
    GLuint pbo{};
    GLsync fence{};
    int x{};
    int y{};
    bool pending{};
  };
  /// A buffer object plus the metadata resizeBuffer() needs to copy it forward.
  struct BufferRecord {
    GLuint name{};
    GLenum target{};
    std::size_t bytes{};
  };

  struct TextureRecord {
    GLuint name{};
    int size{};
    int layers{};
    /// Mip levels allocated. One means the chain is never generated.
    int levels{1};
  };

  struct PipelineRecord {
    GLuint program{};
    GLuint vao{};
    VertexLayout layout;
    GLint mvpLoc{-1};
    GLint opacityLoc{-1};
    GLint identityLoc{-1};
    GLint atlasLoc{-1};
    bool depthTest{true};
  };

  /// Compile one stage, throwing with the driver's log on failure.
  [[nodiscard]] GLuint compileStage(GLenum stage, const std::string &source,
                                    const std::string &name) const;

  /// Create the offscreen colour/picking/depth target the frame renders into.
  void createOffscreenTarget(int width, int height);
  void destroyOffscreenTarget();

  void createPickingSlots();
  void destroyPickingSlots();

  /// Turn on debug output if the context offers it. Optional: KHR_debug is not
  /// core until OpenGL 4.3 or OpenGL ES 3.2, both above what this backend asks
  /// for, so a context without it simply reports nothing.
  void setupDebugOutput();
  /// Entry point the driver calls. Records into `diagnostics` and returns; see
  /// diagnostics.hpp for why it must not throw.
  static void APIENTRY debugCallback(GLenum source, GLenum type, GLuint id,
                                     GLenum severity, GLsizei length,
                                     const GLchar *message, const void *user);
  /// Convert a top-down row to the bottom-up row OpenGL uses.
  [[nodiscard]] int flipY(int y) const;

  Backend backendKind;
  GLApi api;
  void *glContext{};
  AutoSDLWindow *targetWindow{};

  TextureLimits limits{};
  /// Diagnostics reported by the driver since the last frame boundary.
  DiagnosticSink diagnostics;

  std::unordered_map<std::uint32_t, BufferRecord> buffers;
  std::unordered_map<std::uint32_t, TextureRecord> textures;
  std::unordered_map<std::uint32_t, PipelineRecord> pipelines;
  std::uint32_t nextHandleId{1};

  PipelineHandle boundPipeline{};

  std::array<PickingSlot, pickingSlots> picking{};
  /// Next slot a request will use; slots are consumed in order so results come
  /// back oldest first.
  std::size_t nextPickingSlot{};

  GLuint highlightUbo{};
  std::unique_ptr<StreamBufferGL> pboStream;
  std::unique_ptr<StreamBufferGL> highlightUboStream;
  GLuint offscreenFbo{};
  GLuint colourRbo{};
  GLuint pickingRbo{};
  GLuint depthRbo{};
  int targetWidth{};
  int targetHeight{};
  bool initialised{};
};

} // namespace render::gl

#endif // GLEDITOR_RENDER_GL_DEVICE_H
