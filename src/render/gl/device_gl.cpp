/**
 * @file device_gl.cpp
 * @brief OpenGL / OpenGL ES device implementation.
 */
#include <gleditor/render/gl/device_gl.hpp> // IWYU pragma: associated

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <cstddef>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <gleditor/render/shader_source.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace render::gl {

namespace {

/// Binding point index the Highlights uniform block is attached to.
constexpr GLuint highlightBinding = 0;
/// Texture unit the glyph atlas is bound to.
constexpr GLint glyphAtlasUnit = 0;

GLenum bufferTarget(const BufferKind kind) {
  switch (kind) {
  case BufferKind::Vertex:
    return GL_ARRAY_BUFFER;
  case BufferKind::Index:
    return GL_ELEMENT_ARRAY_BUFFER;
  case BufferKind::Uniform:
    return GL_UNIFORM_BUFFER;
  }
  throw std::invalid_argument("DeviceGL: unknown buffer kind");
}

} // namespace

DeviceGL::DeviceGL(const Backend backend) : backendKind(backend) {
  if (Backend::OpenGL != backend && Backend::OpenGLES != backend) {
    throw std::invalid_argument("DeviceGL: not a GL-family backend");
  }
}

DeviceGL::~DeviceGL() { DeviceGL::shutdown(); }

void DeviceGL::initialize(AutoSDLWindow &window) {
  targetWindow = &window;

  glContext = SDL_GL_CreateContext(window.window);
  if (nullptr == glContext) {
    throw std::runtime_error(std::string("SDL GL context creation failed: ") +
                             SDL_GetError());
  }
  if (!SDL_GL_MakeCurrent(window.window, static_cast<SDL_GLContext>(glContext))) {
    throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") +
                             SDL_GetError());
  }

  api.load();

  std::cout << std::format(
      "render: {} device, version {}\n", backendName(backendKind),
      reinterpret_cast<const char *>(api.GetString(GL_VERSION)));

  GLint maxSize = 0;
  GLint maxLayers = 0;
  api.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  api.GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
  limits = TextureLimits{maxSize, maxLayers};

  api.GenBuffers(1, &highlightUbo);
  api.BindBuffer(GL_UNIFORM_BUFFER, highlightUbo);
  api.BufferData(GL_UNIFORM_BUFFER,
                 static_cast<GLsizeiptr>(sizeof(HighlightRange) *
                                         maxHighlightRanges),
                 nullptr, GL_DYNAMIC_DRAW);
  api.BindBufferBase(GL_UNIFORM_BUFFER, highlightBinding, highlightUbo);
  api.BindBuffer(GL_UNIFORM_BUFFER, 0);

  api.Enable(GL_DEPTH_TEST);
  // The glyph quads are emitted as a triangle strip whose winding alternates
  // between the two triangles, so face culling cannot be used here.
  api.Disable(GL_CULL_FACE);
  api.FrontFace(GL_CCW);
  api.ClearColor(0, 0, 0, 1);

  int width = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window.window, &width, &height);
  createOffscreenTarget(width > 0 ? width : 1, height > 0 ? height : 1);

  initialised = true;
}

void DeviceGL::createOffscreenTarget(const int width, const int height) {
  destroyOffscreenTarget();

  targetWidth = width;
  targetHeight = height;

  api.GenFramebuffers(1, &offscreenFbo);
  api.GenRenderbuffers(1, &colourRbo);
  api.GenRenderbuffers(1, &pickingRbo);
  api.GenRenderbuffers(1, &depthRbo);

  api.BindFramebuffer(GL_FRAMEBUFFER, offscreenFbo);

  api.BindRenderbuffer(GL_RENDERBUFFER, colourRbo);
  api.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
  api.BindRenderbuffer(GL_RENDERBUFFER, pickingRbo);
  api.RenderbufferStorage(GL_RENDERBUFFER, GL_RG32UI, width, height);
  api.BindRenderbuffer(GL_RENDERBUFFER, depthRbo);
  // A sized format is required here: unsized GL_DEPTH_COMPONENT is not a valid
  // renderbuffer format on OpenGL ES.
  api.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
  api.BindRenderbuffer(GL_RENDERBUFFER, 0);

  api.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, colourRbo);
  api.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1,
                              GL_RENDERBUFFER, pickingRbo);
  api.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depthRbo);

  if (const auto status = api.CheckFramebufferStatus(GL_FRAMEBUFFER);
      GL_FRAMEBUFFER_COMPLETE != status) {
    throw std::runtime_error(
        std::format("Offscreen framebuffer incomplete: {:#x}", status));
  }

  api.BindFramebuffer(GL_FRAMEBUFFER, 0);
}

