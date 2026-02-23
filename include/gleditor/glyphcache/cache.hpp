/**
 * @file cache.hpp
 * @brief Glyph cache that renders and packs glyphs into a GL texture array.
 *
 * Exposes a GlyphCache used by rendering code to request glyph texture
 * coordinates for character clusters in specific fonts. Internally, the cache
 * uses GlyphPalette and GlyphLane to bin-pack rectangles and maintains a
 * per-font map of cached entries.
 */
#ifndef GLEDITOR_GLYPH_CACHE_H
#define GLEDITOR_GLYPH_CACHE_H

#include <compare>
#include <cstddef>
#include <functional>
#include <memory>
#include <pangomm/font.h>

#include "glibmm/refptr.h"
#include "glibmm/ustring.h"
#include <gleditor/gl/gl.hpp>
#include <gleditor/glyphcache/palette.hpp>
#include <gleditor/glyphcache/types.hpp>
#include <gleditor/log.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * @brief Helper for creating an overload set of call operators.
 *
 * Useful together with std::variant visitation and transparent hashing to
 * combine multiple functors into one.
 */
template <typename... Bases> struct overload : Bases... {
  using is_transparent = void;
  using Bases::operator()...;
};

/**
 * @brief Transparent hasher for raw C string pointers.
 * Allows unordered_map with heterogeneous lookup using const char*.
 */
struct char_pointer_hash {
  auto operator()(const char *ptr) const noexcept {
    return std::hash<std::string_view>{}(ptr);
  }
};

/// Transparent string hasher enabling heterogeneous lookup on std::string, std::string_view, and const char*.
using transparent_string_hash =
    overload<std::hash<std::string>, std::hash<std::string_view>,
             char_pointer_hash>;

/// Shared pointer alias to a Pango font instance.
using FontPtr = Glib::RefPtr<Pango::Font>;

/**
 * @class FontMapKeyAdapter
 * @brief Compares Pango::Font objects by their fully described absolute size strings.
 *
 * Provides a strict-weak-ordering and hash based on the casefolded string
 * from Pango's describe_with_absolute_size(). Useful as a key in maps.
 */
class FontMapKeyAdapter {
private:
  FontPtr font_;

public:
  explicit FontMapKeyAdapter(FontPtr font) : font_(std::move(font)) {}
  FontMapKeyAdapter(const FontMapKeyAdapter &oth)            = default;
  FontMapKeyAdapter &operator=(const FontMapKeyAdapter &oth) = default;
  FontMapKeyAdapter(FontMapKeyAdapter &&oth)                 = default;
  FontMapKeyAdapter &operator=(FontMapKeyAdapter &&oth)      = default;

  /**
   * @brief Access the underlying Pango font reference.
   */
  [[nodiscard]] const FontPtr &font() const { return font_; }

  /**
   * @brief Equality based on casefolded absolute-size font description.
   */
  bool operator==(const FontMapKeyAdapter &oth) const {
    return (*this <=> oth) == std::partial_ordering::equivalent;
  }

  /**
   * @brief Three-way comparison using the casefolded font description.
   */
  std::partial_ordering operator<=>(const FontMapKeyAdapter &oth) const {
    const auto myStr = font_->describe_with_absolute_size().to_string().casefold();
    const auto othStr =
        oth.font_->describe_with_absolute_size().to_string().casefold();
    const auto cmp = myStr.compare(othStr);
    if (0 == cmp) {
      return std::partial_ordering::equivalent;
    }
    if (0 > cmp) {
      return std::partial_ordering::less;
    }
    return std::partial_ordering::greater;
  }
};

/**
 * @brief std::hash specialization for FontMapKeyAdapter based on font description.
 */
template <> struct std::hash<FontMapKeyAdapter> {
  std::size_t operator()(const FontMapKeyAdapter &adapter) const noexcept {
    return std::hash<std::string>{}(
        adapter.font()->describe_with_absolute_size().to_string().casefold());
  }
};

/**
 * @class GlyphCache
 * @brief Caches rendered glyphs into an OpenGL texture array and returns UVs.
 *
 * GlyphCache uses Pango/Cairo to rasterize text for a given Pango::Font and
 * packs the resulting rectangles into a layered texture via GlyphPalette. The
 * put() API returns normalized texture coordinates and pixel dimensions for
 * rendering.
 */
class GlyphCache : public Loggable {
public:
  /**
   * @brief Return values for a cached glyph.
   * texCoords are normalized UVs within the GL texture array; dims are the
   * pixel rectangle sizes used for placement and rendering calculations.
   */
  struct Sizes {
    TextureCoords texCoords; ///< Normalized UVs within the texture layer.
    Rect dims;               ///< Pixel dimensions of the rasterized glyph.
  };
  /**
   * @brief Construct the glyph cache and initialize the GL texture array.
   * @param ogl Shared GL wrapper used for querying limits and issuing commands.
   */
  explicit GlyphCache(const std::shared_ptr<GL> &ogl);
  ~GlyphCache() override = default;

  GlyphCache(GlyphCache &oth)           = delete;
  void operator=(const GlyphCache &oth) = delete;

  /**
   * @brief Retrieve or create a glyph entry for the given character and font.
   * @param chr UTF-8 sequence representing a single codepoint or small cluster.
   * @param font Loaded Pango font to use for rasterization.
   * @return Sizes with UVs and pixel dimensions.
   * @throws std::invalid_argument if the provided sequence is too long.
   */
  Sizes put(const std::string_view &chr, const FontPtr &font);
  /**
   * @brief Bind the internal texture array to an active texture unit.
   * @param active Active texture unit index (0-based).
   */
  void bindTexture(const int active) const {
    glActiveTexture(GL_TEXTURE0 + active);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textArrId);
  }
  /**
   * @brief Unbind any array texture from the active binding point.
   */
  static void clearTexture() { glBindTexture(GL_TEXTURE_2D_ARRAY, 0); }

private:
  std::vector<GlyphPalette> palettes; ///< Palette layers used for packing.
  std::unordered_map<std::string, std::unordered_map<FontMapKeyAdapter, Sizes>,
                     transparent_string_hash, std::equal_to<>>
      glyphs; ///< Map: character string -> (font -> cached sizes).
  std::shared_ptr<GL> gl; ///< GL wrapper.
  int size{}, maxLayers{};    ///< Texture side length and max array layers.
  GLuint textArrId{};       ///< GL texture array object id.

  /**
   * @brief Find or create a palette capable of fitting the given rectangle.
   */
  auto getBestPalette(const Rect &charBox);
  /**
   * @brief Initialize the GL texture array object.
   */
  void initTextureArray();
  /**
   * @brief Rasterize and pack a new glyph into the cache.
   */
  Sizes addToCache(const std::string &chr, const FontPtr &font);
};

#endif // GLEDITOR_GLYPH_CACHE_H
// vi: set sw=2 sts=2 ts=2 et:
