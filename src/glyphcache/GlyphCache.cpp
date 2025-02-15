#include "GlyphCache.hpp"
#include "cairomm/enums.h"
#include "cairomm/fontoptions.h"
#include "types.hpp"
#include <algorithm>
#include <cairomm/context.h>
#include <cairomm/surface.h>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <iterator>
#include <optional>
#include <pango/pangocairo.h>
#include <pangomm.h>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// GlyphCache

inline int clampTextureSize(int size) { return std::min(2048, size); }
inline int clampTextureLayers(int layers) { return std::min(10, layers); }

GlyphCache::GlyphCache(std::shared_ptr<GL> &ogl) : gl{ogl} {
  gl->getIntegerv(GL_MAX_TEXTURE_SIZE, &size);
  std::cerr << "max texture size: " << size << "\n";
  size = clampTextureSize(size);
  std::cerr << "clamped texture size: " << size << "\n";
  gl->getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
  std::cerr << "max layers: " << maxLayers << "\n";
  maxLayers = clampTextureLayers(maxLayers);
  std::cerr << "clamped layers: " << maxLayers << "\n";
  initTextureArray();
}

void GlyphCache::initTextureArray() {
  /*gl->pixelStorei(GL_UNPACK_ALIGNMENT, 1);
  gl->pixelStorei(GL_PACK_ALIGNMENT, 1);*/
  glGenTextures(1, &textArrId);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, textArrId);

  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#if 0
//#ifndef NDEBUG
  // blank the texture with white to make debugging easier, disable in prod code
  auto blankingDataSize =
      static_cast<long>(size * size) * sizeof(int) * maxLayers;
  std::vector<unsigned char> blankingData(blankingDataSize);

  std::ranges::fill(blankingData, 255);

  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, size, size, maxLayers, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, blankingData.data());
#else // NDEBUG defined
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, size, size, maxLayers, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, nullptr);
#endif // ifndef NDEBUG

  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

auto GlyphCache::getBestPalette(const Rect &charBox) {
  for (auto it = palettes.begin(); it != palettes.end(); it++) {
    if (it->canFit(charBox)) {
      return it;
    }
  }
  if (palettes.size() >= (unsigned long)maxLayers) {
    throw new std::overflow_error("Out of Palettes!!!");
  }
  std::cout << "creating palette\n";
  palettes.emplace_back(Rect{Length{size}, Length{size}}, gl);
  std::sort(palettes.begin(), palettes.end());
  return std::prev(palettes.end());
}

inline Glib::RefPtr<Pango::Layout> getLayout(const Glib::ustring &chr,
                                             const FontPtr &font,
                                             Cairo::Surface::Format format) {
  auto tempSurf = Cairo::ImageSurface::create(format, 0, 0);
  auto ctx      = Cairo::Context::create(tempSurf);
  auto layout   = Pango::Layout::create(ctx);
  auto desc     = font->describe_with_absolute_size();
  layout->set_font_description(desc);
  layout->set_text(chr);
  return layout;
}

inline std::tuple<int, int, int>
getLayoutInfo(Glib::RefPtr<Pango::Layout> &layout,
              Cairo::Surface::Format format) {
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

GlyphCache::Sizes GlyphCache::addToCache(const Glib::ustring &chr,
                                         const FontPtr &font) {
  auto format = Cairo::Surface::Format::ARGB32;
  auto layout = getLayout(chr, font, format);

  auto [width, height, stride] = getLayoutInfo(layout, format);
  std::cout << std::format("layout w/h/s: {}/{}/{}\n", width, height, stride);
  auto extents                 = Rect{Length{width}, Length{height}};

  // create layout drawing context
  std::vector<unsigned char> data(static_cast<long>(height) * stride);
  auto layoutSurf =
      Cairo::ImageSurface::create(data.data(), format, width, height, stride);
  auto layCtx = Cairo::Context::create(layoutSurf);

  // clear surface
  layCtx->set_source_rgba(0, 0, 0, 0);
  layCtx->rectangle(0, 0, width, height);
  layCtx->fill();

  // flip the image vertically, texSubImage always draws from bottom to top
  Cairo::Matrix matrix(1.0, 0.0, 0.0, -1.0, 0.0, height);
  layCtx->transform(matrix);
  // draw text cluster
  layCtx->set_source_rgba(1, 0, 0, 1);
  layCtx->set_font_options(getFontOptions());
  layout->show_in_cairo_context(layCtx);

  // debugging
  //layoutSurf->write_to_png("/tmp/page.png");

  auto palette = getBestPalette(extents);
  glActiveTexture(GL_TEXTURE0);
  bindTexture();
  auto coords = glyphs[chr][font] =
      GlyphCache::Sizes{palette->put(extents, data.data()).value(), extents};
  clearTexture();
  return coords;
}

GlyphCache::Sizes GlyphCache::put(const Glib::ustring &chr,
                                  const FontPtr &font) {
  if (auto pair = glyphs.find(chr); pair != glyphs.end()) {
    if (auto pair2 = pair->second.find(FontMapKeyAdapter{font});
        pair2 != pair->second.end()) {
      return pair2->second;
    }
  }
  return addToCache(chr, font);
}
