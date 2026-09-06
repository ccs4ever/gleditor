#include "format.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace xudu {

const char *formatAttributeName(const FormatAttribute attribute) {
  switch (attribute) {
  case FormatAttribute::Italic:
    return "italic";
  case FormatAttribute::Bold:
    return "bold";
  case FormatAttribute::Underline:
    return "underline";
  case FormatAttribute::Overline:
    return "overline";
  case FormatAttribute::Strikethrough:
    return "strikethrough";
  case FormatAttribute::Superscript:
    return "superscript";
  case FormatAttribute::Subscript:
    return "subscript";
  case FormatAttribute::AlignLeft:
    return "align-left";
  case FormatAttribute::AlignCentre:
    return "align-centre";
  case FormatAttribute::AlignRight:
    return "align-right";
  case FormatAttribute::AlignJustify:
    return "align-justify";
  }
  return "italic";
}

namespace {

/// Every attribute's word, concatenated in allFormatAttributes order with
/// nothing between them: vocabularySpanFor() and readVocabulary() only ever
/// address exact [offset, +length) slices of this, so no separator is ever
/// read as part of a word and none is needed.
const std::string &vocabularyText() {
  static const std::string text = [] {
    std::string built;
    for (const auto attribute : allFormatAttributes) {
      built += formatAttributeName(attribute);
    }
    return built;
  }();
  return text;
}

} // namespace

PrimediaSpan vocabularySpanFor(const FormatAttribute attribute) {
  std::uint64_t offset = 0;
  for (const auto candidate : allFormatAttributes) {
    const std::string_view name = formatAttributeName(candidate);
    if (candidate == attribute) {
      return PrimediaSpan{vocabularyScroll, offset, name.size()};
    }
    offset += name.size();
  }
  return PrimediaSpan{vocabularyScroll, 0, 0}; // unreachable: enum exhausted
}

std::optional<std::string> readVocabulary(const PrimediaSpan &span) {
  if (vocabularyScroll != span.scroll) {
    return std::nullopt;
  }
  const auto &text = vocabularyText();
  if (span.start >= text.size()) {
    return std::string{};
  }
  const auto available = text.size() - span.start;
  return text.substr(span.start,
                     std::min<std::uint64_t>(span.length, available));
}

std::optional<FormatAttribute>
formatAttributeFromDecoration(const gleditor::Decoration decoration) {
  switch (decoration) {
  case gleditor::Decoration::Bold:
    return FormatAttribute::Bold;
  case gleditor::Decoration::Italic:
    return FormatAttribute::Italic;
  case gleditor::Decoration::Underline:
    return FormatAttribute::Underline;
  case gleditor::Decoration::Overline:
    return FormatAttribute::Overline;
  case gleditor::Decoration::Strikethrough:
    return FormatAttribute::Strikethrough;
  case gleditor::Decoration::Superscript:
    return FormatAttribute::Superscript;
  case gleditor::Decoration::Subscript:
    return FormatAttribute::Subscript;
  }
  return std::nullopt;
}

std::optional<FormatAttribute>
formatAttributeFromTextAlign(const gleditor::TextAlign align) {
  switch (align) {
  case gleditor::TextAlign::Left:
    return FormatAttribute::AlignLeft;
  case gleditor::TextAlign::Centre:
    return FormatAttribute::AlignCentre;
  case gleditor::TextAlign::Right:
    return FormatAttribute::AlignRight;
  case gleditor::TextAlign::Justify:
    return FormatAttribute::AlignJustify;
  }
  return std::nullopt;
}

std::optional<gleditor::TextAlign>
textAlignFromFormatAttribute(const FormatAttribute attribute) {
  switch (attribute) {
  case FormatAttribute::AlignLeft:
    return gleditor::TextAlign::Left;
  case FormatAttribute::AlignCentre:
    return gleditor::TextAlign::Centre;
  case FormatAttribute::AlignRight:
    return gleditor::TextAlign::Right;
  case FormatAttribute::AlignJustify:
    return gleditor::TextAlign::Justify;
  default:
    return std::nullopt;
  }
}

std::optional<gleditor::Decoration>
decorationFromFormatAttribute(const FormatAttribute attribute) {
  switch (attribute) {
  case FormatAttribute::Bold:
    return gleditor::Decoration::Bold;
  case FormatAttribute::Italic:
    return gleditor::Decoration::Italic;
  case FormatAttribute::Underline:
    return gleditor::Decoration::Underline;
  case FormatAttribute::Overline:
    return gleditor::Decoration::Overline;
  case FormatAttribute::Strikethrough:
    return gleditor::Decoration::Strikethrough;
  case FormatAttribute::Superscript:
    return gleditor::Decoration::Superscript;
  case FormatAttribute::Subscript:
    return gleditor::Decoration::Subscript;
  default:
    return std::nullopt;
  }
}

} // namespace xudu
