/**
 * @file caret_motion.cpp
 * @brief Where the caret goes for a key: by character, word, line and page.
 */
#include <gleditor/caret_motion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gleditor {

namespace {

bool continuation(const char byte) {
  return 0x80 == (static_cast<unsigned char>(byte) & 0xC0);
}

bool space(const char byte) {
  return ' ' == byte || '\t' == byte || '\n' == byte || '\r' == byte;
}

/// The line's text range, page-relative, without the newline that ends it.
std::pair<std::uint32_t, std::uint32_t>
lineSpan(const PageShaping::LineEntry &line, const std::string_view pageText) {
  auto end =
      std::min<std::uint32_t>(line.byteStart + line.byteLength,
                              static_cast<std::uint32_t>(pageText.size()));
  while (end > line.byteStart &&
         ('\n' == pageText[end - 1] || '\r' == pageText[end - 1])) {
    --end;
  }
  return {line.byteStart, end};
}

} // namespace

std::uint32_t stepCharacter(const std::string_view text, std::uint32_t at,
                            const bool forward) {
  const auto size = static_cast<std::uint32_t>(text.size());
  at              = std::min(at, size);
  if (forward) {
    if (at >= size) {
      return size;
    }
    ++at;
    while (at < size && continuation(text[at])) {
      ++at;
    }
    return at;
  }
  if (0 == at) {
    return 0;
  }
  --at;
  while (at > 0 && continuation(text[at])) {
    --at;
  }
  return at;
}

std::uint32_t stepWord(const std::string_view text, std::uint32_t at,
                       const bool forward) {
  const auto size = static_cast<std::uint32_t>(text.size());
  at              = std::min(at, size);
  if (forward) {
    while (at < size && !space(text[at])) {
      ++at;
    }
    while (at < size && space(text[at])) {
      ++at;
    }
    return at;
  }
  while (at > 0 && space(text[at - 1])) {
    --at;
  }
  while (at > 0 && !space(text[at - 1])) {
    --at;
  }
  return at;
}

LinePlace placeOnLines(const PageShaping &shaping, const std::uint32_t at) {
  LinePlace place;
  if (shaping.lines.empty()) {
    return place;
  }
  // The last line starting at or before the byte: a byte past a line's end
  // (its newline, or the page's end) belongs to that line.
  for (std::size_t i = 0; i < shaping.lines.size(); ++i) {
    if (shaping.lines[i].byteStart <= at) {
      place.line = i;
    }
  }
  const auto &line = shaping.lines[place.line];
  place.x          = line.left + line.barWidth;
  float best       = std::numeric_limits<float>::lowest();
  for (const auto &glyph : shaping.glyphs) {
    if (glyph.lineIndex != line.lineIndex ||
        glyph.clusterIndex >= shaping.clusters.size()) {
      continue;
    }
    const auto start = shaping.clusters[glyph.clusterIndex].byteStart;
    if (start == at) {
      place.x = glyph.clusterLeft;
      return place;
    }
    // The cluster the byte falls inside, when it is not on a boundary.
    const auto length = shaping.clusters[glyph.clusterIndex].byteLength;
    const bool inside = start < at && at < start + length;
    if (inside && best < static_cast<float>(start)) {
      best    = static_cast<float>(start);
      place.x = glyph.clusterLeft;
    }
  }
  return place;
}

std::uint32_t offsetOnLine(const PageShaping &shaping,
                           const std::string_view pageText,
                           const std::size_t line, const float x) {
  if (line >= shaping.lines.size()) {
    return static_cast<std::uint32_t>(pageText.size());
  }
  const auto &entry       = shaping.lines[line];
  const auto [start, end] = lineSpan(entry, pageText);
  std::uint32_t nearest   = end;
  float nearestDistance   = std::abs(entry.left + entry.barWidth - x);
  for (const auto &glyph : shaping.glyphs) {
    if (glyph.lineIndex != entry.lineIndex ||
        glyph.clusterIndex >= shaping.clusters.size()) {
      continue;
    }
    const auto byte = shaping.clusters[glyph.clusterIndex].byteStart;
    if (byte < start || byte >= end) {
      continue;
    }
    if (const float distance = std::abs(glyph.clusterLeft - x);
        distance < nearestDistance) {
      nearestDistance = distance;
      nearest         = byte;
    }
  }
  return nearest;
}

std::uint32_t caretTarget(const Doc &doc, const std::uint32_t offset,
                          const CaretMotion motion) {
  const std::string_view text = doc.contents();
  switch (motion) {
  case CaretMotion::Left:
    return stepCharacter(text, offset, false);
  case CaretMotion::Right:
    return stepCharacter(text, offset, true);
  case CaretMotion::WordLeft:
    return stepWord(text, offset, false);
  case CaretMotion::WordRight:
    return stepWord(text, offset, true);
  case CaretMotion::DocumentStart:
    return 0;
  case CaretMotion::DocumentEnd:
    return static_cast<std::uint32_t>(text.size());
  default:
    break;
  }

  const auto pageIndex = doc.pageIndexForOffset(offset);
  if (!pageIndex) {
    return offset;
  }
  const auto page = doc.page(*pageIndex);
  if (!page) {
    return offset;
  }
  const auto shaping = page->ensureShaping();
  const auto base    = page->baseOffset();
  const auto pageText =
      text.substr(std::min<std::size_t>(base, text.size()), page->textLength());
  const auto here = placeOnLines(shaping, offset - base);
  if (shaping.lines.empty()) {
    return offset;
  }
  switch (motion) {
  case CaretMotion::LineStart:
    return base + lineSpan(shaping.lines[here.line], pageText).first;
  case CaretMotion::LineEnd:
    return base + lineSpan(shaping.lines[here.line], pageText).second;
  case CaretMotion::Up:
    if (here.line > 0) {
      return base + offsetOnLine(shaping, pageText, here.line - 1, here.x);
    }
    if (*pageIndex > 0) {
      if (const auto above = doc.page(*pageIndex - 1)) {
        const auto aboveShaping = above->ensureShaping();
        if (!aboveShaping.lines.empty()) {
          return above->baseOffset() +
                 offsetOnLine(
                     aboveShaping,
                     text.substr(above->baseOffset(), above->textLength()),
                     aboveShaping.lines.size() - 1, here.x);
        }
      }
    }
    return 0;
  case CaretMotion::Down:
    if (here.line + 1 < shaping.lines.size()) {
      return base + offsetOnLine(shaping, pageText, here.line + 1, here.x);
    }
    if (const auto below = doc.page(*pageIndex + 1)) {
      const auto belowShaping = below->ensureShaping();
      if (!belowShaping.lines.empty()) {
        return below->baseOffset() +
               offsetOnLine(
                   belowShaping,
                   text.substr(below->baseOffset(), below->textLength()), 0,
                   here.x);
      }
    }
    return static_cast<std::uint32_t>(text.size());
  default:
    return offset;
  }
}

} // namespace gleditor
