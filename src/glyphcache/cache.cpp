/**
 * @file cache.cpp
 * @brief Implementation of the glyph cache: rasterization and texture packing.
 *
 * Implements GlyphCache helpers to rasterize text via Pango/Cairo, manage the
 * device array texture, and pack glyphs into palettes and lanes.
 */
#include <gleditor/glyphcache/cache.hpp> // IWYU pragma: associated

#include <algorithm>                       // for min, sort
#include <cairomm/context.h>               // for Context
#include <cairomm/surface.h>               // for ImageSurface, Surface
#include <cstddef>                         // for byte
#include <format>                          // for format
#include <gleditor/glyphcache/palette.hpp> // for GlyphPalette, operator<=>
#include <gleditor/glyphcache/types.hpp>   // for TextureCoords, Rect
#include <gleditor/render/device.hpp>      // for RenderDevice
#include <iostream>                        // for basic_ostream, operator<<
#include <memory>                          // for shared_ptr
#include <optional>                        // for optional
#include <ranges>                          // for find_if
#include <span>                            // for span
#include <stdexcept>                       // for invalid_argument, overflo...
#include <string>                          // for char_traits, string, oper...
#include <string_view>                     // for operator==, string_view
#include <tuple>                           // for make_tuple, tuple
#include <unordered_map>                   // for unordered_map, operator==
#include <utility>                         // for pair
#include <vector>                          // for vector

#include "cairomm/enums.h"       // for ANTIALIAS_SUBPIXEL, Antia...
#include "cairomm/fontoptions.h" // for FontOptions
#include "cairomm/matrix.h"      // for Matrix
#include "glibmm/refptr.h"       // for RefPtr
#include "pangomm/font.h"        // for Font
#include "pangomm/layout.h"      // for Layout

enum class Length : int;

// GlyphCache

namespace {

/// Upper bound on the atlas side length. Larger atlases waste memory for the
/// handful of layers the cache actually fills.
int clampTextureSize(const int size) { return std::min(2048, size); }
/// Upper bound on atlas layers, which also bounds the 4-bit layer field packed
/// into Doc::VBORow::layerWidthHeight.
int clampTextureLayers(const int layers) { return std::min(10, layers); }

/**
 * @brief Convert a Cairo ARGB32 surface to tightly packed single-channel
 *        coverage.
 *
 * Every backend can upload an R8 rectangle identically, whereas asking the
 * driver to derive one channel from a BGRA upload is an OpenGL-specific
 * convenience that Vulkan has no equivalent for. Doing the narrowing here
 * keeps the device interface honest and the upload path the same everywhere.
 *
 * The glyph is drawn in opaque red on a transparent background, so in Cairo's
 * premultiplied ARGB32 the alpha byte already holds the coverage. On a
 * little-endian host the bytes of each pixel are ordered B, G, R, A.
 */
std::vector<std::byte> toCoverage(const std::span<const unsigned char> surface,
                                  const int width, const int height,
                                  const int stride) {
  std::vector<std::byte> coverage(static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height));
  for (int row = 0; row < height; row++) {
    const auto *src = surface.data() + static_cast<std::size_t>(row) *
                                           static_cast<std::size_t>(stride);
    auto *dst = coverage.data() + static_cast<std::size_t>(row) *
                                      static_cast<std::size_t>(width);
    for (int col = 0; col < width; col++) {
      dst[col] = static_cast<std::byte>(src[(col * 4) + 3]);
    }
  }
  return coverage;
}

} // namespace

GlyphCache::GlyphCache(render::RenderDevice *aDevice) : device(aDevice) {
  const auto limits = device->textureLimits();
  size              = clampTextureSize(limits.maxSize);
  maxLayers         = clampTextureLayers(limits.maxLayers);
  std::cerr << std::format(
      "glyph cache: atlas {}x{} (device max {}), {} layers (device max {})\n",
      size, size, limits.maxSize, maxLayers, limits.maxLayers);

  texture = device->createTextureArray(size, maxLayers,
                                       render::TextureFormat::R8);
}

GlyphCache::~GlyphCache() {
  if (nullptr != device && texture.valid()) {
    device->destroyTexture(texture);
  }
}

auto GlyphCache::getBestPalette(const Rect &charBox) {
  const auto fits = [&charBox](GlyphPalette &pal) { return pal.canFit(charBox); };

  if (const auto it = std::ranges::find_if(palettes, fits);
      it != palettes.end()) {
    return it;
  }
  if (palettes.size() >= static_cast<unsigned long>(maxLayers)) {
    throw std::overflow_error("Out of Palettes!!!");
  }
  // Layers are handed out in creation order; the palette keeps its own index
  // so that sorting the vector cannot detach a palette from its layer.
  palettes.emplace_back(Rect{Length{size}, Length{size}}, device, texture,
                        static_cast<int>(palettes.size()));
  // The sort moves the new palette somewhere unpredictable -- palettes with
  // equal fill compare equivalent and std::sort is not stable -- so look it up
  // again rather than assuming it is still the last element.
  std::ranges::sort(palettes);
  const auto placed = std::ranges::find_if(palettes, fits);
  if (placed == palettes.end()) {
    throw std::overflow_error("Glyph too large for an empty palette");
  }
  return placed;
}

inline Glib::RefPtr<Pango::Layout>
getLayout(const std::string &chr, const FontPtr &font,
          const Cairo::Surface::Format format) {
  const auto tempSurf = Cairo::ImageSurface::create(format, 0, 0);
  const auto ctx      = Cairo::Context::create(tempSurf);
  auto layout         = Pango::Layout::create(ctx);
  const auto desc     = font->describe_with_absolute_size();
  layout->set_font_description(desc);
  layout->set_text(chr);
  return layout;
}

inline std::tuple<int, int, int>
getLayoutInfo(const Glib::RefPtr<Pango::Layout> &layout,
              const Cairo::Surface::Format format) {
  int width;
  int height;
  layout->get_pixel_size(width, height);
  int stride = Cairo::ImageSurface::format_stride_for_width(format, width);
  return std::make_tuple(width, height, stride);
}

inline auto getFontOptions() {
  Cairo::FontOptions opts;
  opts.set_antialias(Cairo::Antialias::ANTIALIAS_SUBPIXEL);
  opts.set_hint_metrics(Cairo::FontOptions::HintMetrics::ON);
  opts.set_hint_style(Cairo::FontOptions::HintStyle::FULL);
  return opts;
}

GlyphCache::Sizes GlyphCache::addToCache(const std::string &chr,
                                         const FontPtr &font) {
  constexpr auto format = Cairo::Surface::Format::ARGB32;
  const auto layout     = getLayout(chr, font, format);

  auto [width, height, stride] = getLayoutInfo(layout, format);
  const auto extents           = Rect{Length{width}, Length{height}};

  // A zero-area cluster -- an isolated newline, for instance -- has nothing to
  // rasterize, but still needs an entry so the caller can advance the pen.
  if (0 == width || 0 == height) {
    const auto empty = Sizes{TextureCoords{}, extents, 0};
    glyphs[chr][FontMapKeyAdapter(font)] = empty;
    return empty;
  }

  // create layout drawing context
  std::vector<unsigned char> data(static_cast<long>(height) * stride);
  const auto layoutSurf =
      Cairo::ImageSurface::create(data.data(), format, width, height, stride);
  const auto layCtx = Cairo::Context::create(layoutSurf);

  // clear surface
  layCtx->set_source_rgba(0, 0, 0, 0);
  layCtx->rectangle(0, 0, width, height);
  layCtx->fill();

  // Flip the image vertically: texture rows are addressed from the bottom up,
  // Cairo draws from the top down.
  const Cairo::Matrix matrix(1.0, 0.0, 0.0, -1.0, 0.0, height);
  layCtx->transform(matrix);
  // draw text cluster
  layCtx->set_source_rgba(1, 0, 0, 1);
  layCtx->set_font_options(getFontOptions());
  layout->show_in_cairo_context(layCtx);
  // Cairo may still be holding drawing operations; flush before reading back.
  layoutSurf->flush();

  const auto coverage = toCoverage(data, width, height, stride);

  const auto palette = getBestPalette(extents);
  const auto placed  = palette->put(extents, coverage);
  if (!placed.has_value()) {
    throw std::overflow_error(
        std::format("GlyphCache: failed to place glyph: {}", chr));
  }
  const auto sizes = Sizes{placed.value(), extents, palette->layerIndex()};
  glyphs[chr][FontMapKeyAdapter(font)] = sizes;
  return sizes;
}

GlyphCache::Sizes GlyphCache::put(const std::string_view &chr,
                                  const FontPtr &font) {
  // A single UTF-8 codepoint occupies at most four bytes; rejecting at three
  // turned every astral-plane character (emoji, CJK extensions, ...) into an
  // exception.
  if (chr.size() > 4) {
    throw std::invalid_argument(
        std::format("GlyphCache: bad character: {}", chr));
  }
  if (const auto &chrToFontMap = glyphs.find(chr);
      chrToFontMap != glyphs.cend()) {
    if (const auto &fontMapToGlyphSizes =
            chrToFontMap->second.find(FontMapKeyAdapter(font));
        fontMapToGlyphSizes != chrToFontMap->second.cend()) {
      return fontMapToGlyphSizes->second;
    }
  }
  return addToCache(std::string{chr}, font);
}
// vi: set sw=2 sts=2 ts=2 et:
