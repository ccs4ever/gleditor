/**
 * @file store.hpp
 * @brief The two spools together, and the three things a server does with
 *        them.
 *
 * "The server does not store versions. Nothing stores versions. Versions
 * themselves are not saved, but regenerated as needed from these two files."
 *
 * That sentence is the design. What is kept is content that was typed, at
 * permanent addresses, and a record of every operation, filed under the state
 * it produced. A version is rebuilt by replaying the operations its name
 * spells out, which is why MicroversionId::path() exists and why nothing here
 * has a cache of documents to keep in step.
 *
 * The protocol Nelson describes has three functions, and they are the three
 * public methods that name them below: store an op under an ascending number,
 * return the op filed under a number, and return the whole sequence needed to
 * regenerate a version.
 */
#ifndef XUDU_STORE_H
#define XUDU_STORE_H

#include <cstdint>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "compact_op.hpp"
#include "format.hpp"
#include "microversion.hpp"
#include "ops.hpp"
#include "resolver.hpp"
#include "scroll.hpp"
#include "segmented_ops_spool.hpp"
#include "spool.hpp"
#include "version.hpp"

namespace xudu {

/**
 * @class Store
 * @brief A xanadoc: one primedia spool, one operations spool, and its links.
 */
class Store : public SpanReader {
public:
  // -- OSMIC's three server functions ---------------------------------------

  /**
   * @brief Function 1: file @p op under the state it produces.
   *
   * @throws std::invalid_argument if something is already filed there, or if
   *         the op's parent is not @p produces's parent. An operations spool
   *         is append-only: a state, once produced, is what it is forever, and
   *         quietly replacing one would silently rewrite every version
   *         downstream of it.
   */
  void putOp(const MicroversionId &produces, const Op &op);

  /// Function 2: the op filed under @p id, or nothing.
  [[nodiscard]] const Op *getOp(const MicroversionId &id) const;

  /// Fast zero-copy access to 64-byte compact node by microversion or index.
  [[nodiscard]] const CompactOpNode *
  getCompactOp(const MicroversionId &id) const {
    return opsSpool.get(id);
  }
  [[nodiscard]] const CompactOpNode *
  getCompactOp(const std::uint32_t index) const {
    return opsSpool.get(index);
  }

  [[nodiscard]] const SegmentedOpsSpool &segmentedOps() const {
    return opsSpool;
  }
  [[nodiscard]] SegmentedOpsSpool &segmentedOps() { return opsSpool; }

  /**
   * @brief Function 3: every op number needed to regenerate @p version, in the
   *        order they must be replayed.
   *
   * This is MicroversionId::path() filtered to what has actually been
   * recorded, which is what makes it answerable about a version rather than
   * about a name.
   */
  [[nodiscard]] std::vector<MicroversionId>
  opsFor(const MicroversionId &version) const;

  // -- versioning on demand -------------------------------------------------

  /**
   * @brief Rebuild @p version by replaying its operations from the null
   *        document.
   *
   * Nothing is cached. Rebuilding is linear in the number of operations
   * leading to the state, which for a document under active editing is the
   * number of keystrokes it has had -- so this is fast enough to do on every
   * change and would not be fast enough to do on every frame.
   */
  [[nodiscard]] Version rebuild(const MicroversionId &version) const;

  /**
   * @brief Carry @p document, which is the document at @p known, forward to
   *        @p version by replaying only the operation between them.
   *
   * Editing moves a document one operation at a time, and whoever is showing
   * it still has what it looked like a moment ago. Replaying just that one
   * operation costs what the operation costs; rebuild() would replay the
   * whole history to arrive at the same place, which for a long document is
   * the difference between a keystroke being free and a keystroke being felt.
   *
   * A branch counts as one step, since the state a branch begins is one
   * operation past the state it forks from -- so going back and typing gets
   * this too, not just typing on the end.
   *
   * @return false when @p version is not one operation past @p known --
   *         travelling in hypertime, opening some other state, or several
   *         edits recorded before anything asked to see them. @p document is
   *         left untouched, and the caller wants rebuild() instead.
   */
  [[nodiscard]] bool advance(Version &document, const MicroversionId &known,
                             const MicroversionId &version) const;

  /// The text of @p version, which is rebuild() followed by materialize().
  /// Content quoted from a torrent this machine cannot reach comes out empty,
  /// so a document is readable even when part of what it points at is not.
  [[nodiscard]] std::string textOf(const MicroversionId &version) const;

  // -- making new states ----------------------------------------------------

