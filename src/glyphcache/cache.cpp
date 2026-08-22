/**
 * @file cache.cpp
 * @brief Implementation of the glyph cache: rasterization and texture packing.
 *
 * Implements GlyphCache helpers to rasterize text via FreeType/PangoFT2, manage
 * the device array texture, and pack glyphs into palettes and lanes.
 */
#include <gleditor/glyphcache/cache.hpp> // IWYU pragma: associated

#include <algorithm> // for min, sort
#include <cstddef>   // for byte
#include <cstdlib>   // for getenv
#include <format>
#include <ft2build.h>
#include <gleditor/glyphcache/palette.hpp> // for GlyphPalette, operator<=>
#include <gleditor/glyphcache/types.hpp>   // for TextureCoords, Rect
#include <gleditor/render/device.hpp>      // for RenderDevice
#include <hb.h>
#include <iostream>      // for basic_ostream, operator<<
#include <numeric>       // for format
#include <optional>      // for optional
#include <ranges>        // for find_if
#include <span>          // for span
#include <stdexcept>     // for invalid_argument, overflo...
#include <string>        // for char_traits, string, oper...
#include <string_view>   // for operator==, string_view
#include <tuple>         // for make_tuple, tuple
#include <unordered_map> // for unordered_map, operator==
#include <utility>       // for to_underlying, move
#include <vector>        // for vector
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_SYNTHESIS_H

namespace gleditor {

enum class Length : int;

// GlyphCache

namespace {

/**
 * @brief Side length the atlas starts at, and the layer count it starts with.
 *
 * Small on purpose. The atlas grows when a glyph will not fit, so the opening
 * size only has to hold the first few dozen clusters; sizing it for the worst
 * case instead meant reserving tens of megabytes for the hundred or so
 * distinct clusters a document of plain English actually uses. Growing is also
 * then exercised by every run rather than by nothing.
 */
constexpr int initialAtlasSize   = 512;
constexpr int initialAtlasLayers = 1;

/**
 * @brief The opening size, with GLEDITOR_ATLAS_SIZE allowed to override it.
 *
 * Growth is the sort of thing that is either exercised or merely believed in,
 * and no real document exercises it: the whole King James Bible packs into one
 * 512x512 layer. Starting small enough that ordinary text overflows is how the
 * grown atlas can be rendered and compared against the ungrown one -- see the
 * atlas-growth check in tools/compare-backends.sh.
 */
int openingAtlasSize() {
  const auto *requested = std::getenv("GLEDITOR_ATLAS_SIZE");
  if (nullptr == requested) {
    return initialAtlasSize;
  }
  try {
    return std::max(1, std::stoi(std::string(requested)));
  } catch (const std::exception &) {
    std::cerr << "GLEDITOR_ATLAS_SIZE is not a number, ignoring: " << requested
              << "\n";
  }
  return initialAtlasSize;
}

/**
 * @brief Mip levels the atlas carries, and the gutter that makes them safe.
 *
 * A mip texel at level L is the average of a 2^L block of level zero, aligned
 * to level zero's grid, so the texels covering a glyph reach up to 2^L - 1
 * texels outside it. Packed edge to edge, as glyphs were, that means level one
 * already averages a glyph with whatever was placed beside it. Each glyph
 * therefore sits inside a zeroed border wide enough for the deepest level.
 *
 * Four levels reach one eighth scale, which is a little past the point where
 * the coarse path takes the page over entirely (0.15 screen pixels per layout
 * pixel, see AppState::coarseBelow), so between them the two cover every size
 * a page is drawn at. Going deeper would quadruple the gutter for sizes
 * nothing ever samples.
 */
constexpr int atlasMipLevels = 4;
constexpr int glyphPadding   = 1 << (atlasMipLevels - 1);

struct PaddedCoverage {
  std::vector<std::byte> data;
  float meanInk{};
};

/**
 * @brief Convert a FreeType 8-bit grayscale bitmap directly to padded
 *        single-channel coverage in a single pass while computing mean ink.
 *
 * FreeType rasterizes directly into 8-bit coverage (FT_PIXEL_MODE_GRAY).
 * Texture rows are addressed from bottom to top, so we flip rows vertically
 * during the copy.
 */
PaddedCoverage
extractPaddedCoverage(const std::span<const unsigned char> surface,
                      const int width, const int height, const int stride) {
  const auto paddedWidth  = width + (2 * glyphPadding);
  const auto paddedHeight = height + (2 * glyphPadding);
  std::vector<std::byte> padded(static_cast<std::size_t>(paddedWidth) *
                                    static_cast<std::size_t>(paddedHeight),
                                std::byte{0});
  std::uint64_t totalInk = 0;
  for (int row = 0; row < height; row++) {
    const auto *src   = surface.data() + static_cast<std::size_t>(row) *
                                             static_cast<std::size_t>(stride);
    const auto dstRow = height - 1 - row;
    auto *dst =
        padded.data() +
        (static_cast<std::size_t>(dstRow + glyphPadding) * paddedWidth) +
        glyphPadding;
    for (int col = 0; col < width; col++) {
      const auto val = static_cast<std::byte>(src[col]);
      dst[col]       = val;
      totalInk += std::to_integer<std::uint64_t>(val);
    }
  }
  const auto totalPixels =
      static_cast<double>(width) * static_cast<double>(height);
  const auto meanInk = totalPixels > 0.0
                           ? static_cast<float>(static_cast<double>(totalInk) /
                                                (255.0 * totalPixels))
                           : 0.0F;
  return PaddedCoverage{std::move(padded), meanInk};
}

/// Paint rows [top, top + thickness) of a width * height, stride-wide 8-bit
/// coverage buffer solid, clamped to the buffer's own bounds. What underline,
/// overline and strikethrough are: none of the three has a glyph outline to
/// rasterise, only a bar at a computed height.
void fillHorizontalBar(std::vector<unsigned char> &data, const int width,
                       const int height, const int stride, const int top,
                       const int thickness) {
  const auto first = std::max(0, top);
  const auto last  = std::min(height, top + thickness);
  for (int row = first; row < last; row++) {
    auto *dstRow = data.data() + (static_cast<std::size_t>(row) * stride);
    std::fill(dstRow, dstRow + width, static_cast<unsigned char>(255));
  }
}

/// Which font to actually shape and rasterise a decorated cluster with, and
/// which of its decorations are still left to synthesise on top of it.
struct ResolvedFont {
  FontPtr font;
  std::unordered_set<Decoration> stillSynthetic;
};

/**
 * @brief Prefer a real bold and/or italic file over synthesising one, when
 *        @p decorations asks for either and the family @p font names has one.
 *
 * FontManager::getFont() resolves a fontconfig spec the same way the rest of
 * this codebase already asks for a family and a size -- "family size", with
 * style properties appended as ":bold" / ":italic" -- and fontconfig always
 * answers with its single closest match rather than refusing, so getting
 * something back is not proof it is actually bold or italic. FT_Face's own
 * style_flags is: it reports what the returned file's metadata really says,
 * which is checked here so a family with no true bold file does not get
 * silently rendered in a face that only claims to be one.
 *
 * A decoration whose real file was not found stays in stillSynthetic, to be
 * embolden'd or obliqued the way it always was. Bold and italic separately,
 * because a family can easily have one real file and not the other -- an
 * italic-only match still gets synthetic bold layered on top of it, rather
 * than losing the italic to fall back on synthesising both.
 */
ResolvedFont
resolveRealVariant(const FontPtr &font,
                   const std::unordered_set<Decoration> &decorations) {
  const bool wantBold   = decorations.contains(Decoration::Bold);
  const bool wantItalic = decorations.contains(Decoration::Italic);
  if (!wantBold && !wantItalic) {
    return {font, decorations};
  }

  auto spec = std::format("{} {:.1f}", font->family(), font->pointSize());
  if (wantBold) {
    spec += ":bold";
  }
  if (wantItalic) {
    spec += ":italic";
  }

  FontPtr candidate;
  try {
    candidate = text::FontManager::instance().getFont(spec);
  } catch (const std::exception &error) {
    std::cerr << "glyph cache: could not resolve \"" << spec
              << "\", synthesising instead: " << error.what() << "\n";
    return {font, decorations};
  }
  if (!candidate || nullptr == candidate->face()) {
    return {font, decorations};
  }

  const auto flags     = candidate->face()->style_flags;
  const bool gotBold   = wantBold && (0 != (flags & FT_STYLE_FLAG_BOLD));
  const bool gotItalic = wantItalic && (0 != (flags & FT_STYLE_FLAG_ITALIC));
  if (!gotBold && !gotItalic) {
    // Fontconfig matched something -- it always does -- but not a file that
    // actually carries either style bit asked for. Switching to it would
    // change nothing but the shaping metrics, to no benefit.
    return {font, decorations};
  }

  auto remaining = decorations;
  if (gotBold) {
    remaining.erase(Decoration::Bold);
  }
  if (gotItalic) {
    remaining.erase(Decoration::Italic);
  }
  return {candidate, std::move(remaining)};
}

} // namespace

GlyphCache::GlyphCache(render::RenderDevice *aDevice) : device(aDevice) {
  const auto limits = device->textureLimits();
  // The hardware's ceiling is the ceiling. A device that reports less than the
  // opening size gets a smaller opening size, not an allocation it cannot
  // make; the one thing not allowed is zero, which no device means literally.
  maxSize    = std::max(1, limits.maxSize);
  maxLayers  = std::min(maxEncodableLayers, std::max(1, limits.maxLayers));
  size       = std::min(openingAtlasSize(), maxSize);
  layerCount = std::min(initialAtlasLayers, maxLayers);
  std::cerr << std::format(
      "glyph cache: atlas {}x{} x{} layers, growing to at most {}x{} x{} "
      "(device allows {}x{} x{}, the vertex encoding {} layers)\n",
      size, size, layerCount, maxSize, maxSize, maxLayers, limits.maxSize,
      limits.maxSize, limits.maxLayers, maxEncodableLayers);

  texture = device->createTextureArray(
      size, layerCount, render::TextureFormat::R8, atlasMipLevels);
}

void GlyphCache::flush() {
  if (!atlasDirty || nullptr == device || !texture.valid()) {
    return;
  }
  atlasDirty = false;
  device->generateMipmaps(texture);
}

GlyphCache::~GlyphCache() {
  if (nullptr != device && texture.valid()) {
    device->destroyTexture(texture);
  }
}

auto GlyphCache::getBestPalette(const Rect &charBox) {
  const auto fits = [&charBox](GlyphPalette &pal) {
    return pal.canFit(charBox);
  };

  if (const auto it = std::ranges::find_if(palettes, fits);
      it != palettes.end()) {
    return it;
  }
  if (palettes.size() < static_cast<unsigned long>(layerCount)) {
    // Layers are handed out in creation order; the palette keeps its own index
    // so that sorting the vector cannot detach a palette from its layer.
    palettes.emplace_back(Rect{Length{size}, Length{size}}, device, texture,
                          static_cast<int>(palettes.size()));
    // The sort moves the new palette somewhere unpredictable -- palettes with
    // equal fill compare equivalent and std::sort is not stable -- so look it
    // up again rather than assuming it is still the last element.
    std::ranges::sort(palettes);
    const auto placed = std::ranges::find_if(palettes, fits);
    if (placed != palettes.end()) {
      return placed;
    }
  }
  return palettes.end();
}

void GlyphCache::reallocate(const int newSize, const int newLayers) {
  {
    std::size_t inked   = 0;
    std::size_t widest  = 0;
    std::size_t tallest = 0;
    for (const auto &placed : placements) {
      inked += static_cast<std::size_t>(placed.width) *
               static_cast<std::size_t>(placed.height);
      widest  = std::max<std::size_t>(widest, placed.width);
      tallest = std::max<std::size_t>(tallest, placed.height);
    }
    std::cerr << std::format(
        "glyph cache: atlas {}x{} x{} -> {}x{} x{} ({} glyphs, {} texels "
        "placed, largest {}x{}, {} palettes)\n",
        size, size, layerCount, newSize, newSize, newLayers, placements.size(),
        inked, widest, tallest, palettes.size());
  }

  // Nothing in flight may still be sampling the old texture. The device is
  // asked to settle before the handle it is reading from is destroyed.
  device->waitIdle();
  const auto old = texture;
  texture        = device->createTextureArray(
      newSize, newLayers, render::TextureFormat::R8, atlasMipLevels);
  size       = newSize;
  layerCount = newLayers;
  device->destroyTexture(old);

  // Existing palettes keep their layer and everything in it: lanes stack up
  // from y = 0 and fill right from x = 0, so a larger layer is room added
  // beyond what is used, never a shuffle of what is there.
  for (auto &palette : palettes) {
    palette.grow(Rect{Length{size}, Length{size}}, texture);
  }
  for (const auto &placed : placements) {
    device->updateTextureLayer(texture, placed.layer, placed.x, placed.y,
                               placed.width, placed.height, placed.coverage);
  }
  atlasDirty = true;
}

void GlyphCache::makeRoomFor(const Rect &padded) {
  const auto needed = std::max(std::to_underlying(padded.width),
                               std::to_underlying(padded.height));
  while (true) {
    // A glyph wider or taller than a whole layer can only be housed by a
    // larger layer, however many layers there are.
    if (needed <= size && palettes.end() != getBestPalette(padded)) {
      return;
    }
    // Size first: a layer twice as wide holds four times as much, where a
    // second layer only doubles it, and layers are the scarcer resource --
    // the vertex encoding allows a few dozen and the hardware allows a
    // texture thousands of pixels across.
    if (size < maxSize) {
      reallocate(std::min(maxSize, size * 2), layerCount);
      continue;
    }
    // Only when the glyph would fit a layer. A glyph too big for one is too
    // big for a hundred, and reallocating anyway would copy the whole atlas
    // into a larger one to no purpose before failing regardless.
    if (needed <= size && layerCount < maxLayers) {
      reallocate(size, std::min(maxLayers, layerCount * 2));
      continue;
    }
    throw std::overflow_error(std::format(
        "GlyphCache: the atlas is full at {}x{} across {} layers, and a "
        "{}x{} glyph will not fit",
        size, size, layerCount, std::to_underlying(padded.width),
        std::to_underlying(padded.height)));
  }
}

GlyphCache::Sizes
GlyphCache::addToCache(const std::string &chr, const FontPtr &font,
                       const std::unordered_set<Decoration> &decorations) {
  if (!font || chr.empty()) {
    const auto empty =
        Sizes{TextureCoords{}, Rect{Length{0}, Length{0}}, 0, 0.0F};
    glyphs[chr][keyFor(font)].push_back(DecoratedSizes{decorations, empty});
    return empty;
  }

  // Shaped and rasterised with whichever font actually carries the requested
  // decorations, which is not always the one requested: see
  // resolveRealVariant(). The cache is still keyed on the caller's own font
  // (keyFor(font) below, untouched) -- which face ended up doing the
  // rasterising is this function's own business, not part of what a caller
  // asked to look up.
  const auto resolved = resolveRealVariant(font, decorations);
  const auto &useFont = resolved.font;

  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, chr.data(), static_cast<int>(chr.size()), 0,
                     static_cast<int>(chr.size()));
  hb_buffer_guess_segment_properties(buf);
  hb_shape(useFont->hbFont(), buf, nullptr, 0);

  unsigned int glyphCount  = 0;
  hb_glyph_info_t *info    = hb_buffer_get_glyph_infos(buf, &glyphCount);
  hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &glyphCount);

  if (0 == glyphCount) {
    hb_buffer_destroy(buf);
    const auto empty =
        Sizes{TextureCoords{}, Rect{Length{0}, Length{0}}, 0, 0.0F};
    glyphs[chr][keyFor(font)].push_back(DecoratedSizes{decorations, empty});
    return empty;
  }

  FT_Face face = useFont->face();
  int penX     = 0;
  int minX = 0, maxX = 0;
  int minY = 0, maxY = 0;

  struct RenderedGlyph {
    FT_BitmapGlyph bitmapGlyph{};
    int x{};
    int y{};
  };
  std::vector<RenderedGlyph> rendered;
  rendered.reserve(glyphCount);

  for (unsigned int i = 0; i < glyphCount; i++) {
    if (FT_Load_Glyph(face, info[i].codepoint, FT_LOAD_DEFAULT) != 0) {
      continue;
    }
    // Only for whichever of bold/italic resolveRealVariant() could not find
    // a genuine file for -- useFont may already be a real bold and/or
    // italic face, in which case its own outline needs no help. These act
    // on the live outline in the slot, so they have to run before
    // FT_Get_Glyph copies it out and before FT_Glyph_To_Bitmap rasterises it
    // -- afterwards there is no outline left to embolden or slant.
    if (resolved.stillSynthetic.contains(Decoration::Bold)) {
      FT_GlyphSlot_Embolden(face->glyph);
    }
    if (resolved.stillSynthetic.contains(Decoration::Italic)) {
      FT_GlyphSlot_Oblique(face->glyph);
    }
    FT_Glyph glyph = nullptr;
    if (FT_Get_Glyph(face->glyph, &glyph) != 0) {
      continue;
    }
    if (FT_Glyph_To_Bitmap(&glyph, FT_RENDER_MODE_NORMAL, nullptr, 1) != 0) {
      FT_Done_Glyph(glyph);
      continue;
    }
    auto bg    = reinterpret_cast<FT_BitmapGlyph>(glyph);
    int gx     = penX + (pos[i].x_offset >> 6) + bg->left;
    int gy     = (pos[i].y_offset >> 6) + bg->top;
    int right  = gx + static_cast<int>(bg->bitmap.width);
    int bottom = gy - static_cast<int>(bg->bitmap.rows);

    minX = rendered.empty() ? gx : std::min(minX, gx);
    maxX = rendered.empty() ? right : std::max(maxX, right);
    maxY = rendered.empty() ? gy : std::max(maxY, gy);
    minY = rendered.empty() ? bottom : std::min(minY, bottom);

    rendered.push_back({bg, gx, gy});
    penX += (pos[i].x_advance >> 6);
  }
  hb_buffer_destroy(buf);

  const auto &metrics = useFont->metrics();
  const int height =
      std::max(1, static_cast<int>(std::ceil(metrics.lineHeight)));
  const int baselineY = static_cast<int>(std::round(metrics.ascent));

  int width = penX;
  if (!rendered.empty()) {
    if (minX < 0) {
      width = std::max(penX - minX, maxX - minX);
    } else {
      width = std::max(penX, maxX);
    }
  }
  if (width <= 0) {
    width = penX > 0 ? penX : 0;
  }

  const auto extents = Rect{Length{width}, Length{height}};
  if (0 == width || rendered.empty()) {
    for (auto &r : rendered) {
      FT_Done_Glyph(reinterpret_cast<FT_Glyph>(r.bitmapGlyph));
    }
    const auto empty = Sizes{TextureCoords{}, extents, 0, 0.0F};
    glyphs[chr][keyFor(font)].push_back(DecoratedSizes{decorations, empty});
    return empty;
  }

  const int stride = width;
  std::vector<unsigned char> data(
      static_cast<std::size_t>(height) * static_cast<std::size_t>(stride), 0);

  const int xShift = minX < 0 ? -minX : 0;
  for (auto &r : rendered) {
    auto bg  = r.bitmapGlyph;
    int dstX = r.x + xShift;
    int dstY = baselineY - r.y;

    for (unsigned int row = 0; row < bg->bitmap.rows; row++) {
      int curDstY = dstY + static_cast<int>(row);
      if (curDstY < 0 || curDstY >= height) {
        continue;
      }
      const unsigned char *srcRow =
          bg->bitmap.buffer + (row * bg->bitmap.pitch);
      unsigned char *dstRow =
          data.data() + (static_cast<std::size_t>(curDstY) * stride);

      for (unsigned int col = 0; col < bg->bitmap.width; col++) {
        int curDstX = dstX + static_cast<int>(col);
        if (curDstX < 0 || curDstX >= width) {
          continue;
        }
        dstRow[curDstX] = static_cast<unsigned char>(
            std::min(255, dstRow[curDstX] + srcRow[col]));
      }
    }
    FT_Done_Glyph(reinterpret_cast<FT_Glyph>(bg));
  }

  // Underline, overline and strikethrough have no outline of their own --
  // they are a bar composited straight onto the assembled bitmap, the same
  // one glyph after glyph was composited onto above, so a decorated cluster
  // still rasterises as one quad rather than a glyph plus a separate
  // rectangle some other draw call would have to align underneath it.
  if (!decorations.empty()) {
    // Font-unit metrics scaled to the pixels this face is currently sized
    // for, the same FT_MulFix(value, y_scale) idiom FreeType's own docs use
    // to scale face->ascender -- and >> 6 rather than a rounding macro, to
    // match every other 26.6 value already truncated that way in this
    // function (pos[i].x_advance and friends, above).
    const auto scaleToPixels = [face](const FT_Short value) {
      return static_cast<int>(FT_MulFix(value, face->size->metrics.y_scale) >>
                              6);
    };
    // usually negative: this is often 0 for a font that never set it.
    const int thickness = std::max(1, scaleToPixels(face->underline_thickness));

    if (decorations.contains(Decoration::Underline)) {
      // underline_position is negative (below the baseline), so subtracting
      // it moves down from baselineY -- see the dstY = baselineY - r.y
      // convention above, where a larger row index is further down.
      const int underlineY =
          baselineY - scaleToPixels(face->underline_position);
      fillHorizontalBar(data, width, height, stride, underlineY, thickness);
    }
    if (decorations.contains(Decoration::Strikethrough)) {
      // No FreeType metric names this position at all. A third of the
      // ascent above the baseline is the approximation most renderers fall
      // back to in the same absence, and it passes through the middle of
      // ordinary lowercase letters at typical proportions.
      const int strikeY =
          baselineY -
          static_cast<int>(static_cast<float>(metrics.ascent) * 0.3F);
      fillHorizontalBar(data, width, height, stride, strikeY, thickness);
    }
    if (decorations.contains(Decoration::Overline)) {
      // Row 0 is already the top of the ascent -- baselineY is round(ascent)
      // pixels down from it, by construction of baselineY above -- so there
      // is no headroom in this buffer to sit an overline any higher than
      // that; pinned to the very top edge instead of computed further above
      // it, which is what put a first attempt here at a negative row and
      // saw it clamped away entirely.
      const int overlineY = 0;
      fillHorizontalBar(data, width, height, stride, overlineY, thickness);
    }
  }

  // The glyph goes into the atlas inside a zeroed border, so that the mip
  // chain averages it with empty space rather than with its neighbour. The
  // palette packs the padded box and knows nothing about the padding; the
  // texture coordinates handed back to the shader are narrowed to the glyph
  // itself below, so nothing downstream sees the border either.
  const auto padded = Rect{Length{width + (2 * glyphPadding)},
                           Length{height + (2 * glyphPadding)}};
  makeRoomFor(padded);
  const auto palette = getBestPalette(padded);
  if (palettes.end() == palette) {
    throw std::overflow_error(
        std::format("GlyphCache: no palette has room for glyph: {}", chr));
  }
  auto paddedCoverage = extractPaddedCoverage(data, width, height, stride);
  const auto placed   = palette->put(padded, paddedCoverage.data);
  if (!placed.has_value()) {
    throw std::overflow_error(
        std::format("GlyphCache: failed to place glyph: {}", chr));
  }
  const auto inked = paddedCoverage.meanInk;
  // Remembered so that growing the atlas can put it back exactly here.
  placements.push_back(Placement{
      palette->layerIndex(), static_cast<int>(placed->topLeft.x),
      static_cast<int>(placed->topLeft.y), std::to_underlying(padded.width),
      std::to_underlying(padded.height), std::move(paddedCoverage.data)});

  // Narrow the placed rectangle from the padded box to the glyph inside it.
  // Texels, so that growing the atlas leaves this glyph where it is; the
  // shader divides by the texture's own size when it samples.
  const auto inner =
      TextureCoords{PointF{placed->topLeft.x + glyphPadding,
                           placed->topLeft.y + glyphPadding},
                    RectF{placed->box.width - (2 * glyphPadding),
                          placed->box.height - (2 * glyphPadding)}};

  const auto sizes = Sizes{inner, extents, palette->layerIndex(), inked};
  // Level zero moved, so the chain below it is stale until it is rebuilt.
  atlasDirty = true;
  glyphs[chr][keyFor(font)].push_back(DecoratedSizes{decorations, sizes});
  return sizes;
}

