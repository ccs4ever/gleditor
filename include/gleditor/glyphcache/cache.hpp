/**
 * @file cache.hpp
 * @brief Glyph cache that renders and packs glyphs into a device array texture.
 *
 * Exposes a GlyphCache used by rendering code to request glyph texture
 * coordinates for character clusters in specific fonts. Internally, the cache
 * uses GlyphPalette and GlyphLane to bin-pack rectangles and maintains a
 * per-font map of cached entries. All device interaction goes through
 * RenderDevice, so the cache is the same on every backend.
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
#include <gleditor/glyphcache/palette.hpp>
#include <gleditor/glyphcache/types.hpp>
#include <gleditor/log.hpp>
#include <gleditor/render/types.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace render {
class RenderDevice;
}

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
  /**
   * @brief The casefolded description, worked out once here.
   *
   * Describing a font, printing the description and casefolding the result are
   * three allocations, and this used to do all three on every comparison --
   * twice, once for each side -- and again for every hash. A hash lookup does
   * that once to find the bucket and once more per key already in it, so the
   * cost fell on the cache's hottest path: a glyph is looked up per cluster
   * per page.
   *
   * Held as bytes rather than as a ustring because what is wanted of it is
   * equality, and casefolding is what makes byte equality the right question.
   */
  std::string key_;
  /// Worked out with the key, so that a probe that will fail usually fails on
  /// a size_t comparison rather than on a string one.
  std::size_t hash_;

public:
  explicit FontMapKeyAdapter(FontPtr font)
      : font_(std::move(font)),
        key_(font_->describe_with_absolute_size().to_string().casefold().raw()),
        hash_(std::hash<std::string>{}(key_)) {}
  FontMapKeyAdapter(const FontMapKeyAdapter &oth)            = default;
  FontMapKeyAdapter &operator=(const FontMapKeyAdapter &oth) = default;
  FontMapKeyAdapter(FontMapKeyAdapter &&oth)                 = default;
  FontMapKeyAdapter &operator=(FontMapKeyAdapter &&oth)      = default;

  /**
   * @brief Access the underlying Pango font reference.
   */
  [[nodiscard]] const FontPtr &font() const { return font_; }

  /// The casefolded description this compares and hashes by.
  [[nodiscard]] const std::string &key() const { return key_; }
  [[nodiscard]] std::size_t hash() const { return hash_; }

  /**
   * @brief Equality based on casefolded absolute-size font description.
   */
  bool operator==(const FontMapKeyAdapter &oth) const {
    return hash_ == oth.hash_ && key_ == oth.key_;
  }

  /**
   * @brief Three-way comparison using the casefolded font description.
   *
   * Byte order over the casefolded description, where this used to use
   * g_utf8_collate. Nothing orders these -- they are keys of an unordered_map
   * and only equality is ever asked for -- and collation answers a question
   * about how a person would sort names, at a price this path cannot pay.
   * Casefolded equality is unchanged, which is the part anything relies on.
   */
  std::partial_ordering operator<=>(const FontMapKeyAdapter &oth) const {
    return key_ <=> oth.key_;
  }
};

/**
 * @brief std::hash specialization for FontMapKeyAdapter based on font description.
 */
template <> struct std::hash<FontMapKeyAdapter> {
  std::size_t operator()(const FontMapKeyAdapter &adapter) const noexcept {
    return adapter.hash();
  }
};

/**
 * @class GlyphCache
 * @brief Caches rendered glyphs into a device array texture and returns UVs.
 *
 * GlyphCache uses Pango/Cairo to rasterize text for a given Pango::Font,
 * converts the result to single-channel coverage, and packs it into a layered
 * texture via GlyphPalette. The put() API returns texture coordinates, in
 * texels, and pixel dimensions for rendering.
 *
 * The atlas is allocated small and grown on demand: a layer is doubled until
 * the hardware will not take a larger one, and only then are layers added,
 * because doubling a layer buys four times the room where a second layer buys
 * two. Nothing already packed moves when it grows, which is what lets the
 * coordinates already written into a page's vertex buffer stay correct.
 */
class GlyphCache : public Loggable {
public:
  /**
   * @brief Return values for a cached glyph.
   * texCoords locate the glyph in its layer, in texels; dims are the pixel
   * rectangle sizes used for placement and rendering calculations.
   */
  struct Sizes {
    /**
     * @brief Where the glyph sits in its layer, in texels.
     *
     * Texels rather than a fraction of the texture because the atlas grows:
     * a fraction would move every glyph already named by a page's vertex
     * buffer the moment it did. The shader divides by the texture's own size
     * when it samples.
     */
    TextureCoords texCoords;
    Rect dims;               ///< Pixel dimensions of the rasterized glyph.
    int layer{};             ///< Array texture layer holding the glyph.
    /**
     * @brief Mean coverage over the glyph's box, 0 for blank and 1 for solid.
     *
     * How much of its own box the glyph actually inks. Free to compute -- the
     * coverage bitmap is already in hand when the glyph is rasterised -- and it
     * is what lets a line of text be replaced by a bar of the right darkness
     * without anything being assumed about the typeface.
     */
    float ink{};
  };
  /**
   * @brief Construct the glyph cache and allocate its device array texture.
   * @param aDevice Device used to size and upload the atlas. Not owned; must
   *        outlive the cache.
   */
  /**
   * @brief Layers the atlas may grow to, whatever the hardware allows.
   *
   * The layer index reaches the shader in six bits of
   * Doc::VBORow::layerWidthHeight, so the encoding is the binding limit and not
   * the driver -- lavapipe reports two thousand layers. doc.cpp asserts that
   * this and the packing agree, since neither header can see the other.
   */
  static constexpr int maxEncodableLayers = 64;

  explicit GlyphCache(render::RenderDevice *aDevice);
  ~GlyphCache() override;

  GlyphCache(GlyphCache &oth)           = delete;
  void operator=(const GlyphCache &oth) = delete;

  /// Longest cluster the cache will key on. Long enough for emoji sequences
  /// joined by zero-width joiners; a bound only so that a pathological run
  /// cannot become a cache key.
  static constexpr std::size_t maxClusterBytes = 64;

  /**
   * @brief Retrieve or create a glyph entry for one shaped cluster.
   * @param chr UTF-8 text of a whole cluster -- a ligature, or a base
   *            character with its combining marks -- rasterised as a unit so
   *            that what is drawn matches what Pango shaped.
   * @param font Loaded Pango font to use for rasterization.
   * @return Sizes with texel coordinates and pixel dimensions.
   * @throws std::invalid_argument if the cluster exceeds maxClusterBytes.
   */
  Sizes put(const std::string_view &chr, const FontPtr &font);

  /// Handle of the array texture holding every cached glyph.
  [[nodiscard]] render::TextureHandle textureHandle() const { return texture; }

  /// Side length of each atlas layer as it stands. Grows as glyphs arrive.
  [[nodiscard]] int atlasSize() const { return size; }
  /// Array layers allocated as it stands. Grows once the side length cannot.
  [[nodiscard]] int atlasLayers() const { return layerCount; }
  /// Ceilings growth stops at: the hardware's, narrowed by what a vertex can
  /// name.
  [[nodiscard]] int atlasMaxSize() const { return maxSize; }
  [[nodiscard]] int atlasMaxLayers() const { return maxLayers; }

  /**
   * @brief Rebuild the atlas mip chain if any glyph has been added since the
   *        last call.
   *
   * Called once per frame rather than once per glyph: loading a document adds
   * thousands of clusters between two frames, and the chain only has to be
   * right by the time something samples it. Cheap when nothing changed, which
   * is every frame after the document has settled.
   */
  void flush();

private:
  std::vector<GlyphPalette> palettes; ///< Palette layers used for packing.

  /**
   * @brief One glyph as it sits in the atlas, kept so a larger atlas can be
   *        refilled exactly as the old one was.
   *
   * Growing means a new texture object: array textures cannot gain layers and
   * 2D textures cannot gain pixels. Every glyph therefore has to be written
   * again, and it has to land on the same texel it was on, because the texture
   * coordinates naming it are already sitting in the vertex buffer of every
   * page that drew it.
   *
   * The coverage is the padded bitmap, the same bytes that were uploaded. That
   * is the memory price of being able to grow: bounded by the atlas itself,
   * since a glyph is only kept here once it has been packed into one.
   */
  struct Placement {
    int layer{};
    int x{};
    int y{};
    int width{};
    int height{};
    std::vector<std::byte> coverage;
  };
  std::vector<Placement> placements;

  /// Reallocate the atlas at @p newSize / @p newLayers and put every glyph back
  /// where it was.
  void reallocate(int newSize, int newLayers);
  /// Make room for a padded glyph box, growing the atlas if that is what it
  /// takes. Throws when neither the size nor the layer count can grow further.
  void makeRoomFor(const Rect &padded);
  /**
   * @brief The key for @p font, worked out once per font rather than per
   *        lookup.
   *
   * Building a key describes the font and prints the description, and printing
   * it formats the size as a float -- which is glibc's floating point printf,
   * and it showed up in a profile as several percent of a whole document load.
   * A cluster is looked up per glyph per page and every one of those was
   * building the same key from the same font.
   *
   * Keyed on the font's address, though what a key *means* is still its
   * description: two font objects describing the same font produce equal keys
   * here as they always did, so nothing about cache hits changes. This only
   * avoids working the description out again for a font already seen.
   */
  const FontMapKeyAdapter &keyFor(const FontPtr &font);

  /// Keys by font address. Holds a reference to each font through the adapter,
  /// so an address used as a key cannot be reused by a different font.
  std::unordered_map<const Pango::Font *, FontMapKeyAdapter> fontKeys;
  std::unordered_map<std::string, std::unordered_map<FontMapKeyAdapter, Sizes>,
                     transparent_string_hash, std::equal_to<>>
      glyphs; ///< Map: character string -> (font -> cached sizes).
  render::RenderDevice *device;   ///< Device the atlas lives on.
  render::TextureHandle texture{}; ///< Array texture holding the glyph atlas.
  int size{};      ///< Current side length of each atlas layer.
  int layerCount{}; ///< Array layers currently allocated.
  /// Ceilings growth stops at: what the hardware reports, narrowed by what the
  /// vertex encoding can address.
  int maxSize{}, maxLayers{};
  /// A glyph has been written to level zero since the mip chain was last built.
  bool atlasDirty{};

  /**
   * @brief Find or create a palette capable of fitting the given rectangle.
   */
  auto getBestPalette(const Rect &charBox);
  /**
   * @brief Rasterize and pack a new glyph into the cache.
   */
  Sizes addToCache(const std::string &chr, const FontPtr &font);
};

#endif // GLEDITOR_GLYPH_CACHE_H
// vi: set sw=2 sts=2 ts=2 et:
