/**
 * @file device_gl.cpp
 * @brief OpenGL / OpenGL ES device implementation.
 */
#include <gleditor/render/gl/device_gl.hpp> // IWYU pragma: associated

#include <array>
#include <cstddef>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gleditor/render/shader_source.hpp>
#include <gleditor/sdl_compat.hpp>
#include <gleditor/sdl_wrap.hpp>

namespace render::gl {

namespace {

/// Binding point index the Highlights uniform block is attached to.
constexpr GLuint highlightBinding = 0;
/// Texture unit the glyph atlas is bound to.
constexpr GLint glyphAtlasUnit = 0;

/// Bytes one picking read occupies: four unsigned integer components. Only two
/// carry data, but four is the component count OpenGL ES guarantees for reading
/// an integer colour buffer.
constexpr GLsizeiptr pickingReadBytes = 4 * sizeof(GLuint);

GLenum bufferTarget(const BufferKind kind) {
  switch (kind) {
  case BufferKind::Vertex:
    return GL_ARRAY_BUFFER;
  case BufferKind::Index:
    return GL_ELEMENT_ARRAY_BUFFER;
  case BufferKind::Uniform:
    return GL_UNIFORM_BUFFER;
  case BufferKind::Readback:
    return GL_PIXEL_PACK_BUFFER;
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
  if (!sdl::glMakeCurrent(window.window,
                          static_cast<SDL_GLContext>(glContext))) {
    throw std::runtime_error(std::string("SDL_GL_MakeCurrent failed: ") +
                             SDL_GetError());
  }

  api.load();
  setupDebugOutput();

  std::cout << std::format(
      "render: {} device, version {}\n", backendName(backendKind),
      reinterpret_cast<const char *>(api.GetString(GL_VERSION)));

  GLint maxSize   = 0;
  GLint maxLayers = 0;
  api.GetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
  api.GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
  limits = TextureLimits{maxSize, maxLayers};

  api.GenBuffers(1, &highlightUbo);
  api.BindBuffer(GL_UNIFORM_BUFFER, highlightUbo);
  api.BufferData(
      GL_UNIFORM_BUFFER,
      static_cast<GLsizeiptr>(sizeof(HighlightRange) * maxHighlightRanges),
      nullptr, GL_DYNAMIC_DRAW);
  api.BindBufferBase(GL_UNIFORM_BUFFER, highlightBinding, highlightUbo);
  api.BindBuffer(GL_UNIFORM_BUFFER, 0);

  api.Enable(GL_DEPTH_TEST);
  // Equal depths pass, and the later draw wins. A page puts its glyphs a tenth
  // of a unit in front of its paper, which is a separation the depth buffer
  // cannot always resolve: with the near plane at a tenth of a unit and a
  // document a thousand units away, one step of a 24-bit depth buffer is over
  // half a unit there. Under GL_LESS the glyphs then landed in the same step as
  // the paper they sit on and were discarded -- a page that rendered blank
  // white, with the text still answering picking queries, depending on nothing
  // more than where the document happened to be. LEQUAL makes the order within
  // a page the order it was submitted in, which is the order the page was built
  // in, and leaves the comparison between documents alone.
  api.DepthFunc(GL_LEQUAL);
  // The glyph quads are emitted as a triangle strip whose winding alternates
  // between the two triangles, so face culling cannot be used here.
  api.Disable(GL_CULL_FACE);
  api.FrontFace(GL_CCW);
  api.ClearColor(0, 0, 0, 1);

  int width  = 0;
  int height = 0;
  SDL_GetWindowSizeInPixels(window.window, &width, &height);
  createOffscreenTarget(width > 0 ? width : 1, height > 0 ? height : 1);
  createPickingSlots();

  initialised = true;
}

void APIENTRY DeviceGL::debugCallback(const GLenum /*source*/,
                                      const GLenum type, const GLuint /*id*/,
                                      const GLenum severity,
                                      const GLsizei length,
                                      const GLchar *message, const void *user) {
  auto *sink = static_cast<DiagnosticSink *>(const_cast<void *>(user));
  if (nullptr == sink || nullptr == message) {
    return;
  }

  // An API error or undefined behaviour is a bug in this program; anything
  // else is advisory. Only the former stops the frame.
  auto level = DiagnosticSeverity::Info;
  if (GL_DEBUG_TYPE_ERROR == type || GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR == type) {
    level = DiagnosticSeverity::Error;
  } else if (GL_DEBUG_SEVERITY_HIGH == severity ||
             GL_DEBUG_SEVERITY_MEDIUM == severity) {
    level = DiagnosticSeverity::Warning;
  }

  // length is negative when the driver passes a null-terminated string.
  const std::string_view text =
      length < 0 ? std::string_view{message}
                 : std::string_view{message, static_cast<std::size_t>(length)};
  sink->record(level, text);
}

void DeviceGL::setupDebugOutput() {
  if (!api.loadDebugOutput()) {
    std::cerr << "render: GL_KHR_debug unavailable; driver diagnostics off\n";
    return;
  }

  api.Enable(GL_DEBUG_OUTPUT);
  // Synchronous delivery costs performance but calls the callback on the thread
  // and at the point of the offending command, so a recorded error belongs to
  // the call that caused it rather than to some later frame.
  api.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  // Notifications are mostly buffer-allocation chatter; leave them off so the
  // sink's dedupe budget goes on things worth reading.
  api.DebugMessageControl(GL_DONT_CARE, GL_DONT_CARE,
                          GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
  api.DebugMessageCallback(debugCallback, &diagnostics);
}

void DeviceGL::createOffscreenTarget(const int width, const int height) {
  destroyOffscreenTarget();

  targetWidth  = width;
  targetHeight = height;

  api.GenFramebuffers(1, &offscreenFbo);
  api.GenRenderbuffers(1, &colourRbo);
  api.GenRenderbuffers(1, &pickingRbo);
  api.GenRenderbuffers(1, &depthRbo);

  api.BindFramebuffer(GL_FRAMEBUFFER, offscreenFbo);

  api.BindRenderbuffer(GL_RENDERBUFFER, colourRbo);
  api.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
  api.BindRenderbuffer(GL_RENDERBUFFER, pickingRbo);
  api.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA32UI, width, height);
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
  destroyPickingSlots();
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
  // Outstanding picking reads refer to the framebuffer being replaced, so they
  // are abandoned rather than answered against a target that no longer exists.
  destroyPickingSlots();
  createOffscreenTarget(width, height);
  createPickingSlots();
}

BufferHandle DeviceGL::createBuffer(const BufferKind kind,
                                    const std::size_t bytes) {
  BufferRecord record{};
  record.target = bufferTarget(kind);
  record.bytes  = bytes;
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

void DeviceGL::copyBufferRange(const BufferHandle buffer,
                               const std::size_t srcOffset,
                               const std::size_t dstOffset,
                               const std::size_t bytes) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceGL::copyBufferRange: unknown buffer");
  }
  if (0 == bytes) {
    return;
  }
  if (srcOffset + bytes > it->second.bytes ||
      dstOffset + bytes > it->second.bytes) {
    throw std::out_of_range("DeviceGL::copyBufferRange: past the end");
  }
  // One buffer bound to both slots is allowed as long as the ranges do not
  // overlap, which is why the caller must guarantee it: OpenGL raises
  // INVALID_VALUE rather than doing something sensible.
  if (srcOffset < dstOffset + bytes && dstOffset < srcOffset + bytes) {
    throw std::invalid_argument("DeviceGL::copyBufferRange: ranges overlap");
  }

  api.BindBuffer(GL_COPY_READ_BUFFER, it->second.name);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, it->second.name);
  api.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(srcOffset),
                        static_cast<GLintptr>(dstOffset),
                        static_cast<GLsizeiptr>(bytes));
  api.BindBuffer(GL_COPY_READ_BUFFER, 0);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
}

BufferHandle DeviceGL::resizeBuffer(const BufferHandle buffer,
                                    const std::size_t bytes) {
  const auto it = buffers.find(buffer.id);
  if (buffers.end() == it) {
    throw std::invalid_argument("DeviceGL::resizeBuffer: unknown buffer");
  }
  const BufferRecord old = it->second;
  if (bytes == old.bytes) {
    return buffer;
  }
  if (0 == bytes) {
    throw std::invalid_argument("DeviceGL::resizeBuffer: zero bytes");
  }

  BufferRecord grown{};
  grown.target = old.target;
  grown.bytes  = bytes;
  api.GenBuffers(1, &grown.name);
  api.BindBuffer(grown.target, grown.name);
  api.BufferData(grown.target, static_cast<GLsizeiptr>(bytes), nullptr,
                 GL_DYNAMIC_DRAW);

  // GL_COPY_READ_BUFFER and GL_COPY_WRITE_BUFFER are plain binding slots with
  // no semantic meaning, which is what makes a copy between two buffers of the
  // same kind possible without disturbing the array binding.
  api.BindBuffer(GL_COPY_READ_BUFFER, old.name);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, grown.name);
  // Whichever is smaller: growing carries everything forward, shrinking
  // carries forward what still fits and drops the rest, which the caller has
  // said nothing is using.
  api.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0,
                        static_cast<GLsizeiptr>(std::min(old.bytes, bytes)));
  api.BindBuffer(GL_COPY_READ_BUFFER, 0);
  api.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
  api.BindBuffer(grown.target, 0);

  api.DeleteBuffers(1, &old.name);
  it->second = grown;
  return buffer;
}

