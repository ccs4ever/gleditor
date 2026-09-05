#include <gleditor/text/layout.hpp>

#include <algorithm>
#include <cmath>
#include <fribidi.h>
#include <hb.h>
#include <iostream>
#include <linebreak.h>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gleditor::text {

namespace {

struct ShapedGlyph {
  uint32_t glyphIndex{};
  uint32_t clusterByteOffset{};
  float xAdvance{};
  float yAdvance{};
  float xOffset{};
  float yOffset{};
};

struct ShapedRun {
  std::vector<ShapedGlyph> glyphs;
};

ShapedRun shapeText(std::string_view text, const FontFacePtr &font) {
  ShapedRun run;
  if (text.empty() || !font || !font->hbFont()) {
    return run;
  }

  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()), 0,
                     static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buf);

  hb_shape(font->hbFont(), buf, nullptr, 0);

  unsigned int glyphCount    = 0;
  hb_glyph_info_t *glyphInfo = hb_buffer_get_glyph_infos(buf, &glyphCount);
  hb_glyph_position_t *glyphPos =
      hb_buffer_get_glyph_positions(buf, &glyphCount);

  run.glyphs.reserve(glyphCount);
  for (unsigned int i = 0; i < glyphCount; i++) {
    auto glyphIdx = glyphInfo[i].codepoint;
    float xAdv    = static_cast<float>(glyphPos[i].x_advance) / 64.0F;
    float yAdv    = static_cast<float>(glyphPos[i].y_advance) / 64.0F;
    float xOff    = static_cast<float>(glyphPos[i].x_offset) / 64.0F;
    float yOff    = static_cast<float>(glyphPos[i].y_offset) / 64.0F;

    if (glyphIdx == 0 && glyphInfo[i].cluster < text.size()) {
      const char *p   = text.data() + glyphInfo[i].cluster;
      const char *end = text.data() + text.size();
      uint32_t cp     = 0;
      if (p < end) {
        const auto c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
          cp = c;
        } else if ((c & 0xE0) == 0xC0 && p + 1 < end) {
          cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(p[1]) & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && p + 2 < end) {
          cp = ((c & 0x0F) << 12) |
               ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(p[2]) & 0x3F);
        } else if ((c & 0xF8) == 0xF0 && p + 3 < end) {
          cp = ((c & 0x07) << 18) |
               ((static_cast<unsigned char>(p[1]) & 0x3F) << 12) |
               ((static_cast<unsigned char>(p[2]) & 0x3F) << 6) |
               (static_cast<unsigned char>(p[3]) & 0x3F);
        }
      }
      if (cp > 32) {
        auto fallback = FontManager::instance().getFallbackFont(font, cp);
        if (fallback && fallback->hbFont()) {
          hb_buffer_t *fbuf = hb_buffer_create();
          hb_buffer_add_utf8(fbuf, text.data() + glyphInfo[i].cluster, -1, 0,
                             -1);
          hb_buffer_guess_segment_properties(fbuf);
          hb_shape(fallback->hbFont(), fbuf, nullptr, 0);
          unsigned int fCount    = 0;
          hb_glyph_info_t *fInfo = hb_buffer_get_glyph_infos(fbuf, &fCount);
          hb_glyph_position_t *fPos =
              hb_buffer_get_glyph_positions(fbuf, &fCount);
          if (fCount > 0 && fInfo[0].codepoint != 0) {
            glyphIdx = fInfo[0].codepoint;
            xAdv     = static_cast<float>(fPos[0].x_advance) / 64.0F;
            yAdv     = static_cast<float>(fPos[0].y_advance) / 64.0F;
            xOff     = static_cast<float>(fPos[0].x_offset) / 64.0F;
            yOff     = static_cast<float>(fPos[0].y_offset) / 64.0F;
          }
          hb_buffer_destroy(fbuf);
        }
      }
    }

    run.glyphs.push_back(ShapedGlyph{
        .glyphIndex        = glyphIdx,
        .clusterByteOffset = glyphInfo[i].cluster,
        .xAdvance          = xAdv,
        .yAdvance          = yAdv,
        .xOffset           = xOff,
        .yOffset           = yOff,
    });
  }

  hb_buffer_destroy(buf);
  return run;
}

std::size_t countUtf8Chars(std::string_view str) {
  std::size_t count = 0;
  for (const unsigned char c : str) {
    if ((c & 0xC0) != 0x80) {
      count++;
    }
  }
  return count;
}

