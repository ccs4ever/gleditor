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

namespace xanadu {

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
  /**
   * OSMIC's sixth hyperop: MAKE/CHANGE STRUCTURE MAP. Establishes the
   * coordinate frame the other five operate inside.
   *
   * Note the shape of the five above. Insert, Delete and Rearrange all name a
   * position in a document; Transclude names a position in another one; Link
   * names content addresses. Five of six are edits expressed *through* a
   * coordinate system. This is the edit *of* one: a document is a map from
   * virtual document space onto invariant stream addresses, "structure map"
   * is the literal name of that object in Udanax Green rather than a
   * metaphor, and this is what creates or alters one.
   *
   * A page break is the degenerate one-dimensional case -- a break partitions
   * a concatext into an ordered sequence of subsequences, which is a
   * structure map with a fixed name and no content of its own -- and
   * deliberately is *not* this. See OpKind::PageBreak for why a break must
   * not travel with a quotation where a cell boundary must.
   *
   * Discriminated further by CompactOpNode::flags rather than by sibling
   * OpKind values: the verbs share every field and differ only in which ones
   * they read, so nothing outside the manifold fold wants to tell them apart.
   * That is a design choice defended on its merits and no longer a forced
   * move -- CompactBinaryV3 widened the wire format's kind field to four bits
   * (migration step 10), so BinStructure = 7 no longer fills it.
   *
   * Nothing emits one yet. Store::replay() treats it as a text no-op, because
   * a slice's structure is a *second* replay product of the same spool -- see
   * design/store-slice-convergence.md §3 and R1.
   */
  Structure,
};

const char *opKindName(OpKind kind);

/**
 * @name The Structure family, in CompactOpNode::flags
 *
 * A Structure op's verb, and what its `value` field holds. `flags` is
 * otherwise unread anywhere in the tree, which is why this family takes it.
 * @{
 */

/// bits 0-2: which Structure verb this is.
inline constexpr std::uint8_t structureVerbMask = 0x07;

enum class StructureVerb : std::uint8_t {
  /// Mint a cell. The span is its content; `to` and `linkId` are unused.
  MakeCell = 0,
  /// (this cell, dimension = linkId, direction) -> `to`. `to == 0` clears it.
  SetLink = 1,
  /// Retarget this cell's content span, its typed value, or both.
  SetValue = 2,
  /**
   * Splice `span` into this cell's content at offset `at`, replacing `length`
   * bytes of what is there -- an insert when `length` is zero, a delete when
   * the span is empty, and a replacement otherwise.
   *
   * The verb that makes an edit an *edit*. SetValue restates a cell's whole
   * content, so changing one byte of a thousand moved all thousand to a new
   * address and severed every transclusion that shared the old one; a splice
   * touches only the piece it lands in and leaves every other address exactly
   * where it was. See U3 in design/store-slice-convergence.md.
   *
   * `at` is an offset within *this cell's* content rather than the document's
   * concatext, which is what §2 means by a structure map establishing the
   * frame the other hyperops' coordinates are meaningful in. It is the one
   * Structure verb for which `at` is not zero.
   */
  Splice = 3,
  // No MakeDim: a dimension is a cell on the d.dims rank, so minting one is
  // MakeCell plus SetLink and needs no verb of its own. See design R12.
};

/// bit 3: which way along the dimension a SetLink points.
inline constexpr std::uint8_t structureNegward = 0x08;

/// bits 4-6: what CompactOpNode::value holds.
inline constexpr std::uint8_t valueKindMask  = 0x70;
inline constexpr std::uint8_t valueKindShift = 4;

/// A scalar cell carries both a real primedia span holding its
/// shortest-round-trip rendering and the canonical bits, so that it is a link
/// endpoint and a formattable, transcludable Xanadu object while a query
/// never has to parse its text. See design R6.
enum class ValueKind : std::uint8_t {
  None   = 0,
  Double = 1,
  Bool   = 2,
  Int64  = 3,
};

// bit 7 is unclaimed.

/// For a dump or a diagnostic. An unrecognised verb or value kind is named
/// "unknown" rather than as one of the real ones, since a build reading a newer
/// spool is exactly when that matters.
const char *structureVerbName(StructureVerb verb);
const char *valueKindName(ValueKind kind);

[[nodiscard]] constexpr StructureVerb
structureVerbOf(const std::uint8_t flags) {
  return static_cast<StructureVerb>(flags & structureVerbMask);
}
[[nodiscard]] constexpr bool structureIsNegward(const std::uint8_t flags) {
  return (flags & structureNegward) != 0;
}
[[nodiscard]] constexpr ValueKind valueKindOf(const std::uint8_t flags) {
  return static_cast<ValueKind>((flags & valueKindMask) >> valueKindShift);
}
[[nodiscard]] constexpr std::uint8_t
structureFlags(const StructureVerb verb, const bool negward = false,
               const ValueKind value = ValueKind::None) {
  return static_cast<std::uint8_t>(
      static_cast<std::uint8_t>(verb) | (negward ? structureNegward : 0U) |
      (static_cast<std::uint8_t>(value) << valueKindShift));
}
/// @}

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
/// The inverse, as linkTypeFromName() is of linkTypeName(). Anything
/// unrecognised reads as Author, which is what a Link is constructed with.
ProminenceTier prominenceTierFromName(const std::string &name);

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
  ///
  /// Structure: the state produced by the previous operation on the same cell.
  /// Not provenance but the operation's subject -- a cell's micro-history is an
  /// intrusive chain, and its far end is the MakeCell whose index *is* the
  /// cell, which is how a SetLink says whose link it is without a field of its
  /// own. See design R7 and zigzag::Manifold::applyStructure().
  MicroversionId source;
  std::uint32_t sourceAt{};
  std::uint32_t sourceLength{};
  /// Link: which link was made. Structure: the dimension cell a SetLink is
  /// along. Held as 64 bits because a link id is, and checked against the
  /// node's 32-bit field when one is built -- see CompactOpNode::fromOp().
  std::uint64_t link{};
  /// Structure: which verb, which direction, and what `value` holds. See
  /// structureVerbOf() and the constants above. Zero for every other kind.
  std::uint8_t flags{};
  /// Structure: the canonical scalar bits, when `flags` says there are any.
  /// Zero for every other kind.
  std::uint64_t value{};
};

} // namespace xanadu

#endif // XUDU_OPS_H