TextureHandle DeviceGL::createTextureArray(const int size, const int layers,
                                           const TextureFormat format,
                                           const int levels) {
  if (TextureFormat::R8 != format) {
    throw std::invalid_argument("DeviceGL: unsupported texture format");
  }

  TextureRecord record{};
  record.size   = size;
  record.layers = layers;
  record.levels = std::max(1, levels);
  api.GenTextures(1, &record.name);
  api.ActiveTexture(GL_TEXTURE0);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, record.name);
  // Trilinear when the glyph is smaller on screen than in the atlas, which is
  // the case this exists for, and plain linear when it is larger: there is no
  // level above zero to blend towards.
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER,
                    1 < record.levels ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // The chain stops where the atlas runs out of gutter between glyphs, not
  // where the texture runs out of pixels, so the cap is the caller's.
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
  api.TexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL,
                    record.levels - 1);
  // Every level is allocated up front and zeroed. Undefined contents would be
  // sampled the moment a glyph is minified, before anything has generated the
  // chain, and would be whatever the driver left in memory.
  for (int level = 0; level < record.levels; level++) {
    const auto extent = std::max(1, size >> level);
    const std::vector<std::uint8_t> zeros(
        static_cast<std::size_t>(extent) * extent * layers, 0);
    api.TexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_R8, extent, extent, layers, 0,
                   GL_RED, GL_UNSIGNED_BYTE, zeros.data());
  }
  api.BindTexture(GL_TEXTURE_2D_ARRAY, 0);
  diagnostics.raiseIfError("creating the glyph atlas");

  const TextureHandle handle{nextHandleId++};
  textures.emplace(handle.id, record);
  return handle;
}

void DeviceGL::generateMipmaps(const TextureHandle texture) {
  const auto it = textures.find(texture.id);
  if (textures.end() == it || 1 >= it->second.levels) {
    return;
  }
  api.ActiveTexture(GL_TEXTURE0);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, it->second.name);
  api.GenerateMipmap(GL_TEXTURE_2D_ARRAY);
  api.BindTexture(GL_TEXTURE_2D_ARRAY, 0);
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
    throw std::invalid_argument(
        "DeviceGL::updateTextureLayer: unknown texture");
  }
  const auto expected =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
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
    std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1),
                    '\0');
    api.GetShaderInfoLog(shader, logLength, nullptr, log.data());
    api.DeleteShader(shader);
    throw std::runtime_error(std::format(
        "Failed to compile {}:\n{}\nSource:\n{}", name, log, source));
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
  const GLuint fragmentShader = compileStage(GL_FRAGMENT_SHADER, fragmentSource,
                                             desc.name + " fragment stage");

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
    std::string log(static_cast<std::size_t>(logLength > 0 ? logLength : 1),
                    '\0');
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

  record.layout      = desc.layout;
  record.depthTest   = desc.depthTest;
  record.mvpLoc      = api.GetUniformLocation(record.program, "uMVP");
  record.opacityLoc  = api.GetUniformLocation(record.program, "uOpacity");
  record.identityLoc = api.GetUniformLocation(record.program, "uIdentity");
  record.atlasLoc    = api.GetUniformLocation(record.program, "uGlyphAtlas");

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
  // Debug output is synchronous, so anything the driver objected to during this
  // frame has already been recorded. Raise it here, from our own stack, rather
  // than from inside the driver callback where unwinding is undefined.
  diagnostics.raiseIfError("opengl driver reported an error");

  api.BindFramebuffer(GL_READ_FRAMEBUFFER, offscreenFbo);
  api.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
  api.ReadBuffer(GL_COLOR_ATTACHMENT0);
  api.BlitFramebuffer(0, 0, targetWidth, targetHeight, 0, 0, targetWidth,
                      targetHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  api.BindFramebuffer(GL_FRAMEBUFFER, 0);

  // The blit above is what fills the window's back buffer; the swap is only
  // what puts it in front of anybody. A capture reads the offscreen target, so
  // skipping the swap costs a caller that asked for it nothing.
  if (present && nullptr != targetWindow) {
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
  if (it->second.depthTest) {
    api.Enable(GL_DEPTH_TEST);
  } else {
    api.Disable(GL_DEPTH_TEST);
  }
  // Left on for every pipeline, because a draw that is not fading passes
  // opacity one and blends to exactly what it would have written unblended.
  // The picking attachment is an integer target, and the spec says blending is
  // not applied to those, so this cannot disturb a tag.
  api.Enable(GL_BLEND);
  api.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
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
  const auto count = std::min<std::size_t>(ranges.size(), maxHighlightRanges);
  api.BindBuffer(GL_UNIFORM_BUFFER, highlightUbo);
  if (0 != count) {
    api.BufferSubData(GL_UNIFORM_BUFFER, 0,
                      static_cast<GLsizeiptr>(count * sizeof(HighlightRange)),
                      ranges.data());
  }
  // The shader stops at the first zeroed entry, so one has to be written after
  // the last real range. Without it a shorter list than last frame's leaves the
  // tail of the old one in place -- clearing a selection left it on screen.
  if (count < static_cast<std::size_t>(maxHighlightRanges)) {
    const HighlightRange terminator{};
    api.BufferSubData(GL_UNIFORM_BUFFER,
                      static_cast<GLintptr>(count * sizeof(HighlightRange)),
                      static_cast<GLsizeiptr>(sizeof(HighlightRange)),
                      &terminator);
  }
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
  const auto bufferIt   = buffers.find(vertices.id);
  if (pipelines.end() == pipelineIt || buffers.end() == bufferIt) {
    throw std::invalid_argument(
        "DeviceGL::drawGlyphs: unknown pipeline or buffer");
  }
  const auto &record = pipelineIt->second;

  if (-1 != record.mvpLoc) {
    api.UniformMatrix4fv(record.mvpLoc, 1, GL_FALSE, uniforms.mvp.data());
  }
  if (-1 != record.opacityLoc) {
    api.Uniform1f(record.opacityLoc, uniforms.opacity);
  }
  if (-1 != record.identityLoc) {
    api.Uniform1ui(record.identityLoc, uniforms.identity);
  }

  // The draw starts partway into the vertex buffer. Baking that offset into
  // the attribute pointers works on OpenGL ES too, where the base-instance
  // draw entry points that would otherwise express it do not exist.
  api.BindBuffer(GL_ARRAY_BUFFER, bufferIt->second.name);
  for (const auto &attribute : record.layout.attributes) {
    const auto offset = vertexByteOffset + attribute.offset;
    api.EnableVertexAttribArray(attribute.location);
    if (AttributeType::UnsignedInt == attribute.type) {
      api.VertexAttribIPointer(attribute.location, attribute.components,
                               GL_UNSIGNED_INT,
                               static_cast<GLsizei>(record.layout.stride),
                               // NOLINTNEXTLINE(performance-no-int-to-ptr)
                               reinterpret_cast<const void *>(offset));
    } else {
      api.VertexAttribPointer(attribute.location, attribute.components,
                              GL_FLOAT, GL_FALSE,
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

/// Convert a top-down row, as the picking API and window coordinates use, to
/// the bottom-up row OpenGL addresses framebuffers with.
int DeviceGL::flipY(const int y) const { return targetHeight - 1 - y; }

void DeviceGL::createPickingSlots() {
  destroyPickingSlots();
  for (auto &slot : picking) {
    api.GenBuffers(1, &slot.pbo);
    api.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    // GL_STREAM_READ says the buffer is written by the GPU and read once by the
    // application, which is exactly this access pattern.
    api.BufferData(GL_PIXEL_PACK_BUFFER, pickingReadBytes, nullptr,
                   GL_STREAM_READ);
  }
  api.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

void DeviceGL::destroyPickingSlots() {
  for (auto &slot : picking) {
    if (nullptr != slot.fence) {
      api.DeleteSync(slot.fence);
    }
    if (0 != slot.pbo) {
      api.DeleteBuffers(1, &slot.pbo);
    }
    slot = PickingSlot{};
  }
  nextPickingSlot = 0;
}

void DeviceGL::requestPickingTag(const int x, const int y) {
  if (x < 0 || y < 0 || x >= targetWidth || y >= targetHeight) {
    return;
  }

  auto &slot = picking[nextPickingSlot];
  if (slot.pending) {
    // Every slot is already waiting on the GPU. Dropping the request keeps the
    // frame moving; the caller asks again next frame anyway.
    return;
  }

  api.BindFramebuffer(GL_READ_FRAMEBUFFER, offscreenFbo);
  api.ReadBuffer(GL_COLOR_ATTACHMENT1);
  api.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
  api.PixelStorei(GL_PACK_ALIGNMENT, 4);

  // With a pixel pack buffer bound the last argument is an offset into that
  // buffer rather than a client pointer, and the call returns without waiting
  // for the pixels.
  //
  // The read is four components even though the attachment has two: OpenGL ES
  // only guarantees GL_RGBA_INTEGER for an integer colour buffer, and desktop
  // GL accepts it too, so one format works on both.
  api.ReadPixels(x, flipY(y), 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);

  api.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);

  slot.fence      = api.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  slot.x          = x;
  slot.y          = y;
  slot.pending    = true;
  nextPickingSlot = (nextPickingSlot + 1) % picking.size();
}

std::optional<PickingResult> DeviceGL::takePickingTag() {
  // Slots are filled in order, so the oldest outstanding one is the slot after
  // the one that will be written next.
  for (std::size_t i = 0; i < picking.size(); i++) {
    auto &slot = picking[(nextPickingSlot + i) % picking.size()];
    if (!slot.pending) {
      continue;
    }
    if (nullptr == slot.fence) {
      slot.pending = false;
      continue;
    }

    // A zero timeout makes this a poll: either the read has landed or the
    // caller tries again next frame.
    const auto status = api.ClientWaitSync(slot.fence, 0, 0);
    if (GL_TIMEOUT_EXPIRED == status || GL_WAIT_FAILED == status) {
      continue;
    }

    api.DeleteSync(slot.fence);
    slot.fence   = nullptr;
    slot.pending = false;

    api.BindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
    const auto *mapped = static_cast<const GLuint *>(api.MapBufferRange(
        GL_PIXEL_PACK_BUFFER, 0, pickingReadBytes, GL_MAP_READ_BIT));
    PickingResult result{slot.x, slot.y, {}};
    if (nullptr != mapped) {
      result.tag = unpackPickingTag(mapped[0], mapped[1], mapped[2]);
      api.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    api.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return result;
  }
  return std::nullopt;
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
