// A RenderDevice that does nothing and allocates nothing, for a test that
// counts what the code under test allocates: a mock records its calls, and
// that costs allocations of its own.
#ifndef TESTS_UI_NULL_DEVICE_HPP
#define TESTS_UI_NULL_DEVICE_HPP

#include <gleditor/render/device.hpp>

class NullRenderDevice final : public render::RenderDevice {
public:
  render::Backend backend() const override { return render::Backend::OpenGL; }
  void initialize(AutoSDLWindow &) override {}
  void shutdown() override {}
  void resize(int, int) override {}

  render::BufferHandle createBuffer(render::BufferKind, std::size_t) override {
    return render::BufferHandle{1};
  }
  void destroyBuffer(render::BufferHandle) override {}
  void updateBuffer(render::BufferHandle, std::size_t,
                    std::span<const std::byte>) override {}
  render::BufferHandle resizeBuffer(render::BufferHandle buffer,
                                    std::size_t) override {
    return buffer;
  }
  void copyBufferRange(render::BufferHandle, std::size_t, std::size_t,
                       std::size_t) override {}

  render::TextureHandle createTextureArray(int, int, render::TextureFormat,
                                           int) override {
    return render::TextureHandle{1};
  }
  void generateMipmaps(render::TextureHandle) override {}
  void destroyTexture(render::TextureHandle) override {}
  void updateTextureLayer(render::TextureHandle, int, int, int, int, int,
                          std::span<const std::byte>) override {}
  render::TextureLimits textureLimits() const override { return {2048, 10}; }

  render::PipelineHandle createPipeline(const render::PipelineDesc &) override {
    return render::PipelineHandle{1};
  }

  bool beginFrame() override { return true; }
  void endFrame() override {}
  void bindPipeline(render::PipelineHandle) override {}
  void bindAtlasTexture(render::TextureHandle) override {}
  void setHighlights(std::span<const render::HighlightRange>) override {}
  void drawGlyphs(const render::DrawUniforms &, render::BufferHandle,
                  std::size_t, std::uint32_t) override {}
  bool requestPickingTag(int, int, std::uint64_t) override { return false; }
  std::optional<render::PickingResult> takePickingTag() override {
    return std::nullopt;
  }
  render::FrameImage captureColorTarget() override { return {}; }
  void waitIdle() override {}
  std::vector<render::Diagnostic> takeDiagnostics() override { return {}; }
  void setStrictDiagnostics(bool) override {}
  void setPresentEnabled(bool) override {}
};

#endif // TESTS_UI_NULL_DEVICE_HPP
