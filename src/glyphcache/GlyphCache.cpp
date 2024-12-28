#include "GlyphCache.hpp"
#include <algorithm>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

// GlyphCache

inline int clampTextureSize(int size) { return std::min(2048, size); }
inline int clampTextureLayers(int layers) { return std::min(10, layers); }

GlyphCache::GlyphCache(std::shared_ptr<GL> &ogl) : gl{ogl} {
  gl->getIntegerv(GL_MAX_TEXTURE_SIZE, &size);
  logger->warn("max texture size: {} clamped: {}", size,
               clampTextureSize(size));
  size = clampTextureSize(size);
  gl->getIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
  logger->warn("max array texture layers: {} clamped: {}", maxLayers,
               clampTextureLayers(maxLayers));
  maxLayers = clampTextureLayers(maxLayers);
}

void GlyphCache::initTextureArray() {
  /*gl->pixelStorei(GL_UNPACK_ALIGNMENT, 1);
  gl->pixelStorei(GL_PACK_ALIGNMENT, 1);*/
  /*
  glGenTextures(1, &textArrId);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D_ARRAY, textArrId);

  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

#ifndef NDEBUG
  // blank the texture with white to make debugging easier, disable in prod code
  auto blankingDataSize = static_cast<long>(size * size) * maxLayers;
  auto *blankingData    = new unsigned char[blankingDataSize];

  std::fill(blankingData, &blankingData[blankingDataSize - 1], 255);

  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, size, size, maxLayers, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, blankingData);
#else  // NDEBUG defined
  glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8, size, size, maxLayers, 0, GL_BGRA,
               GL_UNSIGNED_BYTE, nullptr);
#endif // ifndef NDEBUG

  glBindTexture(GL_TEXTURE_2D_ARRAY, 0);*/
}

GlyphPalette &GlyphCache::getBestPalette(const Rect &charBox) {
  for (GlyphPalette &palette : palettes) {
    if (palette.canFit(charBox)) {
      return palette;
    }
  }
  if (palettes.size() >= (unsigned long)maxLayers) {
    throw new std::overflow_error("Out of Palettes!!!");
  }
  GlyphPalette &newPalette =
      palettes.emplace_back(Rect{Length{size}, Length{size}}, gl);
  std::sort(palettes.begin(), palettes.end());
  return newPalette;
}

Pango::Glyph GlyphCache::addToCache(const Glib::ustring &chr,
                                    const FontPtr &font) {
  /*auto desc = font->get_metrics();
  auto rect = desc.
  auto palette = getBestPalette(Rect{Length{0},Length{0}});*/
  return 0;
}

Pango::Glyph GlyphCache::put(const Glib::ustring &chr, const FontPtr &font) {
  /*if (auto pair = glyphs.find(chr); pair != glyphs.end()) {
    if (auto pair2 = pair->second.find(FontMapKeyAdapter{font});
        pair2 != pair->second.end()) {
      return pair2->second;
    }
  }
  return addToCache(chr, font);*/
  return 0;
}
