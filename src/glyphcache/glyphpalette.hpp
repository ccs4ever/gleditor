#ifndef GLYPH_PALETTE_H
#define GLYPH_PALETTE_H

#include <atomic>
#include <compare>
#include <iostream>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

//#include "../custom_concepts.hpp"
#include "../gl/GL.hpp"
#include "../log.hpp"
#include "glyphLane.hpp"

class GlyphPalette : public Loggable {
private:
  int layer;
  Rect paletteDims;
  std::shared_ptr<GL> gl;
  Length usedHeight{};
  std::vector<GlyphLane> lanes;

  static std::atomic<int> layerCount;

  auto getBestLane(const Rect &charBox);

protected:
  void print(std::ostream &ost) const override {
    ost << "GlyphPalette(lanes: ";
    std::for_each(lanes.cbegin(), lanes.cend(),
                          [&ost](auto lane) { ost << lane << "\n"; });
    ost << ", w: " << paletteDims.width << ", h: " << paletteDims.height
        << ", availH: " << availHeight() << ")";
  }

public:
  GlyphPalette(Rect paletteDims, std::shared_ptr<GL> &ogl)
      : layer(layerCount.fetch_add(1)), paletteDims(paletteDims), gl{ogl} {}
  ~GlyphPalette() override = default;
  GlyphPalette(GlyphPalette &&oth) noexcept
      : layer(oth.layer), paletteDims(oth.paletteDims), gl(oth.gl),
        usedHeight(oth.usedHeight), lanes(std::move(oth.lanes)) {}
  GlyphPalette &operator=(GlyphPalette &&oth) noexcept {
    if (this == &oth) {
      return *this;
    }

    layer       = oth.layer;
    paletteDims = oth.paletteDims;
    usedHeight  = oth.usedHeight;
    lanes       = std::move(oth.lanes);

    return *this;
  }
  GlyphPalette(const GlyphPalette &oth)            = default;
  GlyphPalette &operator=(const GlyphPalette &oth) = default;

  /** Get the height of the remaining free space that isn't occupied
        by characters. The free width can be found on a per-GlyphLane basis
        by iterating over freeLanes. **/
  [[nodiscard]] Length availHeight() const {
    return Length{std::to_underlying(paletteDims.height) -
                  std::to_underlying(usedHeight)};
  }
  bool canFit(const Rect &rect);
  std::optional<TextureCoords> put(const Rect &charBox,
                                   const unsigned char *data);

  // order by most full to least full to ensure best usage
  friend std::partial_ordering operator<=>(const GlyphPalette &left,
                                           const GlyphPalette &right);
  friend bool operator==(const GlyphPalette &left, const GlyphPalette &right);
};

#endif // GLYPH_PALETTE_H
// vi: set sw=2 sts=2 ts=2 et:
