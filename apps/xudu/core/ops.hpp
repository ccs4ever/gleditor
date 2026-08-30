/**
 * @file ops.hpp
 * @brief The reversible operations, and the links they can make.
 *
 * OSMIC's defining document lists the hyperops: INSERT, REARRANGE, DELETE
 * ("actually REARRANGE TO LIMBO"), TRANSCLUDE, MAKE/CHANGE LINK, MAKE/CHANGE
 * STRUCTURE MAP. Each is recorded in the operations spool, the second of the
 * two, in a form that "permits replaying the operation" -- so an op names
 * where in the document it acted and which primedia it acted with, never the
 * text itself. The text went into the primedia spool when it was first typed
 * and stays there.
 *
 * Every op also names the state it applies to. That is what makes time a graph
 * rather than a line: two ops naming the same parent are two futures of the
 * same document, and neither destroys the other.
 */
#ifndef XUDU_OPS_H
#define XUDU_OPS_H

#include <cstdint>
#include <string>
#include <vector>

#include "microversion.hpp"
#include "spool.hpp"

namespace xudu {

/// Which of the hyperops this is.
enum class OpKind : std::uint8_t {
  Insert,     ///< Put new address pointers into the sequence.
  Delete,     ///< Rearrange to limbo: stop pointing at content.
  Rearrange,  ///< Move pointers within the sequence.
  Transclude, ///< Insert pointers to material another version already uses.
  Link,       ///< Make a connection between spans of content.
  /// Force a page break at a point in this version's own text. Not one of
  /// OSMIC's hyperops -- an addition beyond the six it names -- because
  /// nothing in OSMIC needs a concept that is deliberately NOT a primedia
  /// address. A Link's ends survive transclusion by design: content quoted
  /// into a second document carries its links with it, because a link
  /// attaches to the address, not the document. A page break must not do
  /// that. Where a version breaks into pages is a fact about that version's
  /// own assembled text -- its concatext -- and a passage quoted into some
  /// other document, laid out alongside completely different neighbours, has
  /// no reason to break in the same place. So this names a position with
  /// Op::at, exactly like Insert or Delete, rather than a PrimediaSpan: it is
  /// concatext-relative and content-address-less on purpose, and Version
  /// carries it as a zero-length marker so it is re-split, shifted and
  /// moved by ordinary edits without ever being resolvable back to an
  /// address the way a real piece is.
  PageBreak,
};

const char *opKindName(OpKind kind);

/// How two ends of a link relate. Nelson's examples, plus quotation, which is
/// what a transclusion made deliberately amounts to.
enum class LinkType : std::uint8_t {
  Comment,
  Illustration,
  Disagreement,
  Authorship,
  Quotation,
  Other,
  /**
   * A presentation attribute, attached the same way any other link attaches
   * to content. Nelson's own answer to "where does formatting live" -- see
   * format.hpp for FormatAttribute and how the right end names one without
   * Link needing a field no other link type has. Unlike OpKind::PageBreak,
   * this is exactly Nelson's link, and it gets exactly Nelson's guarantee for
   * free: attach italics to a span once, and it shows up wherever that span
   * is quoted, because the link is attached to the address and not to any
   * one document's position. That is also why a page break could not be one
   * of these -- see OpKind::PageBreak for why that had to go the other way.
   */
  Format,
  /**
   * A Project Xanadu ZigZag 2-rank dimensional manifold link. Left span
   * connects to Right span along the dimension specified in Link::owner
   * ("dimension:d.1" or "d.1").
   */
  Dimension,
};

const char *linkTypeName(LinkType type);
LinkType linkTypeFromName(const std::string &name);

/// The prominence hierarchy of a link.
enum class ProminenceTier : std::uint8_t {
  Author = 0, ///< Bundled with the author's document publication (highest
              ///< prominence).
  Curated =
      1, ///< From the user's subscribed curator graph (secondary prominence).
  Public =
      2, ///< Discovered via public DHT swarm rendezvous (bounded / tertiary).
};

const char *prominenceTierName(ProminenceTier tier);

/**
 * @brief A butterfly link: two lists of spans, an identity and a type.
 *
 * "We represent a link as two lists, a Left List and a Right List, plus
 * address & type." The ends are primedia spans rather than positions in a
 * document, and that is the whole point: a link made to a passage is attached
 * to the content, so it shows up on every version that quotes that passage and
 * survives editing around it. Nelson's criterion is that a "link to any
 * portion is present on all manifestations", which a link naming document
 * offsets could not satisfy.
 *
 * Pluralism is a consequence of there being nowhere else to put a link.
 * "THE AUTHOR'S LINKS ARE NO DIFFERENT FROM ANYONE ELSE'S IN IMPLEMENTATION
 * (though superior in prestige and legitimacy.)" The owner is recorded, and is
 * read by nothing here that decides what may be done.
 */
struct Link {
  std::uint64_t id{};
  LinkType type{LinkType::Comment};
  ProminenceTier tier{ProminenceTier::Author};
  /// Who made it. Prestige, not permission.
  std::string owner;
  /// Curator or publisher key if from a third-party link package.
  std::string curator;
  std::vector<PrimediaSpan> left;
  std::vector<PrimediaSpan> right;

  /// Whether either end covers any of @p span.
  [[nodiscard]] bool touches(const PrimediaSpan &span) const;
};

/**
 * @brief One recorded operation.
 *
 * A single struct rather than a variant hierarchy because an op is written to
 * and read from the spool as one line, and the fields a kind does not use are
 * simply zero. Which fields matter per kind is spelled out on each below.
 */
struct Op {
  OpKind kind{OpKind::Insert};
  /// The state this op is applied to. The state it produces is named by the
  /// id it is filed under, which is where the graph comes from.
  MicroversionId parent;

  /// Insert, Delete, Rearrange, Transclude: where in the version it acts.
  std::uint32_t at{};
  /// Delete, Rearrange: how much.
  std::uint32_t length{};
  /// Rearrange: where it goes.
  std::uint32_t to{};
  /// Insert: the content, already in the primedia spool.
  PrimediaSpan span;
  /// Transclude: which version the material is taken from, and where in it.
  /// The spans are resolved against that version when the op is replayed, so
  /// what is transcluded is content rather than a position.
  MicroversionId source;
  std::uint32_t sourceAt{};
  std::uint32_t sourceLength{};
  /// Link: which link was made.
  std::uint64_t link{};
};

} // namespace xudu

#endif // XUDU_OPS_H