const FontMapKeyAdapter &GlyphCache::keyFor(const FontPtr &font) {
  const auto found = fontKeys.find(font.get());
  if (found != fontKeys.end()) {
    return found->second;
  }
  return fontKeys.emplace(font.get(), FontMapKeyAdapter(font)).first->second;
}

GlyphCache::Sizes
GlyphCache::put(const std::string_view &chr, const FontPtr &font,
                const std::unordered_set<Decoration> &decorations) {
  // A whole shaped cluster is cached, not a single codepoint: a ligature or a
  // base letter with its combining marks is one quad covering several
  // characters, and rasterising only the first of them dropped the rest from
  // the page. The bound is generous enough for emoji sequences joined by
  // zero-width joiners, and exists only so that a pathological run cannot
  // become a cache key.
  if (chr.size() > maxClusterBytes) {
    throw std::invalid_argument(
        std::format("GlyphCache: cluster of {} bytes exceeds the {}-byte limit",
                    chr.size(), maxClusterBytes));
  }
  if (const auto &chrToFontMap = glyphs.find(chr);
      chrToFontMap != glyphs.cend()) {
    if (const auto &variants = chrToFontMap->second.find(keyFor(font));
        variants != chrToFontMap->second.cend()) {
      // Size first, so the common case of "no decorations at all" on either
      // side is settled without comparing a single element.
      for (const auto &variant : variants->second) {
        if (variant.decorations.size() == decorations.size() &&
            variant.decorations == decorations) {
          return variant.sizes;
        }
      }
    }
  }
  return addToCache(std::string{chr}, font, decorations);
}

} // namespace gleditor