void DeviceGL::destroyOffscreenTarget() {
  if (0 != offscreenFbo) {
    api.DeleteFramebuffers(1, &offscreenFbo);
    offscreenFbo = 0;
  }
  const std::array rbos = {colourRbo, pickingRbo, depthRbo};
  for (const auto rbo : rbos) {
    if (0 != rbo) {
      api.DeleteRenderbuffers(1, &rbo);
    }
  }
  colourRbo = pickingRbo = depthRbo = 0;
}

void DeviceGL::shutdown() {
  if (!initialised) {
    return;
  }
  initialised = false;

  for (const auto &[id, record] : pipelines) {
    api.DeleteProgram(record.program);
    api.DeleteVertexArrays(1, &record.vao);
  }
  pipelines.clear();

  for (const auto &[id, record] : buffers) {
    api.DeleteBuffers(1, &record.name);
  }
  buffers.clear();

  for (const auto &[id, record] : textures) {
    api.DeleteTextures(1, &record.name);
  }
  textures.clear();

  if (0 != highlightUbo) {
    api.DeleteBuffers(1, &highlightUbo);
    highlightUbo = 0;
  }
  destroyOffscreenTarget();

  if (nullptr != glContext) {
    SDL_GL_DestroyContext(static_cast<SDL_GLContext>(glContext));
    glContext = nullptr;
  }
}

void DeviceGL::resize(const int width, const int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  if (width == targetWidth && height == targetHeight) {
    return;
  }
  createOffscreenTarget(width, height);
}

BufferHandle DeviceGL::createBuffer(const BufferKind kind,
                                    const std::size_t bytes) {
  BufferRecord record{};
  record.target = bufferTarget(kind);
  record.bytes = bytes;
  api.GenBuffers(1, &record.name);
  api.BindBuffer(record.target, record.name);
  api.BufferData(record.target, static_cast<GLsizeiptr>(bytes), nullptr,
                 GL_DYNAMIC_DRAW);
  api.BindBuffer(record.target, 0);

  const BufferHandle handle{nextHandleId++};
  buffers.emplace(handle.id, record);
  return handle;
}

void DeviceGL::destroyBuffer(const BufferHandle buffer) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    return;
  }
  api.DeleteBuffers(1, &it->second.name);
  buffers.erase(it);
}

void DeviceGL::updateBuffer(const BufferHandle buffer, const std::size_t offset,
                            const std::span<const std::byte> data) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceGL::updateBuffer: unknown buffer");
  }
  if (offset + data.size() > it->second.bytes) {
    throw std::out_of_range("DeviceGL::updateBuffer: write past end of buffer");
  }
  api.BindBuffer(it->second.target, it->second.name);
  api.BufferSubData(it->second.target, static_cast<GLintptr>(offset),
                    static_cast<GLsizeiptr>(data.size()), data.data());
  api.BindBuffer(it->second.target, 0);
}

BufferHandle DeviceGL::growBuffer(const BufferHandle buffer,
                                  const std::size_t bytes) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceGL::growBuffer: unknown buffer");
  }
  const BufferRecord old = it->second;
  if (bytes <= old.bytes) {
    return buffer;
  }

  BufferRecord grown{};
  grown.target = old.target;
  grown.bytes = bytes;
  api.GenBuffers(1, &grown.name);
  api.BindBuffer(grown.target, grown.name);
  api.BufferData(grown.target, static_cast<GLsizeiptr>(bytes), nullptr,
                 GL_DYNAMIC_DRAW);

  // GL_COPY_READ_BUFFER and GL_COPY_WRITE_BUFFER are plain binding slots with
  // no semantic meaning, which is what makes a copy between two buffers of the
  // same kind possible without disturbing the array binding.
  api.BindBuffer(GL_COPY_READ_BUFFER, old.name);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, grown.name);
  api.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                        static_cast<GLsizeiptr>(old.bytes));
  api.BindBuffer(GL_COPY_READ_BUFFER, 0);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
  api.BindBuffer(grown.target, 0);

  api.DeleteBuffers(1, &old.name);
  it->second = grown;
  return buffer;
}

TextureHandle DeviceGL::createTextureArray(const int size, const int layers,
                                           const TextureFormat format) {
  if (TextureFormat::R8 != format) {
    throw std::invalid_argument("DeviceGL: unsupported texture format");
  }

  TextureRecord record{};
  record.size = size;
  record.layers = layers;
  api.GenTextures(1, &record.name);
  api.ActiveTexture(GL_TEXTURE0);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, record.name);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  api.TexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, size, size, layers, 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, 0);

  const TextureHandle handle{nextHandleId++};
  textures.emplace(handle.id, record);
  return handle;
}

void DeviceGL::destroyTexture(const TextureHandle texture) {
  const auto it = textures.find(texture.id);
  if (textures.end() == it) {
    return;
  }
  api.DeleteTextures(1, &it->second.name);
  textures.erase(it);
}

void DeviceGL::updateTextureLayer(const TextureHandle texture, const int layer,
                                  const int xOffset, const int yOffset,
                                  const int width, const int height,
                                  const std::span<const std::byte> data) {
  if (0 == width || 0 == height) {
    return;
  }
  const auto it = textures.find(texture.id);
  if (textures.end() == it) {
    throw std::invalid_argument("DeviceGL::updateTextureLayer: unknown texture");
  }
  const auto expected = static_cast<std::size_t>(width) *
                        static_cast<std::size_t>(height);
  if (data.size() < expected) {
    throw std::invalid_argument(
        "DeviceGL::updateTextureLayer: data shorter than the given rectangle");
  }

  api.ActiveTexture(GL_TEXTURE0);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, it->second.name);
  // Glyph rows are tightly packed rather than padded to the default 4-byte
  // row alignment.
  api.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
  api.TexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, xOffset, yOffset, layer, width,
                    height, 1, GL_RED, GL_UNSIGNED_BYTE, data.data());
  api.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

GLuint DeviceGL::compileStage(const GLenum stage, const std::string &source,
                              const std::string &name) const {
  const GLuint shader = api.CreateShader(stage);
  if (0 == shader) {
    throw std::runtime_error("Failed to create shader object for " + name);
  }
  const char *text = source.c_str();
  api.ShaderSource(shader, 1, &text, nullptr);
  api.CompileShader(shader);

  GLint compiled = GL_FALSE;
  api.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (GL_TRUE != compiled) {
    GLint logLength = 0;
    api.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
    api.GetShaderInfoLog(shader, logLength, nullptr, log.data());
    api.DeleteShader(shader);
    throw std::runtime_error(
        std::format("Failed to compile {}:\n{}\nSource:\n{}", name, log, source));
  }
  return shader;
}

PipelineHandle DeviceGL::createPipeline(const PipelineDesc &desc) {
  const auto vertexSource =
      assembleShaderSource(backendKind, ShaderStage::Vertex, desc.vertexSource);
  const auto fragmentSource = assembleShaderSource(
      backendKind, ShaderStage::Fragment, desc.fragmentSource);

  const GLuint vertexShader =
      compileStage(GL_VERTEX_SHADER, vertexSource, desc.name + " vertex stage");
  const GLuint fragmentShader = compileStage(
      GL_FRAGMENT_SHADER, fragmentSource, desc.name + " fragment stage");

  PipelineRecord record{};
  record.program = api.CreateProgram();
  api.AttachShader(record.program, vertexShader);
  api.AttachShader(record.program, fragmentShader);
  api.LinkProgram(record.program);

  GLint linked = GL_FALSE;
  api.GetProgramiv(record.program, GL_LINK_STATUS, &linked);
  if (GL_TRUE != linked) {
    GLint logLength = 0;
    api.GetProgramiv(record.program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1), '\0');
    api.GetProgramInfoLog(record.program, logLength, nullptr, log.data());
    api.DeleteProgram(record.program);
    api.DeleteShader(vertexShader);
    api.DeleteShader(fragmentShader);
    throw std::runtime_error(
        std::format("Failed to link {}:\n{}", desc.name, log));
  }

  api.DetachShader(record.program, vertexShader);
  api.DetachShader(record.program, fragmentShader);
  api.DeleteShader(vertexShader);
  api.DeleteShader(fragmentShader);

  record.layout = desc.layout;
  record.projectionLoc = api.GetUniformLocation(record.program, "uProjection");
  record.viewLoc = api.GetUniformLocation(record.program, "uView");
  record.modelLoc = api.GetUniformLocation(record.program, "uModel");
  record.atlasLoc = api.GetUniformLocation(record.program, "uGlyphAtlas");

  if (const GLuint blockIndex =
          api.GetUniformBlockIndex(record.program, "Highlights");
      GL_INVALID_INDEX != blockIndex) {
    api.UniformBlockBinding(record.program, blockIndex, highlightBinding);
  }

  api.GenVertexArrays(1, &record.vao);

  const PipelineHandle handle{nextHandleId++};
  pipelines.emplace(handle.id, record);
  return handle;
}

bool DeviceGL::beginFrame() {
  api.BindFramebuffer(GL_FRAMEBUFFER, offscreenFbo);
  constexpr std::array<GLenum, 2> targets = {GL_COLOR_ATTACHMENT0,
                                             GL_COLOR_ATTACHMENT1};
  api.DrawBuffers(static_cast<GLsizei>(targets.size()), targets.data());
  api.Viewport(0, 0, targetWidth, targetHeight);
  api.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  // The picking attachment is an integer target, which the fixed-point clear
  // does not touch.
  constexpr std::array<GLuint, 4> zero = {0, 0, 0, 0};
  api.ClearBufferuiv(GL_COLOR, 1, zero.data());
  return true;
}

void DeviceGL::endFrame() {
  api.BindFramebuffer(GL_READ_FRAMEBUFFER, offscreenFbo);
  api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  api.ReadBuffer(GL_COLOR_ATTACHMENT0);
  api.BlitFramebuffer(0, 0, targetWidth, targetHeight, 0, 0, targetWidth,
                      targetHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  api.BindFramebuffer(GL_FRAMEBUFFER, 0);

  if (nullptr != targetWindow) {
    SDL_GL_SwapWindow(targetWindow->window);
  }
}

void DeviceGL::bindPipeline(const PipelineHandle pipeline) {
  const auto it = pipelines.find(pipeline.id);
  if (pipelines.end() == it) {
    throw std::invalid_argument("DeviceGL::bindPipeline: unknown pipeline");
  }
  boundPipeline = pipeline;
  api.UseProgram(it->second.program);
  api.BindVertexArray(it->second.vao);
  if (-1 != it->second.atlasLoc) {
    api.Uniform1i(it->second.atlasLoc, glyphAtlasUnit);
  }
}

void DeviceGL::setFrameUniforms(const FrameUniforms &uniforms) {
  const auto it = pipelines.find(boundPipeline.id);
  if (pipelines.end() == it) {
    return;
  }
  if (-1 != it->second.projectionLoc) {
    api.UniformMatrix4fv(it->second.projectionLoc, 1, GL_FALSE,
                         uniforms.projection.data());
  }
  if (-1 != it->second.viewLoc) {
    api.UniformMatrix4fv(it->second.viewLoc, 1, GL_FALSE, uniforms.view.data());
  }
}

void DeviceGL::bindGlyphTexture(const TextureHandle texture) {
  const auto it = textures.find(texture.id);
  if (textures.end() == it) {
    return;
  }
  api.ActiveTexture(GL_TEXTURE0 + glyphAtlasUnit);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, it->second.name);
}

