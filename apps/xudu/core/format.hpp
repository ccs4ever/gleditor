/**
 * @file format.hpp
 * @brief Presentation attributes, named the way any other link end is.
 *
 * Nelson's own answer to "where does formatting live" is that it is a link
 * like any other: one end is the content, and the other is a specifier for
 * how to show it. Neither end needs a kind Link does not already have --
 * PrimediaSpan is a content address, and a specifier is just a very short
 * piece of content, so the right end of a LinkType::Format link is a span
 * into a fixed vocabulary of attribute names rather than a new field grafted
 * onto Link for this one link type.
 *
 * That vocabulary is spool.hpp's vocabularyScroll: a system scroll, resolved
 * from this file's own compiled-in words rather than written into any one
 * store's local spool. vocabularySpanFor() is therefore a pure function of
 * the attribute -- the same span for every store, every session, on every
 * machine that has this file compiled in -- which is what makes a format
 * link genuinely portable rather than only recognisable where it was made:
 * attach italics to a span once, and it is recognised on every version
 * quoting that span in this store, in a store that adopted it through a
 * LinkPackage, wherever. Store::formatAttributeOf() recognises one back by
 * comparing addresses, which now works, rather than by reading bytes and
 * matching text, which is what an earlier version of this file did before
 * the vocabulary had a portable address to compare against.
 *
 * Rendering the parts of formatting that are a background-colour problem
 * reuses gleditor::SpanDecorator; italic and bold are not that, and need the
 * shaping path instead.
 */
#ifndef XUDU_FORMAT_H
#define XUDU_FORMAT_H

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <gleditor/glyphcache/types.hpp>

#include "spool.hpp"

namespace xudu {

/**
 * @brief A presentation attribute a LinkType::Format link can name.
 *
 * A closed enum rather than an arbitrary string: recognising which attribute
 * a format link names (see Store::formatAttributeOf()) is then an address
 * comparison against a handful of known spans, not a parser for whatever
 * text happened to be quoted. Bold and italic have real font-file variants a
 * rasteriser can prefer when one exists; the rest are always synthesised.
 * Superscript and subscript's baseline shift is layout-time positioning, not
 * something baked into the glyph bitmap -- see doc.cpp's page layout -- so
 * what they name here is only the reduced size a rasteriser renders at.
 *
 * Only ever append here, and never reorder or remove an existing entry:
 * vocabularySpanFor() places each attribute's word end to end in
 * declaration order, so inserting one in the middle would move every later
 * attribute's address, and a format link already saved with the old address
 * would silently stop being recognised.
 */
enum class FormatAttribute : std::uint8_t {
  Italic,
  Bold,
  Underline,
  Overline,
  Strikethrough,
  Superscript,
  Subscript,
};

/// Every attribute, in the declaration order vocabularySpanFor() lays their
/// words out in. The one place that order is spelled out, so nothing else
/// has to repeat it.
inline constexpr std::array<FormatAttribute, 7> allFormatAttributes{
    FormatAttribute::Italic,        FormatAttribute::Bold,
    FormatAttribute::Underline,     FormatAttribute::Overline,
    FormatAttribute::Strikethrough, FormatAttribute::Superscript,
    FormatAttribute::Subscript,
};

/// The vocabulary text an attribute is named as. See allFormatAttributes and
/// FormatAttribute's own comment for why this may only ever gain new
/// entries at the end.
const char *formatAttributeName(FormatAttribute attribute);

/**
 * @brief The span naming @p attribute in the vocabulary scroll.
 *
 * A pure function of @p attribute: the same span comes back on every call,
 * in every process, on every machine this is compiled on, because it names
 * an offset into vocabularyScroll rather than anything written to a
 * per-store spool. That is the whole difference from an ordinary quotation's
 * address, and it is what a format link needs to be portable.
 */
[[nodiscard]] PrimediaSpan vocabularySpanFor(FormatAttribute attribute);

/**
 * @brief The bytes at @p span, if it addresses the vocabulary scroll and
 *        falls within it.
 *
 * What Store::read() calls so a vocabulary span reads back its word the same
 * way any other span reads back its content. Nothing but Store::read() ought
 * to need this directly.
 */
[[nodiscard]] std::optional<std::string>
readVocabulary(const PrimediaSpan &span);

/**
 * @brief The FormatAttribute @p decoration names, or nullopt if none does.
 *
 * gleditor::Decoration (the base library's rasterisation-facing concept, used
 * by --type's automation syntax and GlyphCache) and FormatAttribute (this
 * file's content-addressed, portable one) name the same set of words on
 * purpose, but are kept as separate enums so that neither is forced to
 * change shape for the other's reasons. This is the one place that
 * correspondence is spelled out; nothing relies on the two enums' numeric
 * values lining up.
 */
[[nodiscard]] std::optional<FormatAttribute>
formatAttributeFromDecoration(gleditor::Decoration decoration);

} // namespace xudu

#endif // XUDU_FORMAT_H
