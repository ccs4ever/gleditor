/**
 * @file svg_cache.cpp
 * @brief Static SVG rasterization via ThorVG: a GL-native fast path for
 *        OpenGL/OpenGL ES (ThorVG's GlCanvas rendering straight into a
 *        texture this device owns) and a CPU fallback for every other
 *        backend (ThorVG's SwCanvas, uploaded the same way MediaWidget
 *        already uploads video frames).
 *
 * Gated by GLEDITOR_HAVE_SVG_THORVG (thorvg-1.pc), mirroring
 * decode_index.cpp's per-format optional-dependency shape: the #else branch
 * below stubs every entry point rather than leaving anything declared but
 * undefined, so a build without thorvg-1 still links and simply cannot show
 * SVG spans.
 */
#include <gleditor/svg_cache.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef GLEDITOR_HAVE_SVG_THORVG
#include <thorvg.h>

#include <gleditor/render/gl/device_gl.hpp>
#endif

namespace gleditor {

#ifdef GLEDITOR_HAVE_SVG_THORVG

namespace {

/// Reference-counted, matching every other user of tvg::Initializer::init():
/// more than one SvgCache (an image overlay per open document window, in
/// principle) must not tear the engine down while another is still using it.
std::mutex initMutex;
int initRefCount = 0;

void thorvgRef() {
  const std::lock_guard<std::mutex> lock(initMutex);
  if (0 == initRefCount++) {
    tvg::Initializer::init();
  }
}

void thorvgUnref() {
  const std::lock_guard<std::mutex> lock(initMutex);
  if (0 == --initRefCount) {
    tvg::Initializer::term();
  }
}

/**
 * @brief Rasterize @p bytes on the CPU and upload into layer 0 of @p tex.
 *
 * Parses its own Picture rather than sharing one with tryRenderGl(): ThorVG
 * hands Picture ownership to whichever Canvas it is added to (freed when
 * that canvas is), so the two paths sharing one Picture would mean tracking
 * whether the GL attempt already consumed it before falling back here. A
 * second cheap XML parse of a small SVG document is a simpler and safer
 * price than that bookkeeping.
 */
bool renderSw(render::RenderDevice *const device,
              const std::span<const std::uint8_t> bytes,
              const render::TextureHandle tex, const int width,
              const int height) {
  auto *const picture = tvg::Picture::gen();
  const auto loadRes  = picture->load(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::uint32_t>(bytes.size()), "svg+xml", nullptr, true);
  if (tvg::Result::Success != loadRes) {
    picture->unref();
    return false;
  }

  auto *const canvas = tvg::SwCanvas::gen();
  if (nullptr == canvas) {
    picture->unref();
    return false;
  }

  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * height,
                                    0);
  const auto targetRes = canvas->target(
      pixels.data(), static_cast<std::uint32_t>(width),
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
      tvg::ColorSpace::ABGR8888S);
  if (tvg::Result::Success != targetRes ||
      tvg::Result::Success != canvas->add(picture)) {
    delete canvas;
    picture->unref();
    return false;
  }
  // add() succeeded: picture is now the canvas's, freed when it is below.
  canvas->draw();
  canvas->sync();

  // ABGR8888S's in-memory byte order for a little-endian uint32_t is R, G,
  // B, A -- the same order DecodedImage::rgba and updateTextureLayer()
  // already expect (confirmed against a real rendered pixel, see the design
  // doc), so the buffer reinterprets directly with no channel shuffling.
  device->updateTextureLayer(
      tex, 0, 0, 0, width, height,
      std::span<const std::byte>(
          reinterpret_cast<const std::byte *>(pixels.data()),
          pixels.size() * sizeof(std::uint32_t)));

  delete canvas;
  return true;
}

/**
 * @brief Rasterize @p bytes straight into layer 0 of @p tex via ThorVG's
 *        GlCanvas, using DeviceGL's renderIntoTextureLayer() escape hatch.
 * @return false if @p device is not a DeviceGL, the SVG fails to parse, or
 *        the GL render itself fails for any reason -- the caller falls
 *        back to renderSw() in every one of those cases.
 */
bool tryRenderGl(render::RenderDevice *const device,
                 const std::span<const std::uint8_t> bytes,
                 const render::TextureHandle tex, const int texSize,
                 const int width, const int height) {
  auto *const gl = dynamic_cast<render::gl::DeviceGL *>(device);
  if (nullptr == gl) {
    return false;
  }

  auto *const picture = tvg::Picture::gen();
  const auto loadRes  = picture->load(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::uint32_t>(bytes.size()), "svg+xml", nullptr, true);
  if (tvg::Result::Success != loadRes) {
    picture->unref();
    return false;
  }

  bool consumed = false; // true once add() hands the picture to a GlCanvas
  bool rendered = false;
  gl->renderIntoTextureLayer(
      tex, 0, [&](const unsigned fbo, void *const glContext) {
        auto *const canvas = tvg::GlCanvas::gen();
        if (nullptr == canvas) {
          return;
        }
        // Null display/surface: per GlCanvas::target()'s own doc, that means
        // the GL context handed in is assumed already current rather than
        // something ThorVG must bind itself -- true here, since DeviceGL
        // made glContext current on this same (the render) thread and never
        // releases it. Confirmed against a real offscreen SDL/GL context
        // and framebuffer readback before this was wired in; see the design
        // doc.
        const auto targetRes = canvas->target(
            nullptr, nullptr, glContext, static_cast<std::int32_t>(fbo),
            texSize, texSize, tvg::ColorSpace::ABGR8888S);
        if (tvg::Result::Success != targetRes) {
          delete canvas;
          return;
        }
        canvas->viewport(0, 0, width, height);
        if (tvg::Result::Success != canvas->add(picture)) {
          delete canvas;
          return;
        }
        consumed = true;
        rendered = (tvg::Result::Success == canvas->draw());
        canvas->sync();
        delete canvas;
      });

  if (!consumed) {
    picture->unref();
  }
  return rendered;
}

} // namespace

SvgCache::SvgCache(render::RenderDevice *const device) : device_(device) {
  thorvgRef();
}

SvgCache::~SvgCache() {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (nullptr != device_) {
    for (const auto &[id, res] : cache_) {
      if (res.texture.valid()) {
        device_->destroyTexture(res.texture);
      }
    }
  }
  cache_.clear();
  thorvgUnref();
}

std::optional<std::pair<float, float>>
SvgCache::peekSize(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty()) {
    return std::nullopt;
  }
  auto *const picture = tvg::Picture::gen();
  const auto loadRes  = picture->load(
      reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::uint32_t>(bytes.size()), "svg+xml", nullptr, true);
  if (tvg::Result::Success != loadRes) {
    picture->unref();
    return std::nullopt;
  }
  float width  = 0.0F;
  float height = 0.0F;
  picture->size(&width, &height);
  picture->unref();
  if (width <= 0.0F || height <= 0.0F) {
    return std::nullopt;
  }
  return std::make_pair(width, height);
}

std::optional<ImageResource>
SvgCache::rasterize(const std::span<const std::uint8_t> bytes,
                    const float width, const float height) {
  const auto pixelW = static_cast<int>(std::lround(width));
  const auto pixelH = static_cast<int>(std::lround(height));
  if (pixelW <= 0 || pixelH <= 0 || nullptr == device_) {
    return std::nullopt;
  }
  const int texSize = std::max(pixelW, pixelH);

  const auto tex =
      device_->createTextureArray(texSize, 1, render::TextureFormat::RGBA8);
  if (!tex.valid()) {
    return std::nullopt;
  }

  bool rendered = false;
  if (render::Backend::OpenGL == device_->backend() ||
      render::Backend::OpenGLES == device_->backend()) {
    rendered = tryRenderGl(device_, bytes, tex, texSize, pixelW, pixelH);
  }
  if (!rendered) {
    rendered = renderSw(device_, bytes, tex, pixelW, pixelH);
  }
  if (!rendered) {
    device_->destroyTexture(tex);
    return std::nullopt;
  }

  const float texSizeF = static_cast<float>(texSize);
  return ImageResource{
      .id      = {}, // filled in by loadBuffer() once it knows the cache key
      .width   = pixelW,
      .height  = pixelH,
      .layer   = 0,
      .u0      = 0.0F,
      .v0      = 0.0F,
      .u1      = static_cast<float>(pixelW) / texSizeF,
      .v1      = static_cast<float>(pixelH) / texSizeF,
      .texture = tex,
  };
}

std::optional<ImageResource>
SvgCache::loadBuffer(const std::string &id,
                     const std::span<const std::uint8_t> bytes) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = cache_.find(id); cache_.end() != it) {
      return it->second;
    }
  }

  const auto size = peekSize(bytes);
  if (!size) {
    return std::nullopt;
  }
  auto resource = rasterize(bytes, size->first, size->second);
  if (!resource) {
    return std::nullopt;
  }
  resource->id = id;

  const std::lock_guard<std::mutex> lock(mutex_);
  // The render thread is the only caller (see design doc), so a race here
  // cannot actually happen -- guarded anyway rather than leaking a texture
  // if that ever changes.
  if (const auto it = cache_.find(id); cache_.end() != it) {
    device_->destroyTexture(resource->texture);
    return it->second;
  }
  cache_.emplace(id, *resource);
  return resource;
}

std::optional<ImageResource> SvgCache::find(const std::string &id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (const auto it = cache_.find(id); cache_.end() != it) {
    return it->second;
  }
  return std::nullopt;
}

#else // !GLEDITOR_HAVE_SVG_THORVG

SvgCache::SvgCache(render::RenderDevice *const device) : device_(device) {}
SvgCache::~SvgCache() = default;

std::optional<std::pair<float, float>>
SvgCache::peekSize(std::span<const std::uint8_t> /*bytes*/) {
  return std::nullopt;
}

std::optional<ImageResource>
SvgCache::rasterize(std::span<const std::uint8_t> /*bytes*/, float /*width*/,
                    float /*height*/) {
  return std::nullopt;
}

std::optional<ImageResource>
SvgCache::loadBuffer(const std::string & /*id*/,
                     std::span<const std::uint8_t> /*bytes*/) {
  return std::nullopt;
}

std::optional<ImageResource> SvgCache::find(const std::string & /*id*/) const {
  return std::nullopt;
}

#endif // GLEDITOR_HAVE_SVG_THORVG

} // namespace gleditor