void DeviceGL::setHighlights(const std::span<const HighlightRange> ranges) {
  const auto count =
      std::min<std::size_t>(ranges.size(), maxHighlightRanges);
  api.BindBuffer(GL_UNIFORM_BUFFER, highlightUbo);
  api.BufferSubData(GL_UNIFORM_BUFFER, 0,
                    static_cast<GLsizeiptr>(count * sizeof(HighlightRange)),
                    ranges.data());
  api.BindBuffer(GL_UNIFORM_BUFFER, 0);
}

void DeviceGL::drawGlyphs(const DrawUniforms &uniforms,
                          const BufferHandle vertices,
                          const std::size_t vertexByteOffset,
                          const std::uint32_t instanceCount) {
  if (0 == instanceCount) {
    return;
  }
  const auto pipelineIt = pipelines.find(boundPipeline.id);
  const auto bufferIt = buffers.find(vertices.id);
  if (pipelines.end() == pipelineIt || buffers.end() == bufferIt) {
    throw std::invalid_argument("DeviceGL::drawGlyphs: unknown pipeline or buffer");
  }
  const auto &record = pipelineIt->second;

  if (-1 != record.modelLoc) {
    api.UniformMatrix4fv(record.modelLoc, 1, GL_FALSE, uniforms.model.data());
  }

  // The draw starts partway into the vertex buffer. Baking that offset into
  // the attribute pointers works on OpenGL ES too, where the base-instance
  // draw entry points that would otherwise express it do not exist.
  api.BindBuffer(GL_ARRAY_BUFFER, bufferIt->second.name);
  for (const auto &attribute : record.layout.attributes) {
    const auto offset = vertexByteOffset + attribute.offset;
    api.EnableVertexAttribArray(attribute.location);
    if (AttributeType::UnsignedInt == attribute.type) {
      api.VertexAttribIPointer(
          attribute.location, attribute.components, GL_UNSIGNED_INT,
          static_cast<GLsizei>(record.layout.stride),
          // NOLINTNEXTLINE(performance-no-int-to-ptr)
          reinterpret_cast<const void *>(offset));
    } else {
      api.VertexAttribPointer(
          attribute.location, attribute.components, GL_FLOAT, GL_FALSE,
          static_cast<GLsizei>(record.layout.stride),
          // NOLINTNEXTLINE(performance-no-int-to-ptr)
          reinterpret_cast<const void *>(offset));
    }
    // Every attribute is per-instance: the quad's own four vertices carry no
    // data and are synthesised from the vertex index in the shader.
    api.VertexAttribDivisor(attribute.location, 1);
  }

  api.DrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4,
                          static_cast<GLsizei>(instanceCount));
}

FrameImage DeviceGL::captureColorTarget() {
  FrameImage image;
  image.width  = targetWidth;
  image.height = targetHeight;
  image.rgba.resize(static_cast<std::size_t>(targetWidth) *
                    static_cast<std::size_t>(targetHeight) * 4);

  api.BindFramebuffer(GL_READ_FRAMEBUFFER, offscreenFbo);
  api.ReadBuffer(GL_COLOR_ATTACHMENT0);
  api.PixelStorei(GL_PACK_ALIGNMENT, 1);
  api.ReadPixels(0, 0, targetWidth, targetHeight, GL_RGBA, GL_UNSIGNED_BYTE,
                 image.rgba.data());
  api.PixelStorei(GL_PACK_ALIGNMENT, 4);

  // GL hands back rows bottom-up; flip so the result matches the top-down
  // convention FrameImage promises.
  const auto rowBytes = static_cast<std::size_t>(targetWidth) * 4;
  for (int row = 0; row < targetHeight / 2; row++) {
    auto *top = image.rgba.data() + (static_cast<std::size_t>(row) * rowBytes);
    auto *bottom =
        image.rgba.data() +
        (static_cast<std::size_t>(targetHeight - 1 - row) * rowBytes);
    std::swap_ranges(top, top + rowBytes, bottom);
  }
  return image;
}

void DeviceGL::waitIdle() {
  // Every GL command in this backend is issued from the render thread and the
  // driver serialises them, so there is nothing to wait for.
}

} // namespace render::gl
// vi: set sw=2 sts=2 ts=2 et:
