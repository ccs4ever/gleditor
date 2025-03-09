#ifndef GLEDITOR_GLYPH_CACHE_H
#define GLEDITOR_GLYPH_CACHE_H

#include <glibmm/ustring_hash.h>
#include <map>
#include <memory>
#include <optional>
#include <pangomm/font.h>
#include <utility>

#include "../gl/GL.hpp"
#include "../log.hpp"
#include "glibmm/ustring.h"
#include "glyphpalette.hpp"
#include "types.hpp"

template <typename... Bases> struct overload : Bases... {
  using is_transparent = void;
  using Bases::operator()...;
};

struct char_pointer_hash {
  auto operator()(const char *ptr) const noexcept {
    return std::hash<std::string_view>{}(ptr);
  }
};

using transparent_string_hash =
    overload<std::hash<std::string>, std::hash<std::string_view>,
             char_pointer_hash>;

using FontPtr = Glib::RefPtr<Pango::Font>;

class FontMapKeyAdapter {
private:
  FontPtr font_;

public:
  FontMapKeyAdapter(FontPtr font) : font_(std::move(font)) {}
  FontMapKeyAdapter(const FontMapKeyAdapter &oth)            = default;
  FontMapKeyAdapter &operator=(const FontMapKeyAdapter &oth) = default;
  FontMapKeyAdapter(FontMapKeyAdapter &&oth)                 = default;
  FontMapKeyAdapter &operator=(FontMapKeyAdapter &&oth)      = default;

  [[nodiscard]] const FontPtr &font() const { return font_; }

  bool operator==(const FontMapKeyAdapter &oth) const {
    return (*this <=> oth) == std::partial_ordering::equivalent;
  }

  std::partial_ordering operator<=>(const FontMapKeyAdapter &oth) const {
    auto myStr = font_->describe_with_absolute_size().to_string().casefold();
    auto othStr =
        oth.font_->describe_with_absolute_size().to_string().casefold();
    auto cmp = myStr.compare(othStr);
    if (0 == cmp) {
      return std::partial_ordering::equivalent;
    }
    if (0 > cmp) {
      return std::partial_ordering::less;
    }
    return std::partial_ordering::greater;
  }
};

template <> struct std::hash<FontMapKeyAdapter> {
  std::size_t operator()(const FontMapKeyAdapter &adapter) const {
    return std::hash<std::string>{}(
        adapter.font()->describe_with_absolute_size().to_string().casefold());
  }
};

class GlyphCache : public Loggable {
public:
  struct Sizes {
    TextureCoords texCoords;
    Rect dims;
  };
  GlyphCache(std::shared_ptr<GL> &ogl);
  ~GlyphCache() override = default;

  GlyphCache(GlyphCache &oth)           = delete;
  void operator=(const GlyphCache &oth) = delete;

  Sizes put(const std::string_view &chr, const FontPtr &font);
  void bindTexture(int active) const {
    glActiveTexture(GL_TEXTURE0 + active);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textArrId);
  }
  static void clearTexture() { glBindTexture(GL_TEXTURE_2D_ARRAY, 0); }

private:
  std::vector<GlyphPalette> palettes;
  std::unordered_map<std::string, std::unordered_map<FontMapKeyAdapter, Sizes>,
                     transparent_string_hash, std::equal_to<>>
      glyphs;
  std::shared_ptr<GL> gl;
  int size, maxLayers;
  GLuint textArrId;

  auto getBestPalette(const Rect &charBox);
  void initTextureArray();
  Sizes addToCache(const std::string &chr, const FontPtr &font);
};

#endif // GLEDITOR_GLYPH_CACHE_H
// vi: set sw=2 sts=2 ts=2 et:
