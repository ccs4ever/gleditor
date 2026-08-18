/**
 * @file link_layout.hpp
 * @brief Which links run between which of the documents on screen.
 *
 * A link attaches to content rather than to a position: "a link to any portion
 * is present on all manifestations". So where its two ends have come to rest is
 * not something the link knows -- it is a question about what the open versions
 * are made of, and the answer changes when somebody quotes a passage into a
 * document the link has never heard of.
 *
 * That question is decidable from the store alone, which is why it is here
 * rather than beside the drawing. Given versions and links, these are the
 * connections; turning one into a ribbon between two points in space is the
 * display's business and needs a device, and none of the rules do.
 */
#ifndef XUDU_LINK_LAYOUT_H
#define XUDU_LINK_LAYOUT_H

#include <cstdint>
#include <map>
#include <vector>

#include "ops.hpp"
#include "spool.hpp"
#include "version.hpp"

namespace xudu {

/// One end of a link, found in a document that is open.
struct LinkEnd {
  std::uint32_t doc{};   ///< Index among the open documents.
  std::uint32_t start{}; ///< Byte range of the content within that document.
  std::uint32_t end{};
  bool operator==(const LinkEnd &) const = default;
};

/// A link whose two ends both landed in open documents: one connection to
/// draw.
struct LinkedPair {
  std::uint64_t link{};
  LinkType type{LinkType::Comment};
  LinkEnd from; ///< The left list, which is the end the link was attached at.
  LinkEnd to;   ///< The right list, which is what it points at.
};

/// A link with one end in an open document and the other in none -- the
/// ordinary case, since a link is made to content and not to whatever happens
/// to be open.
struct HalfLink {
  std::uint64_t link{};
  LinkEnd here;
  /// The spans of the end that is nowhere, for finding a version showing it.
  std::vector<PrimediaSpan> elsewhere;
};

/**
 * @brief Sort @p links into the ones that run between @p views and the ones
 *        that run off them.
 *
 * At most one place per document per side. A link end quoted twice into one
 * document is still one relation, and reporting it twice would claim something
 * the link does not say; the extent handed back covers every occurrence, so
 * whatever is drawn lands within what the link points at.
 *
 * Links with both ends in one document are reported as neither: they are
 * shown by shading both passages, and a connection drawn between them would
 * run back across the text it connects.
 *
 * @param views One version per open document, in document order. A null entry
 *        is a document that has nothing to say yet.
 */
void placeLinks(const std::map<std::uint64_t, Link> &links,
                const std::vector<const Version *> &views,
                std::vector<LinkedPair> &between,
                std::vector<HalfLink> &leaving);

/**
 * @brief Colour a link of @p type is shown in, as packed RGBA8.
 *
 * Types are told apart by colour rather than by a label, because a label on a
 * connection between two documents would have to be legible at whatever angle
 * the connection runs at. Not opaque: what a link crosses is the space between
 * two documents, and it is not the thing being read.
 */
[[nodiscard]] std::uint32_t linkColour(LinkType type);

} // namespace xudu

#endif // XUDU_LINK_LAYOUT_H
