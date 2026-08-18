#include <gleditor/text/layout.hpp>

#include <algorithm>
#include <cmath>
#include <fribidi.h>
#include <hb.h>
#include <iostream>
#include <linebreak.h>
#include <string>
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

ShapedRun shapeText(std::string_view text, hb_font_t *hbFont) {
  ShapedRun run;
  if (text.empty() || !hbFont) {
    return run;
  }

  hb_buffer_t *buf = hb_buffer_create();
  hb_buffer_add_utf8(buf, text.data(), static_cast<int>(text.size()), 0,
                     static_cast<int>(text.size()));
  hb_buffer_guess_segment_properties(buf);

  hb_shape(hbFont, buf, nullptr, 0);

  unsigned int glyphCount    = 0;
  hb_glyph_info_t *glyphInfo = hb_buffer_get_glyph_infos(buf, &glyphCount);
  hb_glyph_position_t *glyphPos =
      hb_buffer_get_glyph_positions(buf, &glyphCount);

  run.glyphs.reserve(glyphCount);
  for (unsigned int i = 0; i < glyphCount; i++) {
    run.glyphs.push_back(ShapedGlyph{
        .glyphIndex        = glyphInfo[i].codepoint,
        .clusterByteOffset = glyphInfo[i].cluster,
        .xAdvance          = static_cast<float>(glyphPos[i].x_advance) / 64.0F,
        .yAdvance          = static_cast<float>(glyphPos[i].y_advance) / 64.0F,
        .xOffset           = static_cast<float>(glyphPos[i].x_offset) / 64.0F,
        .yOffset           = static_cast<float>(glyphPos[i].y_offset) / 64.0F,
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

} // namespace

PageShaping TextLayout::layoutPage(std::string_view text,
                                   const FontFacePtr &font,
                                   const LayoutOptions &options) {
  PageShaping shaping;
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

  // 2. Shape the text with HarfBuzz
  const auto shaped = shapeText(text, font->hbFont());
  if (shaped.glyphs.empty()) {
    shaping.limit = 0;
    return shaping;
  }

  const float lineHeight = font->metrics().lineHeight;
  const float ascent     = font->metrics().ascent;
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
  };

  std::vector<LineInfo> lines;
  float currentY             = 0.0F;
  std::size_t lineStart      = 0;
  float lineWidth            = 0.0F;
  std::size_t lastBreakGlyph = 0;
  float widthAtBreak         = 0.0F;

  for (std::size_t i = 0; i < shaped.glyphs.size(); i++) {
    const auto &g      = shaped.glyphs[i];
    const auto bytePos = g.clusterByteOffset;

    // Check if character is explicit newline
    const bool isNewline = (bytePos < text.size() && text[bytePos] == '\n' &&
                            !options.singleParagraph);

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
        // Emergency single glyph break
        breakAt    = i;
        finalWidth = lineWidth;
      }

      const auto startByte = shaped.glyphs[lineStart].clusterByteOffset;
      std::size_t endGlyph = breakAt;
      std::size_t endByte  = (breakAt < shaped.glyphs.size())
                                 ? shaped.glyphs[breakAt].clusterByteOffset
                                 : text.size();

      if (isNewline) {
        endGlyph = i;
        endByte  = (i + 1 < shaped.glyphs.size())
                       ? shaped.glyphs[i + 1].clusterByteOffset
                       : text.size();
      }

      lines.push_back(LineInfo{
          .startGlyph = lineStart,
          .endGlyph   = endGlyph,
          .startByte  = startByte,
          .endByte    = endByte,
          .width      = finalWidth,
          .top        = currentY,
      });

      currentY += lineHeight;

      // Check if page capacity is exceeded
      if (currentY + lineHeight > maxHeight) {
        break;
      }

      // If broken on newline, advance past newline character
      if (isNewline) {
        lineStart = i + 1;
      } else {
        lineStart = breakAt;
        if (breakAt < shaped.glyphs.size() &&
            shaped.glyphs[breakAt].clusterByteOffset < text.size() &&
            text[shaped.glyphs[breakAt].clusterByteOffset] == ' ') {
          lineStart = breakAt + 1;
        }
      }

      lineWidth      = 0.0F;
      lastBreakGlyph = lineStart;
      widthAtBreak   = 0.0F;

      if (isNewline) {
        continue;
      }

      // Re-measure remaining glyph
      i = lineStart > 0 ? lineStart - 1 : 0;
      continue;
    }

    lineWidth = nextWidth;
  }

  // Trailing line
  if (lineStart < shaped.glyphs.size() && currentY + lineHeight <= maxHeight) {
    const auto startByte = shaped.glyphs[lineStart].clusterByteOffset;
    lines.push_back(LineInfo{
        .startGlyph = lineStart,
        .endGlyph   = shaped.glyphs.size(),
        .startByte  = startByte,
        .endByte    = text.size(),
        .width      = lineWidth,
        .top        = currentY,
    });
    currentY += lineHeight;
  }

  if (lines.empty()) {
    shaping.limit = 0;
    return shaping;
  }

  // 3. Assemble PageShaping data
  shaping.limit        = lines.back().endByte;
  shaping.textHeightPx = static_cast<int>(std::ceil(currentY));
  shaping.lineCount    = lines.size();

  float maxSeenWidth = 0.0F;
  for (std::size_t lineIdx = 0; lineIdx < lines.size(); lineIdx++) {
    const auto &line = lines[lineIdx];
    maxSeenWidth     = std::max(maxSeenWidth, line.width);

    shaping.lines.push_back(PageShaping::LineEntry{
        .barWidth  = line.width,
        .barHeight = lineHeight,
        .left      = 0.0F,
        .top       = line.top,
        .lineIndex = lineIdx,
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
          .clusterLeft  = penX + g.xOffset,
          .clusterTop   = line.top,
          .clusterIndex = clusterBoxIdx,
          .lineIndex    = lineIdx,
      });

      penX += g.xAdvance;
    }
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