/// Every decoration named by a range covering @p byteOffset, combined.
/// Overlapping ranges are additive rather than a conflict: a glyph inside
/// two ranges gets both sets of decorations, which is what lets an italic
/// span and a bold span meet or overlap without either caller needing to
/// know about the other.
gleditor::DecorationMask
decorationsAt(const std::size_t byteOffset,
              const std::vector<gleditor::DecoratedRange> &ranges) {
  gleditor::DecorationMask mask = 0;
  for (const auto &range : ranges) {
    if (byteOffset >= range.start && byteOffset < range.end) {
      mask = static_cast<gleditor::DecorationMask>(mask | range.decorations);
    }
  }
  return mask;
}

/// The line count of the atomic range starting exactly at @p byteOffset, if
/// any -- nullopt for a byte that is not the first byte of one of
/// @p ranges. A range is (end - start) consecutive newline characters (see
/// AtomicRange's own comment), so its line count at any font's pitch is just
/// that byte length; no separate pixel height is carried alongside it to
/// (dis)agree with this.
std::optional<std::size_t>
atomicRangeLinesStartingAt(const std::size_t byteOffset,
                           const std::vector<AtomicRange> &ranges) {
  for (const auto &range : ranges) {
    if (static_cast<std::size_t>(range.start) == byteOffset &&
        range.end > range.start) {
      return static_cast<std::size_t>(range.end - range.start);
    }
  }
  return std::nullopt;
}

/// The box anchored exactly at @p byteOffset, if any -- nullptr for a byte
/// that is not one of @p boxes' own anchors.
const gleditor::LayoutBox *
boxAnchoredAt(const std::size_t byteOffset,
              const std::vector<gleditor::LayoutBox> &boxes) {
  for (const auto &box : boxes) {
    if (static_cast<std::size_t>(box.anchor) == byteOffset) {
      return &box;
    }
  }
  return nullptr;
}

/// The paragraph style covering @p byteOffset, or TextAlign::Left with no
/// indent -- the meaning every line already had before BlockStyleRange
/// existed -- for a byte no range covers. The first matching range wins,
/// same as DecoratedRange callers already assume for a byte only ever
/// meant to be covered once (unlike DecoratedRange itself, whose per-glyph
/// decorations are deliberately additive).
gleditor::BlockStyleRange
blockStyleAt(const std::size_t byteOffset,
             const std::vector<gleditor::BlockStyleRange> &ranges) {
  for (const auto &range : ranges) {
    if (byteOffset >= range.start && byteOffset < range.end) {
      return range;
    }
  }
  return gleditor::BlockStyleRange{};
}

/// A FloatLeft/FloatRight box already placed on the page, holding just
/// enough to answer "how much does this narrow a line at height y" -- the
/// one question availableAt() below needs an active float for.
struct FloatSpan {
  float top{};
  float bottom{}; ///< Half-open: the float is active for y in [top, bottom).
  bool leftSide{};
  /// For a left float, the x just past its right edge (plus margin) -- a
  /// line's left bound can be no smaller than this. For a right float, the x
  /// just before its left edge (minus margin) -- a line's right bound can be
  /// no larger. One field serves both because a line only ever needs to
  /// compare against the edge nearer its own side.
  float edge{};
};

/// The horizontal band still open for something spanning [y, y+height), once
/// every float active over that span has narrowed it from its own side.
/// Floats are always flushed to an edge -- never straddling the middle of a
/// line -- so a line only ever sees one contiguous band rather than a set of
/// gaps to route text through; that is what keeps this a pair of scalars
/// instead of a list of spans.
struct Band {
  float left{};
  float right{};
};
Band availableAt(const float y, const float height, const float maxWidth,
                 const std::vector<FloatSpan> &floats) {
  Band band{0.0F, maxWidth};
  for (const auto &f : floats) {
    if (f.top < y + height && f.bottom > y) {
      if (f.leftSide) {
        band.left = std::max(band.left, f.edge);
      } else {
        band.right = std::min(band.right, f.edge);
      }
    }
  }
  return band;
}

/// Where a newly anchored float box lands: beside any already-active float
/// on the same side if the column still has room once the new one is
/// clamped to it, else pushed down to below whatever is blocking it --
/// repeated, since more than one float can already be stacked on that side.
struct FloatPlacement {
  float top{};
  float left{};
  float width{}; ///< Clamped to maxWidth -- see the comment at the call site.
};
FloatPlacement placeFloat(const float startY, const float rawWidth,
                          const float heightPx, const bool leftSide,
                          const float maxWidth,
                          const std::vector<FloatSpan> &floats) {
  const float width  = std::min(rawWidth, maxWidth);
  float candidateTop = startY;
  for (;;) {
    const auto band = availableAt(candidateTop, heightPx, maxWidth, floats);
    const float bandWidth = band.right - band.left;
    if (bandWidth >= width) {
      return FloatPlacement{
          .top   = candidateTop,
          .left  = leftSide ? band.left : (band.right - width),
          .width = width,
      };
    }
    // Doesn't fit beside what's already here -- find the nearest bottom
    // among floats blocking this exact y and try again just past it.
    float nextBottom = -1.0F;
    for (const auto &f : floats) {
      if (f.top <= candidateTop && f.bottom > candidateTop) {
        nextBottom =
            nextBottom < 0.0F ? f.bottom : std::min(nextBottom, f.bottom);
      }
    }
    if (nextBottom < 0.0F) {
      // Nothing at this y is actually blocking -- width alone must exceed
      // maxWidth, which should not happen since it is already clamped to
      // it. Place anyway rather than loop forever.
      return FloatPlacement{
          .top   = candidateTop,
          .left  = leftSide ? band.left : (band.right - width),
          .width = width,
      };
    }
    candidateTop = nextBottom;
  }
}

/// Where a box of @p width fits horizontally within a column of
/// @p maxWidth, per @p align. Justify has no meaning for a single box --
/// there is nothing to distribute slack between -- so it is treated as
/// Left, the same fallback a text line with no expansion opportunities
/// will use once justification exists.
float alignedLeft(const float maxWidth, const float width,
                  const gleditor::TextAlign align) {
  switch (align) {
  case gleditor::TextAlign::Right:
    return maxWidth - width;
  case gleditor::TextAlign::Centre:
    return (maxWidth - width) / 2.0F;
  case gleditor::TextAlign::Left:
  case gleditor::TextAlign::Justify:
  default:
    return 0.0F;
  }
}

/// One line's worth of glyphs, and where the line after it picks up.
struct FilledLine {
  std::size_t endGlyph{};  ///< One past the last glyph drawn on this line.
  std::size_t endByte{};   ///< One past the last byte this line covers.
  std::size_t nextGlyph{}; ///< Where the following line starts.
  float width{};
  /// Byte of the newline this line broke on, when it broke on one. What the
  /// caller needs to ask whether something is anchored there before it
  /// commits the line -- the page may have to end instead.
  std::size_t breakByte{};
  bool brokeOnNewline{false};
  /// Set when the glyphs ran out rather than the line filling up. The
  /// height check such a line faces is one a line ended by a real break does
  /// not: see layoutPage()'s own comment where this is consulted.
  bool hitEnd{false};
};

/**
 * @brief Fit as many of @p shaped's glyphs from @p lineStart as @p maxWidth
 *        holds, breaking at the last opportunity libunibreak offered.
 *
 * Lifted out of layoutPage()'s own loop, where the same work was done with
 * the glyph index rewound (`i = lineStart - 1; continue`) to re-measure the
 * remainder of a line that had just been broken. Pulling it out is what lets
 * the caller decide *where* a line goes -- which is the whole point of a
 * layout that has to step around a float -- before deciding what fits on it:
 * the width a line has available is no longer a constant the loop can close
 * over.
 *
 * Breaks on the first newline (unless @p singleParagraph), else at the last
 * break opportunity past @p lineStart, else -- a single word longer than the
 * line -- at whatever glyph overflowed. A glyph wider than @p maxWidth on its
 * own is still placed rather than looping forever, which is why the overflow
 * test requires having placed something first.
 */
FilledLine fillLine(const ShapedRun &shaped, const std::string_view text,
                    const std::vector<char> &breakAttrs,
                    const std::size_t lineStart, const float maxWidth,
                    const bool singleParagraph) {
  float lineWidth            = 0.0F;
  std::size_t lastBreakGlyph = lineStart;
  float widthAtBreak         = 0.0F;

  for (std::size_t i = lineStart; i < shaped.glyphs.size(); i++) {
    const auto &g      = shaped.glyphs[i];
    const auto bytePos = g.clusterByteOffset;

    const bool isNewline =
        (bytePos < text.size() && text[bytePos] == '\n' && !singleParagraph);

    const auto charBreak =
        bytePos < breakAttrs.size() ? breakAttrs[bytePos] : LINEBREAK_NOBREAK;
    if (charBreak == LINEBREAK_ALLOWBREAK || charBreak == LINEBREAK_MUSTBREAK) {
      lastBreakGlyph = i;
      widthAtBreak   = lineWidth;
    }

    const float nextWidth = lineWidth + g.xAdvance;

    if (isNewline || (nextWidth > maxWidth && i > lineStart)) {
      std::size_t breakAt = i;
      float finalWidth    = lineWidth;

      if (!isNewline && lastBreakGlyph > lineStart) {
        breakAt    = lastBreakGlyph;
        finalWidth = widthAtBreak;
      }
      if (breakAt == lineStart) {
        // Emergency single glyph break.
        breakAt    = i;
        finalWidth = lineWidth;
      }

      FilledLine out;
      out.width     = finalWidth;
      out.endGlyph  = breakAt;
      out.endByte   = (breakAt < shaped.glyphs.size())
                          ? shaped.glyphs[breakAt].clusterByteOffset
                          : text.size();
      out.breakByte = bytePos;

      if (isNewline) {
        out.brokeOnNewline = true;
        out.endGlyph       = i;
        out.endByte        = (i + 1 < shaped.glyphs.size())
                                 ? shaped.glyphs[i + 1].clusterByteOffset
                                 : text.size();
        out.nextGlyph      = i + 1;
        return out;
      }

      out.nextGlyph = breakAt;
      // A line broken at a space leaves that space behind rather than
      // opening the next line with it.
      if (breakAt < shaped.glyphs.size() &&
          shaped.glyphs[breakAt].clusterByteOffset < text.size() &&
          text[shaped.glyphs[breakAt].clusterByteOffset] == ' ') {
        out.nextGlyph = breakAt + 1;
      }
      return out;
    }

    lineWidth = nextWidth;
  }

  // Ran out of glyphs rather than out of room: everything left is one line.
  FilledLine out;
  out.width     = lineWidth;
  out.endGlyph  = shaped.glyphs.size();
  out.endByte   = text.size();
  out.nextGlyph = shaped.glyphs.size();
  out.hitEnd    = true;
  return out;
}

} // namespace

PageShaping TextLayout::layoutPage(std::string_view text,
                                   const FontFacePtr &font,
                                   const LayoutOptions &options) {
  PageShaping shaping;
  shaping.page = options.page;
  if (text.empty() || !font) {
    shaping.limit = 0;
    return shaping;
  }

  // Bound the input text slice if maxHeightPx is set, to avoid redundant
  // quadratic full-document shaping on multi-megabyte texts when generating
  // individual pages.
  if (options.maxHeightPx > 0.0F) {
    const float lh = std::max(1.0F, font->metrics().lineHeight);
    const auto maxLinesEst =
        static_cast<std::size_t>(std::ceil(options.maxHeightPx / lh)) + 8;
    const std::size_t sliceBudget =
        std::max<std::size_t>(32768, maxLinesEst * 1024);
    if (text.size() > sliceBudget) {
      std::size_t cut = sliceBudget;
      while (cut < text.size() &&
             (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        cut++;
      }
      text = text.substr(0, cut);
    }
  }

  static bool lbInited = false;
  if (!lbInited) {
    init_linebreak();
    lbInited = true;
  }

  // 1. Calculate Unicode line breaking opportunities
  std::vector<char> breakAttrs(text.size(), 0);
  set_linebreaks_utf8(reinterpret_cast<const utf8_t *>(text.data()),
                      text.size(), "",
                      reinterpret_cast<char *>(breakAttrs.data()));

  // 2. Shape the text with HarfBuzz (with multi-font fallback)
  const auto shaped = shapeText(text, font);
  if (shaped.glyphs.empty()) {
    shaping.limit = 0;
    return shaping;
  }

  const float lineHeight = font->metrics().lineHeight;
  const float maxWidth = options.maxWidthPx > 0.0F ? options.maxWidthPx : 1e6F;
  const float maxHeight =
      options.maxHeightPx > 0.0F ? options.maxHeightPx : 1e6F;

  struct LineInfo {
    std::size_t startGlyph{};
    std::size_t endGlyph{};
    std::size_t startByte{};
    std::size_t endByte{};
    float width{};
    float top{};
    /// Where this line's ink starts, text-area-relative -- 0 except where a
    /// float has pushed it in, or a Block box sits right/centred. Baked into
    /// each glyph's clusterLeft and echoed in LineEntry.left below, rather
    /// than left for a renderer to add: the renderer only ever draws
    /// g.clusterLeft on its own (src/doc.cpp's glyph loop), so this is the
    /// one place that offset can still take effect.
    float left{};
  };

  std::vector<LineInfo> lines;
  std::vector<PageShaping::PlacedBox> placedBoxes;
  std::vector<FloatSpan> floats;
  float currentY        = 0.0F;
  std::size_t lineStart = 0;

  // One iteration places one line. Where the line goes is settled before what
  // goes on it, which is what a layout stepping around a float needs and what
  // the single glyph-indexed loop this replaces could not express.
  while (lineStart < shaped.glyphs.size()) {
    const auto lineStartByte = shaped.glyphs[lineStart].clusterByteOffset;

    // A Block or Float box anchored exactly here interrupts the flow rather
    // than being filled in among surrounding text the way an Inline box will
    // be (a later stage) or fillLine()'s glyphs are. Inline falls through to
    // fillLine() untouched -- threading a box into the middle of a line is
    // not implemented yet, and nothing supplies that placement before it is.
    if (const auto *box = boxAnchoredAt(lineStartByte, options.boxes);
        nullptr != box && gleditor::BoxPlacement::Block == box->placement) {
      const float boxHeight = box->heightPx + box->marginPx;

      // Same rule as an atomic range just below: a box that would not fit
      // what remains of this page rolls whole to the next page's call
      // rather than starting here and being cut off partway through.
      // Skipped on an empty page for the same reason -- a box taller than a
      // whole page should not happen (callers are expected to fit media to
      // the page before handing it a LayoutBox), but must still make
      // progress rather than looping in place.
      if (!lines.empty() && currentY + boxHeight > maxHeight) {
        break;
      }

      const auto align = blockStyleAt(lineStartByte, options.blockStyles).align;
      const float width   = std::min(box->widthPx, maxWidth);
      const float left    = alignedLeft(maxWidth, width, align);
      const auto nextByte = (lineStart + 1 < shaped.glyphs.size())
                                ? shaped.glyphs[lineStart + 1].clusterByteOffset
                                : text.size();

      placedBoxes.push_back(PageShaping::PlacedBox{
          .anchorByteOffset = static_cast<std::uint32_t>(lineStartByte),
          .id               = box->id,
          .left             = left,
          .top              = currentY,
          .width            = width,
          .height           = box->heightPx,
          .placement        = box->placement,
      });

      // A caret must still be able to land on the anchor: an empty glyph
      // range (startGlyph == endGlyph) is the same convention a media
      // placeholder's blank newlines already use, and for the same two
      // reasons -- it keeps the assembly pass below from emitting a
      // GlyphEntry nothing should draw a glyph over (an anchor character
      // can shape to a real, visible fallback glyph otherwise -- confirmed
      // empirically, not assumed), and it is what lets caretGeometry()'s
      // line-range fallback find the anchor by byte range alone.
      lines.push_back(LineInfo{
          .startGlyph = lineStart,
          .endGlyph   = lineStart,
          .startByte  = lineStartByte,
          .endByte    = nextByte,
          .width      = width,
          .top        = currentY,
          .left       = left,
      });

      currentY += boxHeight;
      if (currentY + lineHeight > maxHeight) {
        break;
      }
      lineStart += 1;
      continue;
    } else if (nullptr != box &&
               (gleditor::BoxPlacement::FloatLeft == box->placement ||
                gleditor::BoxPlacement::FloatRight == box->placement)) {
      const bool leftSide = gleditor::BoxPlacement::FloatLeft == box->placement;
      const auto placed   = placeFloat(currentY, box->widthPx, box->heightPx,
                                       leftSide, maxWidth, floats);

      // Same roll-to-next-page rule as a Block box: this can push the float
      // lower than currentY (stacked below a same-side float already here),
      // so the check is against where it actually landed, not where its
      // anchor was found.
      if (!lines.empty() && placed.top + box->heightPx > maxHeight) {
        break;
      }

      floats.push_back(FloatSpan{
          .top      = placed.top,
          .bottom   = placed.top + box->heightPx,
          .leftSide = leftSide,
          .edge     = leftSide ? (placed.left + placed.width + box->marginPx)
                               : (placed.left - box->marginPx),
      });

      const auto nextByte = (lineStart + 1 < shaped.glyphs.size())
                                ? shaped.glyphs[lineStart + 1].clusterByteOffset
                                : text.size();
      placedBoxes.push_back(PageShaping::PlacedBox{
          .anchorByteOffset = static_cast<std::uint32_t>(lineStartByte),
          .id               = box->id,
          .left             = placed.left,
          .top              = placed.top,
          .width            = placed.width,
          .height           = box->heightPx,
          .placement        = box->placement,
      });

      // Same empty-glyph-range convention as a Block box's caret anchor, but
      // this line carries no vertical weight of its own -- a float does not
      // itself end the line its anchor sits on, text keeps flowing on it,
      // just against a band the float has now narrowed.
      lines.push_back(LineInfo{
          .startGlyph = lineStart,
          .endGlyph   = lineStart,
          .startByte  = lineStartByte,
          .endByte    = nextByte,
          .width      = 0.0F,
          .top        = currentY,
      });

      lineStart += 1;
      continue;
    }

    // The band still open at this height, once every active float has
    // narrowed it from its own side. A line with nothing usable in it (two
    // floats meeting in the middle) advances past the nearer float's bottom
    // and tries again, rather than shaping a zero-width line.
    Band band{0.0F, maxWidth};
    bool pageFull = false;
    for (;;) {
      band = availableAt(currentY, lineHeight, maxWidth, floats);
      if (band.right - band.left > 0.0F) {
        break;
      }
      float nextBottom = -1.0F;
      for (const auto &f : floats) {
        if (f.top <= currentY && f.bottom > currentY) {
          nextBottom =
              nextBottom < 0.0F ? f.bottom : std::min(nextBottom, f.bottom);
        }
      }
      if (nextBottom < 0.0F) {
        // Nothing here is actually blocking -- maxWidth itself must be zero,
        // which should not happen. Stop rather than spin in place.
        break;
      }
      currentY = nextBottom;
      if (!lines.empty() && currentY + lineHeight > maxHeight) {
        pageFull = true;
        break;
      }
    }
    if (pageFull) {
      break;
    }

    const auto filled =
        fillLine(shaped, text, breakAttrs, lineStart, band.right - band.left,
                 options.singleParagraph);

    // An atomic range (a media placeholder's reserved lines) must not start
    // on this page unless the whole thing fits: checked before committing
    // the line whose newline the range begins at, so a range that does not
    // fit rolls onto the next page's call in its entirety, together with
    // whatever un-terminated text shares that line, rather than splitting
    // mid-range. Skipped on an empty page (no lines committed yet) so a
    // range taller than maxHeight itself -- which should not happen, since
    // every placeholder height reaching here is already clamped to at most
    // one page, but a future caller's mistake should not be able to spin
    // this in place forever -- still makes progress.
    if (filled.brokeOnNewline && !lines.empty()) {
      if (const auto rangeLines = atomicRangeLinesStartingAt(
              filled.breakByte, options.atomicRanges)) {
        const float rangeHeight = static_cast<float>(*rangeLines) * lineHeight;
        if (currentY + rangeHeight > maxHeight) {
          break;
        }
      }
    }

    // A line the glyphs simply ran out on has to clear the height bar before
    // it is committed; a line ended by a real break does not, having already
    // been started. The asymmetry is inherited deliberately -- it is what
    // decided, before this loop was one loop, whether a page shorter than a
    // single line came back empty (limit 0, which stops pagination) rather
    // than with one line overhanging it.
    if (filled.hitEnd && currentY + lineHeight > maxHeight) {
      break;
    }

    lines.push_back(LineInfo{
        .startGlyph = lineStart,
        .endGlyph   = filled.endGlyph,
        .startByte  = shaped.glyphs[lineStart].clusterByteOffset,
        .endByte    = filled.endByte,
        .width      = filled.width,
        .top        = currentY,
        .left       = band.left,
    });

    currentY += lineHeight;
    if (currentY + lineHeight > maxHeight) {
      break;
    }
    lineStart = filled.nextGlyph;
  }

  if (lines.empty()) {
    shaping.limit = 0;
    return shaping;
  }

  // 3. Assemble PageShaping data
  shaping.limit        = lines.back().endByte;
  shaping.textHeightPx = static_cast<int>(std::ceil(currentY));
  shaping.lineCount    = lines.size();
  shaping.boxes        = std::move(placedBoxes);

  float maxSeenWidth = 0.0F;
  for (std::size_t lineIdx = 0; lineIdx < lines.size(); lineIdx++) {
    const auto &line = lines[lineIdx];
    // The rightmost extent, not the ink width alone -- a line pushed right by
    // a float (or, later, a right/centre-aligned one) can end well past its
    // own width from text-area x=0.
    maxSeenWidth = std::max(maxSeenWidth, line.left + line.width);

    shaping.lines.push_back(PageShaping::LineEntry{
        .barWidth   = line.width,
        .barHeight  = lineHeight,
        .left       = line.left,
        .top        = line.top,
        .lineIndex  = lineIdx,
        .byteStart  = static_cast<std::uint32_t>(line.startByte),
        .byteLength = static_cast<std::uint32_t>(line.endByte - line.startByte),
    });

    float penX = 0.0F;
    for (std::size_t gi = line.startGlyph; gi < line.endGlyph; gi++) {
      const auto &g      = shaped.glyphs[gi];
      const auto byteOff = g.clusterByteOffset;

      // Extract cluster substring
      std::size_t nextByte = text.size();
      if (gi + 1 < shaped.glyphs.size()) {
        nextByte = shaped.glyphs[gi + 1].clusterByteOffset;
      }
      if (nextByte <= byteOff || nextByte > text.size()) {
        nextByte = std::min(text.size(), static_cast<std::size_t>(byteOff + 1));
      }

      const auto clusterStr    = text.substr(byteOff, nextByte - byteOff);
      const auto clusterBoxIdx = shaping.clusters.size();

      shaping.clusters.push_back(ClusterBox{
          .byteStart  = static_cast<uint32_t>(byteOff),
          .byteLength = static_cast<uint32_t>(clusterStr.size()),
          .charCount  = static_cast<uint32_t>(countUtf8Chars(clusterStr)),
      });

      shaping.glyphs.push_back(PageShaping::GlyphEntry{
          .chr          = std::string{clusterStr},
          .clusterLeft  = line.left + penX + g.xOffset,
          .clusterTop   = line.top,
          .clusterIndex = clusterBoxIdx,
          .lineIndex    = lineIdx,
          .decorations  = decorationsAt(byteOff, options.decoratedRanges),
      });

      penX += g.xAdvance;
    }
  }

  // A blank placeholder line has no glyphs, so the scan above never sees
  // whatever's actually drawn over it -- an atomic range fully included on
  // this page (its start byte is before where this page ends) widens the
  // page to at least its own minWidthPx, the same way a real text line's
  // glyph width already does above.
  for (const auto &range : options.atomicRanges) {
    if (range.start < shaping.limit) {
      maxSeenWidth = std::max(maxSeenWidth, range.minWidthPx);
    }
  }

  // A placed box's own rightmost extent, for the same reason: it has no
  // glyphs of its own for the scan above to see either. left + width rather
  // than width alone, so a right-aligned or right-floated box still widens
  // the page to reach its own right edge rather than just its own size.
  for (const auto &placed : shaping.boxes) {
    maxSeenWidth = std::max(maxSeenWidth, placed.left + placed.width);
  }

  shaping.textWidthPx = static_cast<int>(std::ceil(maxSeenWidth));
  return shaping;
}

PageShaping TextLayout::layoutSingleLine(std::string_view text,
                                         const FontFacePtr &font,
                                         const float maxWidthPx,
                                         const bool ellipsize) {
  LayoutOptions opts{
      .maxWidthPx      = maxWidthPx,
      .maxHeightPx     = 0.0F,
      .singleParagraph = true,
      .ellipsize       = ellipsize,
  };
  return layoutPage(text, font, opts);
}

PageShaping TextLayout::layoutSingleLine(std::string_view text,
                                         const FontFacePtr &font,
                                         const LayoutOptions &options) {
  LayoutOptions opts   = options;
  opts.singleParagraph = true;
  return layoutPage(text, font, opts);
}

} // namespace gleditor::text
