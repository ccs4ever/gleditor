#ifndef MOCK_GLEDITOR_RENDER_DEVICE_H
#define MOCK_GLEDITOR_RENDER_DEVICE_H

#include <gmock/gmock.h>

#include <gleditor/render/device.hpp>

/**
 * @brief Mock RenderDevice.
 *
 * The abstraction moved the seam from "a handful of wrapped GL calls" to the
 * whole device interface, so tests can exercise the glyph cache and buffer
 * pool without any graphics API at all.
 */
class MockRenderDevice : public render::RenderDevice {
public:
  MOCK_METHOD(render::Backend, backend, (), (const, override));
  MOCK_METHOD(void, initialize, (AutoSDLWindow & window), (override));
  MOCK_METHOD(void, shutdown, (), (override));
  MOCK_METHOD(void, resize, (int width, int height), (override));

  MOCK_METHOD(render::BufferHandle, createBuffer,
              (render::BufferKind kind, std::size_t bytes), (override));
  MOCK_METHOD(void, destroyBuffer, (render::BufferHandle buffer), (override));
  MOCK_METHOD(void, updateBuffer,
              (render::BufferHandle buffer, std::size_t offset,
               std::span<const std::byte> data),
              (override));
  MOCK_METHOD(render::BufferHandle, growBuffer,
              (render::BufferHandle buffer, std::size_t bytes), (override));

  MOCK_METHOD(render::TextureHandle, createTextureArray,
              (int size, int layers, render::TextureFormat format), (override));
  MOCK_METHOD(void, destroyTexture, (render::TextureHandle texture),
              (override));
  MOCK_METHOD(void, updateTextureLayer,
              (render::TextureHandle texture, int layer, int xOffset,
               int yOffset, int width, int height,
               std::span<const std::byte> data),
              (override));
  MOCK_METHOD(render::TextureLimits, textureLimits, (), (const, override));

  MOCK_METHOD(render::PipelineHandle, createPipeline,
              (const render::PipelineDesc &desc), (override));

  MOCK_METHOD(bool, beginFrame, (), (override));
  MOCK_METHOD(void, endFrame, (), (override));
  MOCK_METHOD(void, bindPipeline, (render::PipelineHandle pipeline),
              (override));
  MOCK_METHOD(void, setFrameUniforms, (const render::FrameUniforms &uniforms),
              (override));
  MOCK_METHOD(void, bindGlyphTexture, (render::TextureHandle texture),
              (override));
  MOCK_METHOD(void, setHighlights,
              (std::span<const render::HighlightRange> ranges), (override));
  MOCK_METHOD(void, drawGlyphs,
              (const render::DrawUniforms &uniforms,
               render::BufferHandle vertices, std::size_t vertexByteOffset,
               std::uint32_t instanceCount),
              (override));
  MOCK_METHOD(void, requestPickingTag, (int x, int y), (override));
  MOCK_METHOD(std::optional<render::PickingResult>, takePickingTag, (),
              (override));
  MOCK_METHOD(render::FrameImage, captureColorTarget, (), (override));
  MOCK_METHOD(void, waitIdle, (), (override));
};

#endif /* MOCK_GLEDITOR_RENDER_DEVICE_H */
