#include <gleditor/text/fit.hpp>

#include <gleditor/text/diagnostics.hpp>
#include <gleditor/text/shaping_cache.hpp>

#include "unicode_breaks.hpp"

#include <algorithm>
#include <cmath>
#include <fribidi.h>
#include <graphemebreak.h>
#include <hb.h>
#include <limits>
#include <linebreak.h>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace gleditor::text {
namespace {

struct Character {
  FriBidiChar value{};
  std::size_t byte{};
  FriBidiLevel level{};
  FriBidiParType paragraph{FRIBIDI_PAR_LTR};
  hb_script_t script{HB_SCRIPT_COMMON};
  FontFacePtr font;
};

std::vector<Character> decode(const std::string_view text) {
  return detail::decodeCharacters<Character>(text);
}

bool weakScript(const hb_script_t script) {
  return script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED ||
         script == HB_SCRIPT_UNKNOWN;
}

bool boundary(const std::vector<char> &graphemes, const std::size_t byte) {
  return byte == 0 || byte == graphemes.size() ||
         graphemes[byte - 1] == GRAPHEMEBREAK_BREAK;
}

void resolveCharacters(std::vector<Character> &characters,
                       const std::vector<char> &graphemes,
                       const FontFacePtr &font) {
  for (std::size_t begin = 0; begin < characters.size();) {
    auto end = begin;
    while (end < characters.size() && characters[end].value != '\n') {
      end++;
    }
    std::vector<FriBidiChar> values;
    values.reserve(end - begin);
    for (auto i = begin; i < end; i++) {
      values.push_back(characters[i].value);
    }
    std::vector<FriBidiCharType> types(values.size());
    std::vector<FriBidiBracketType> brackets(values.size());
    std::vector<FriBidiLevel> levels(values.size());
    FriBidiParType paragraph = FRIBIDI_PAR_ON;
    if (!values.empty()) {
      const auto count = static_cast<FriBidiStrIndex>(values.size());
      fribidi_get_bidi_types(values.data(), count, types.data());
      fribidi_get_bracket_types(values.data(), count, types.data(),
                                brackets.data());
      if (fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(),
                                              count, &paragraph,
                                              levels.data()) == 0) {
        throw std::runtime_error("Cannot resolve text direction");
      }
    }
    if (paragraph == FRIBIDI_PAR_ON) {
      paragraph = FRIBIDI_PAR_LTR;
    }
    hb_script_t script = HB_SCRIPT_COMMON;
    for (auto i = begin; i < end; i++) {
      const auto candidate = hb_unicode_script(hb_unicode_funcs_get_default(),
                                               characters[i].value);
      if (!weakScript(candidate)) {
        script = candidate;
        break;
      }
    }
    for (auto i = begin; i < end; i++) {
      auto &character = characters[i];
      const auto candidate =
          hb_unicode_script(hb_unicode_funcs_get_default(), character.value);
      if (!weakScript(candidate)) {
        script = candidate;
      }
      character.script    = script;
      character.level     = levels[i - begin];
      character.paragraph = paragraph;
      character.font      = font;
    }
    if (end < characters.size()) {
      characters[end].paragraph = paragraph;
      characters[end].font      = font;
    }
    begin = end + 1;
  }

  // Font and script itemization cannot separate a base from its combining
  // marks or an emoji from the rest of its ZWJ sequence.
  for (std::size_t begin = 0; begin < characters.size();) {
    auto end = begin + 1;
    while (end < characters.size() &&
           !boundary(graphemes, characters[end].byte)) {
      end++;
    }
    auto selected = font;
    for (auto i = begin; i < end; i++) {
      hb_codepoint_t glyph{};
      const auto value = characters[i].value;
      if (value <= 0x20 || value == 0x200DU ||
          (value >= 0xFE00U && value <= 0xFE0FU) ||
          hb_font_get_nominal_glyph(font->hbFont(), value, &glyph)) {
        continue;
      }
      const auto fallback =
          FontManager::instance().getFallbackFont(font, value);
      if (fallback) {
        selected = *fallback;
        break;
      }
    }
    for (auto i = begin; i < end; i++) {
      characters[i].font   = selected;
      characters[i].script = characters[begin].script;
      characters[i].level  = characters[begin].level;
    }
    begin = end;
  }
}

struct Advance {
  std::size_t byte{};
  float width{};
};

using Buffer = std::unique_ptr<hb_buffer_t, decltype(&hb_buffer_destroy)>;

std::vector<Advance> shapeRuns(const std::string_view text,
                               const std::vector<Character> &characters,
                               const std::vector<char> &graphemes,
                               const FontFacePtr &primary) {
  std::vector<Advance> advances;
  for (std::size_t begin = 0; begin < characters.size();) {
    if (characters[begin].value == '\n') {
      advances.push_back(Advance{.byte = characters[begin].byte});
      begin++;
      continue;
    }
    auto end          = begin + 1;
    const auto &first = characters[begin];
    while (end < characters.size() && characters[end].value != '\n') {
      const auto &next = characters[end];
      if (boundary(graphemes, next.byte) &&
          (next.level != first.level || next.script != first.script ||
           next.font != first.font)) {
        break;
      }
      end++;
    }
    const auto endByte =
        end < characters.size() ? characters[end].byte : text.size();
    Buffer buffer{hb_buffer_create(), &hb_buffer_destroy};
    hb_buffer_add_utf8(buffer.get(), text.data(), static_cast<int>(text.size()),
                       static_cast<unsigned int>(first.byte),
                       static_cast<int>(endByte - first.byte));
    hb_buffer_set_direction(buffer.get(), (first.level & 1U) != 0
                                              ? HB_DIRECTION_RTL
                                              : HB_DIRECTION_LTR);
    hb_buffer_set_script(buffer.get(), first.script);
    hb_buffer_set_cluster_level(buffer.get(),
                                HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_guess_segment_properties(buffer.get());
    detail::recordHarfBuzz(first.font != primary);
    hb_shape(first.font->hbFont(), buffer.get(), nullptr, 0);
    unsigned int count{};
    const auto *info      = hb_buffer_get_glyph_infos(buffer.get(), &count);
    const auto *positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
    for (unsigned int i = 0; i < count; i++) {
      advances.push_back(Advance{
          .byte = info[i].cluster,
          .width =
              static_cast<float>(positions[i].x_advance) / kFixed26Dot6Scale,
      });
    }
    begin = end;
  }
  return advances;
}

struct Atom {
  std::size_t begin{};
  std::size_t end{};
  float width{};
  FriBidiParType paragraph{FRIBIDI_PAR_LTR};
  bool newline{};
  bool breakAfter{};
};

std::vector<Atom> atomsFor(const std::string_view text,
                           const std::vector<Character> &characters,
                           const std::vector<Advance> &advances,
                           const std::vector<char> &graphemes) {
  std::vector<std::size_t> starts{0, text.size()};
  for (const auto &advance : advances) {
    if (boundary(graphemes, advance.byte)) {
      starts.push_back(advance.byte);
    }
  }
  // A newline is a flow boundary, even when a shaping engine suppresses its
  // glyph. Its following byte must remain a possible next-line start.
  for (const auto &character : characters) {
    if (character.value == '\n') {
      starts.push_back(character.byte);
      starts.push_back(character.byte + 1);
    }
  }
  std::ranges::sort(starts);
  starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
  std::vector<char> breaks(text.size());
  set_linebreaks_utf8(reinterpret_cast<const utf8_t *>(text.data()),
                      text.size(), nullptr, breaks.data());
  std::vector<Atom> atoms;
  std::size_t characterIndex = 0;
  for (std::size_t i = 0; i + 1 < starts.size(); i++) {
    while (characterIndex + 1 < characters.size() &&
           characters[characterIndex + 1].byte <= starts[i]) {
      characterIndex++;
    }
    atoms.push_back(Atom{
        .begin      = starts[i],
        .end        = starts[i + 1],
        .paragraph  = characters[characterIndex].paragraph,
        .newline    = text[starts[i]] == '\n',
        .breakAfter = breaks[starts[i + 1] - 1] == LINEBREAK_ALLOWBREAK ||
                      breaks[starts[i + 1] - 1] == LINEBREAK_MUSTBREAK,
    });
  }
  for (const auto &advance : advances) {
    const auto found =
        std::upper_bound(starts.begin(), starts.end(), advance.byte);
    const auto index = static_cast<std::size_t>(found - starts.begin() - 1);
    if (index < atoms.size()) {
      atoms[index].width += advance.width;
    }
  }
  for (auto &atom : atoms) {
    atom.width = std::max(0.0F, atom.width);
  }
  return atoms;
}

struct Marker {
  std::string text;
  float width{};
};

Marker markerFor(const FontFacePtr &font) {
  Marker marker{.text = "\xE2\x80\xA6"};
  auto selected = font;
  hb_codepoint_t glyph{};
  if (!hb_font_get_nominal_glyph(font->hbFont(), 0x2026U, &glyph)) {
    const auto fallback =
        FontManager::instance().getFallbackFont(font, 0x2026U);
    if (fallback &&
        hb_font_get_nominal_glyph((*fallback)->hbFont(), 0x2026U, &glyph)) {
      selected = *fallback;
    } else {
      marker.text = "...";
    }
  }
  Buffer buffer{hb_buffer_create(), &hb_buffer_destroy};
  hb_buffer_add_utf8(buffer.get(), marker.text.data(),
                     static_cast<int>(marker.text.size()), 0,
                     static_cast<int>(marker.text.size()));
  hb_buffer_guess_segment_properties(buffer.get());
  detail::recordHarfBuzz(selected != font);
  hb_shape(selected->hbFont(), buffer.get(), nullptr, 0);
  unsigned int count{};
  const auto *positions = hb_buffer_get_glyph_positions(buffer.get(), &count);
  for (unsigned int i = 0; i < count; i++) {
    marker.width +=
        static_cast<float>(positions[i].x_advance) / kFixed26Dot6Scale;
  }
  return marker;
}

struct Piece {
  const Atom *atom{};
  std::size_t sourceByte{};
  float width{};
};

std::vector<Piece> selectLine(const std::vector<Atom> &atoms,
                              const std::size_t begin, const std::size_t end,
                              const float maxWidth, const bool ellipsize,
                              const EllipsisAt at, const Marker &marker,
                              bool &truncated) {
  float width = 0.0F;
  for (auto i = begin; i < end; i++) {
    width += atoms[i].width;
  }
  const bool omit = ellipsize || width > maxWidth;
  truncated       = truncated || omit;
  std::vector<Piece> pieces;
  if (!omit) {
    for (auto i = begin; i < end; i++) {
      pieces.push_back(Piece{.atom       = &atoms[i],
                             .sourceByte = atoms[i].begin,
                             .width      = atoms[i].width});
    }
    return pieces;
  }
  const float markerWidth = ellipsize ? marker.width : 0.0F;
  if (markerWidth > maxWidth) {
    return pieces;
  }
  const float budget  = maxWidth - markerWidth;
  auto prefixEnd      = begin;
  auto suffixBegin    = end;
  float used          = 0.0F;
  const auto position = ellipsize ? at : EllipsisAt::End;
  if (position == EllipsisAt::End) {
    while (prefixEnd < end && used + atoms[prefixEnd].width <= budget) {
      used += atoms[prefixEnd++].width;
    }
    if (!ellipsize && prefixEnd < end) {
      // Canvas needs the intersecting cluster to crop its edge quad.
      ++prefixEnd;
    }
  } else if (position == EllipsisAt::Start) {
    while (suffixBegin > begin &&
           used + atoms[suffixBegin - 1].width <= budget) {
      used += atoms[--suffixBegin].width;
    }
  } else {
    // Alternate sides according to their accumulated widths, retaining useful
    // context on both sides of identifiers without repeated shaping.
    float prefixWidth = 0.0F;
    float suffixWidth = 0.0F;
    while (prefixEnd < suffixBegin) {
      const bool takePrefix = prefixWidth <= suffixWidth;
      const auto index      = takePrefix ? prefixEnd : suffixBegin - 1;
      if (used + atoms[index].width > budget) {
        const auto other = takePrefix ? suffixBegin - 1 : prefixEnd;
        if (used + atoms[other].width > budget) {
          break;
        }
        if (takePrefix) {
          suffixWidth += atoms[--suffixBegin].width;
        } else {
          prefixWidth += atoms[prefixEnd++].width;
        }
        used += atoms[other].width;
      } else {
        used += atoms[index].width;
        if (takePrefix) {
          prefixWidth += atoms[prefixEnd++].width;
        } else {
          suffixWidth += atoms[--suffixBegin].width;
        }
      }
    }
  }
  for (auto i = begin; i < prefixEnd; i++) {
    pieces.push_back(Piece{.atom       = &atoms[i],
                           .sourceByte = atoms[i].begin,
                           .width      = atoms[i].width});
  }
  const auto cutByte =
      prefixEnd < atoms.size() ? atoms[prefixEnd].begin : atoms.back().end;
  if (ellipsize) {
    pieces.push_back(Piece{.sourceByte = cutByte, .width = marker.width});
  }
  for (auto i = suffixBegin; i < end; i++) {
    pieces.push_back(Piece{.atom       = &atoms[i],
                           .sourceByte = atoms[i].begin,
                           .width      = atoms[i].width});
  }
  return pieces;
}

std::vector<std::size_t> visualOrder(const std::vector<Piece> &pieces,
                                     const std::string_view text,
                                     const Marker &marker,
                                     FriBidiParType paragraph) {
  std::vector<FriBidiChar> values;
  std::vector<std::size_t> pieceForCharacter;
  for (std::size_t i = 0; i < pieces.size(); i++) {
    const auto &piece  = pieces[i];
    const auto content = piece.atom != nullptr
                             ? text.substr(piece.atom->begin,
                                           piece.atom->end - piece.atom->begin)
                             : std::string_view{marker.text};
    for (const auto &character : decode(content)) {
      values.push_back(character.value);
      pieceForCharacter.push_back(i);
    }
  }
  std::vector<std::size_t> order;
  if (values.empty()) {
    return order;
  }
  const auto count = static_cast<FriBidiStrIndex>(values.size());
  std::vector<FriBidiCharType> types(values.size());
  std::vector<FriBidiBracketType> brackets(values.size());
  std::vector<FriBidiLevel> levels(values.size());
  std::vector<FriBidiStrIndex> map(values.size());
  std::iota(map.begin(), map.end(), FriBidiStrIndex{0});
  fribidi_get_bidi_types(values.data(), count, types.data());
  fribidi_get_bracket_types(values.data(), count, types.data(),
                            brackets.data());
  if (fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), count,
                                          &paragraph, levels.data()) == 0 ||
      fribidi_reorder_line(FRIBIDI_FLAGS_DEFAULT, types.data(), count, 0,
                           paragraph, levels.data(), nullptr,
                           map.data()) == 0) {
    throw std::runtime_error("Cannot reorder fitted text");
  }
  std::vector<bool> seen(pieces.size());
  for (const auto character : map) {
    const auto piece = pieceForCharacter[static_cast<std::size_t>(character)];
    if (!seen[piece]) {
      seen[piece] = true;
      order.push_back(piece);
    }
  }
  return order;
}

struct Line {
  std::size_t begin{};
  std::size_t end{};
  std::size_t consumed{};
};

std::vector<Line> wrapLines(const std::vector<Atom> &atoms,
                            const float maxWidth, const std::size_t maxLines) {
  std::vector<Line> lines;
  for (std::size_t begin = 0;
       begin < atoms.size() && lines.size() < maxLines;) {
    auto end       = begin;
    auto lastBreak = begin;
    float width    = 0.0F;
    while (end < atoms.size() && !atoms[end].newline) {
      if (width + atoms[end].width > maxWidth && end > begin) {
        if (lastBreak > begin) {
          end = lastBreak;
        }
        break;
      }
      width += atoms[end].width;
      end++;
      if (atoms[end - 1].breakAfter) {
        lastBreak = end;
      }
    }
    const auto consumed =
        end < atoms.size() && atoms[end].newline ? end + 1 : end;
    lines.push_back(Line{.begin = begin, .end = end, .consumed = consumed});
    begin = consumed;
  }
  return lines;
}

float alignedLeft(const float width, const float limit,
                  const gleditor::TextAlign align) {
  if (!std::isfinite(limit)) {
    return 0.0F;
  }
  switch (align) {
  case gleditor::TextAlign::Right:
    return limit - width;
  case gleditor::TextAlign::Centre:
    return (limit - width) / 2.0F;
  case gleditor::TextAlign::Left:
  case gleditor::TextAlign::Justify:
    return 0.0F;
  }
  return 0.0F;
}

} // namespace

FittedText fit(const std::string_view source, const FontFacePtr &font,
               const TextFit &constraints, ShapingCache *cache) {
  if (cache != nullptr) {
    return cache->fitted(source, font, constraints);
  }
  detail::recordLayout(source.size());
  FittedText result;
  if (source.empty() || !font || !font->hbFont()) {
    return result;
  }
  if (source.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("Text fitting input exceeds HarfBuzz's byte limit");
  }
  detail::initializeUnicodeBreaks();
  const float lineHeight = std::max(1.0F, font->metrics().lineHeight);
  std::size_t maxLines   = constraints.maxLines != 0
                               ? constraints.maxLines
                               : std::numeric_limits<std::uint16_t>::max();
  if (constraints.overflow != Overflow::Wrap) {
    maxLines = std::min(maxLines, std::size_t{1});
  }
  if (constraints.maxHeightPx > 0.0F &&
      std::isfinite(constraints.maxHeightPx)) {
    const auto heightLines = std::floor(constraints.maxHeightPx / lineHeight);
    if (heightLines < static_cast<float>(maxLines)) {
      maxLines = static_cast<std::size_t>(heightLines);
    }
  }
  if (maxLines == 0) {
    result.truncated = true;
    return result;
  }
  const float maxWidth = constraints.maxWidthPx > 0.0F
                             ? constraints.maxWidthPx
                             : std::numeric_limits<float>::infinity();
  std::string normalized;
  auto text = source;
  if (constraints.overflow != Overflow::Wrap &&
      source.find_first_of("\r\n") != std::string_view::npos) {
    normalized.assign(source);
    std::ranges::replace(normalized, '\n', ' ');
    std::ranges::replace(normalized, '\r', ' ');
    text = normalized;
  }
  std::vector<char> graphemes(text.size());
  set_graphemebreaks_utf8(reinterpret_cast<const utf8_t *>(text.data()),
                          text.size(), nullptr, graphemes.data());
  auto characters = decode(text);
  detail::retainConjuncts(characters, graphemes);
  resolveCharacters(characters, graphemes, font);
  const auto advances = shapeRuns(text, characters, graphemes, font);
  const auto atoms    = atomsFor(text, characters, advances, graphemes);
  const auto marker =
      constraints.overflow == Overflow::Clip ? Marker{} : markerFor(font);
  auto lines = constraints.overflow == Overflow::Wrap
                   ? wrapLines(atoms, maxWidth, maxLines)
                   : std::vector<Line>{
                         Line{.end = atoms.size(), .consumed = atoms.size()}};
  std::vector<const Atom *> retained;
  float rightmost = 0.0F;
  for (std::size_t lineIndex = 0; lineIndex < lines.size(); lineIndex++) {
    const auto &line = lines[lineIndex];
    const bool hidden =
        lineIndex + 1 == lines.size() && line.consumed < atoms.size();
    float fullWidth = 0.0F;
    for (auto i = line.begin; i < line.end; i++) {
      fullWidth += atoms[i].width;
    }
    const bool ellipsize = constraints.overflow != Overflow::Clip &&
                           (hidden || fullWidth > maxWidth);
    const auto pieces =
        selectLine(atoms, line.begin, line.end, maxWidth, ellipsize,
                   constraints.overflow == Overflow::Wrap ? EllipsisAt::End
                                                          : constraints.at,
                   marker, result.truncated);
    float width = 0.0F;
    for (const auto &piece : pieces) {
      width += piece.width;
      if (piece.atom != nullptr &&
          (constraints.overflow != Overflow::Clip || width <= maxWidth)) {
        retained.push_back(piece.atom);
      }
    }
    const float visibleWidth = constraints.overflow == Overflow::Clip
                                   ? std::min(width, maxWidth)
                                   : width;
    const float left = alignedLeft(visibleWidth, maxWidth, constraints.align);
    const float top  = static_cast<float>(lineIndex) * lineHeight;
    const auto paragraph = atoms[line.begin].paragraph;
    float pen            = left;
    if (constraints.overflow == Overflow::Clip && width > maxWidth &&
        FRIBIDI_IS_RTL(paragraph)) {
      pen -= width - maxWidth;
    }
    for (const auto index : visualOrder(pieces, text, marker, paragraph)) {
      const auto &piece = pieces[index];
      const auto content =
          piece.atom != nullptr
              ? text.substr(piece.atom->begin,
                            piece.atom->end - piece.atom->begin)
              : std::string_view{marker.text};
      const auto cluster = result.shaping.clusters.size();
      result.shaping.clusters.push_back(ClusterBox{
          .byteStart  = static_cast<std::uint32_t>(piece.sourceByte),
          .byteLength = static_cast<std::uint32_t>(
              piece.atom != nullptr ? piece.atom->end - piece.atom->begin : 0),
          .charCount = static_cast<std::uint32_t>(
              piece.atom != nullptr ? decode(content).size() : 0),
      });
      result.shaping.glyphs.push_back(PageShaping::GlyphEntry{
          .chr          = std::string{content},
          .clusterLeft  = pen,
          .clusterTop   = top,
          .clusterIndex = cluster,
          .lineIndex    = lineIndex,
      });
      pen += piece.width;
    }
    auto startByte   = atoms[line.begin].begin;
    auto endByte     = startByte;
    bool foundSource = false;
    for (const auto &piece : pieces) {
      if (piece.atom != nullptr) {
        if (!foundSource) {
          startByte   = piece.atom->begin;
          foundSource = true;
        }
        endByte = piece.atom->end;
      }
    }
    if (line.consumed > line.end && !hidden && !ellipsize) {
      endByte = atoms[line.end].end;
    }
    result.shaping.lines.push_back(PageShaping::LineEntry{
        .barWidth   = visibleWidth,
        .barHeight  = lineHeight,
        .left       = left,
        .top        = top,
        .lineIndex  = lineIndex,
        .byteStart  = static_cast<std::uint32_t>(startByte),
        .byteLength = static_cast<std::uint32_t>(endByte - startByte),
    });
    result.widthPx = std::max(result.widthPx, visibleWidth);
    rightmost      = std::max(rightmost, left + visibleWidth);
    // A consumed newline belongs to the visible prefix, despite having no ink.
    if (line.consumed > line.end && !hidden && !ellipsize) {
      retained.push_back(&atoms[line.end]);
    }
  }
  std::ranges::sort(retained, {}, &Atom::begin);
  for (const auto *atom : retained) {
    if (atom->begin != result.visibleBytes) {
      break;
    }
    result.visibleBytes = atom->end;
  }
  result.lines                = static_cast<std::uint16_t>(lines.size());
  result.heightPx             = static_cast<float>(lines.size()) * lineHeight;
  result.shaping.limit        = result.visibleBytes;
  result.shaping.lineCount    = lines.size();
  result.shaping.textWidthPx  = static_cast<int>(std::ceil(rightmost));
  result.shaping.textHeightPx = static_cast<int>(std::ceil(result.heightPx));
  return result;
}

} // namespace gleditor::text