  /**
   * @brief Record an operation applied to @p parent, and say which state it
   *        produced.
   *
   * Continues the chain when @p parent has no successor yet, and starts a new
   * branch when it has: editing a state you have gone back to does not
   * overwrite what already came after it. That is the whole of what OSMIC is
   * arguing for -- "the previous work... do not have to be lost" -- and it is
   * one branch here rather than a policy anything has to opt into.
   *
   * @throws std::runtime_error on the practically-unreachable case that the
   *         parent already has a successor on every ordinal a branch name
   *         can hold (see MicroversionId::branch()).
   */
  MicroversionId apply(const MicroversionId &parent, Op op);

  /// Type @p text into @p parent at @p at. Appends to the primedia spool and
  /// records an INSERT.
  MicroversionId insert(const MicroversionId &parent, std::uint32_t at,
                        std::string_view text);

  /// Stop pointing at [@p at, @p at + @p length) of @p parent. The content
  /// stays in the spool; see OpKind::Delete.
  MicroversionId erase(const MicroversionId &parent, std::uint32_t at,
                       std::uint32_t length);

  /// Move a range of @p parent so that it starts at @p to.
  MicroversionId rearrange(const MicroversionId &parent, std::uint32_t at,
                           std::uint32_t length, std::uint32_t to);

  /**
   * @brief Put the content [@p sourceAt, +@p sourceLength) of @p source into
   *        @p parent at @p at.
   *
   * A virtual copy: what is inserted is pointers to the addresses the source
   * already uses, so there is one copy of the content and two documents
   * showing it. Comparing the two afterwards -- see Version::occurrencesOf --
   * finds the shared passage by address.
   */
  MicroversionId transclude(const MicroversionId &parent, std::uint32_t at,
                            const MicroversionId &source,
                            std::uint32_t sourceAt, std::uint32_t sourceLength);

  /**
   * @brief Force a page break at @p at in @p parent. Records an
   *        OpKind::PageBreak.
   *
   * Unlike insert(), erase() or transclude(), this names no primedia address
   * at all: see the comment on OpKind::PageBreak for why a break has to be
   * concatext-relative rather than content-addressed, and therefore does not
   * travel with a passage the way a Link does when it is quoted elsewhere.
   */
  MicroversionId insertBreak(const MicroversionId &parent, std::uint32_t at);

  // -- links ----------------------------------------------------------------

  /**
   * @brief Record a link and file the operation that made it.
   *
   * Links are kept beside the operations rather than inside a version, because
   * they attach to content and every version quoting that content has them.
   */
  MicroversionId addLink(const MicroversionId &parent, Link link);

  /// Every link with an end covering any of @p span.
  [[nodiscard]] std::vector<const Link *>
  linksTouching(const PrimediaSpan &span) const;
  [[nodiscard]] const std::map<std::uint64_t, Link> &links() const {
    return linkTable;
  }

  // -- formatting -------------------------------------------------------------
  //
  // See format.hpp. A format link is made the same way as any other: build a
  // Link with type Format, left naming the content, right naming
  // vocabularySpanFor(attribute) -- the free function; a Store is not needed
  // to compute it -- and call addLink(). There is no separate addFormat(),
  // because nothing about making one differs from any other link once the
  // right end is in hand.

  /**
   * @brief Which attribute @p link names, if it is a recognised format link.
   *
   * Nothing but LinkType::Format and a right end that is exactly
   * vocabularySpanFor() of some attribute qualifies -- a Format link with a
   * right end some other program wrote and this one does not recognise reads
   * as unformatted rather than guessed at.
   */
  [[nodiscard]] std::optional<FormatAttribute>
  formatAttributeOf(const Link &link) const;

  // -- the hypertime map ----------------------------------------------------

  /// The states reachable in one step from @p id: its continuation, and the
  /// first state of each branch off it.
  [[nodiscard]] std::vector<MicroversionId>
  children(const MicroversionId &id) const;

  /// Every state that has been recorded, in replay order.
  [[nodiscard]] std::vector<MicroversionId> allVersions() const;

  /// The most recently recorded state, which is where a program that has just
  /// opened a store should start.
  [[nodiscard]] MicroversionId latest() const;

  [[nodiscard]] const PrimediaSpool &primedia() const { return spool; }
  [[nodiscard]] std::size_t opCount() const { return ops.size(); }

  // -- content that was not typed here --------------------------------------

  /**
   * @brief Record a scroll, and give back the id spans name it by.
   *
   * Recording the same scroll twice gives the same id: a scroll is identified
   * by its publisher's name, or failing that by the file it was found in, so
   * two references to one scroll are two references to one thing -- which is
   * what makes a transclusion between them detectable.
   *
   * Segments of a scroll already known are merged into it rather than starting
   * a second entry, so learning about a further seal does not fork the
   * identity of everything referring to it.
   */
  ScrollId addScroll(const Scroll &scroll);

  /**
   * @brief Say that a stretch of a scroll is carried by a torrent.
   *
   * This is what sealing does, and the reason the addressing was arranged this
   * way: it changes where bytes are fetched from and leaves every address that
   * refers to them untouched. A segment covering a stretch that already has
   * one replaces it.
   */
  void addSegment(ScrollId id, const ScrollSegment &segment);

  /// The scroll @p id names, or nullptr for the local spool and for anything
  /// this store has never recorded.
  [[nodiscard]] const Scroll *scroll(ScrollId id) const;
  [[nodiscard]] const std::vector<Scroll> &scrolls() const { return externals; }

  /// Where the bytes of external scrolls are fetched from. Not owned.
  void setContentSource(const ContentSource *source) {
    resolver.setSource(source);
  }
  [[nodiscard]] const Resolver &contentResolver() const { return resolver; }

  /**
   * @brief Read a span, wherever its content lives.
   *
   * Local spans come from the spool. External ones are fetched and verified
   * against the torrent's piece hashes; content that cannot be reached, or
   * that does not hash to what the reference named, reads as nothing rather
   * than as something plausible.
   */
  [[nodiscard]] std::string read(const PrimediaSpan &span) const override;

  /**
   * @brief Quote a range of somebody else's scroll into @p parent at @p at.
   *
   * The document ends up pointing at content this machine may not hold, whose
   * address means the same thing to everyone. Nothing is copied and nothing
   * needs to be downloaded for the reference to be made -- only to be read.
   *
   * @param scrollOffset An offset in @p from's own coordinates, which for a
   *        scroll that is one torrent file is an offset into that file.
   */
  MicroversionId transcludeExternal(const MicroversionId &parent,
                                    std::uint32_t at, const Scroll &from,
                                    std::uint64_t scrollOffset,
                                    std::uint64_t length);

  // -- persistence ----------------------------------------------------------

  /**
   * @brief Write the store to @p directory as its two spools and a link file.
   *
   * The primedia spool is written as the bytes it is. The operations spool is
   * written in an ultra-compact binary format.
   */
  void save(const std::string &directory) const;

  /**
   * @brief Write the store to @p directory using canonical human-readable
   *        OSMIC text format for the operations spool.
   */
  void saveOsmicText(const std::string &directory) const;

  /// Generate standard OSMIC text format of all operations on demand.
  [[nodiscard]] std::string exportOsmicText() const;

  /// Stream standard OSMIC text format of all operations on demand.
  void writeOsmicText(std::ostream &out) const;

  /// Read back what save() or saveOsmicText() wrote. Auto-detects binary and
  /// text formats. A directory with no store in it produces an empty one.
  void load(const std::string &directory);

private:
  /// Apply one recorded op to @p onto. The single replay path: everything that
  /// rebuilds a document comes through here, so replaying and recording cannot
  /// drift.
  ///
  /// Takes the spool's own 64-byte node rather than an Op. The two carry the
  /// same operation -- CompactOpNode names the parent and transclusion source
  /// by spool index where Op names them by microversion -- but the node is
  /// what the ancestral walk already has in hand, and going back through the
  /// Op map for it cost about nine tenths of a rebuild.
  void replay(const CompactOpNode &node, Version &onto) const;

  /// rebuild(), for a state already known to be in the spool at @p index.
  /// Split out because a transclusion replays its source the same way, and it
  /// has the index rather than the name.
  [[nodiscard]] Version rebuildFromIndex(std::uint32_t index) const;

  PrimediaSpool spool;
  /// How many bytes of @ref spool are already on disk, and in which directory.
  /// The primedia spool only ever grows, so a save to the directory the last
  /// one went to appends what is new rather than rewriting the whole thing --
  /// which is what made saving cost the length of the document rather than
  /// the length of what had just been typed. Reset by load(); advanced by
  /// save(). A save to any other directory rewrites from scratch, since
  /// nothing is known about what is already there.
  mutable std::uint64_t primediaFlushed{0};
  mutable std::string flushedPrimediaDirectory;
  /// Scrolls other than the local spool, in the order they were first
  /// recorded. A span's ScrollId is one more than the index here, so that zero
  /// stays the local spool.
  std::vector<Scroll> externals;
  Resolver resolver;
  /// The operations spool, filed by the state each op produces. Ordered, so
  /// iteration is replay order.
  SegmentedOpsSpool opsSpool;
  std::map<MicroversionId, Op> ops;
  std::map<std::uint64_t, Link> linkTable;
  std::uint64_t nextLinkId{1};
};

} // namespace xudu

#endif // XUDU_STORE_H
