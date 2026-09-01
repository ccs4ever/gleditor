/**
 * @file publication.hpp
 * @brief A document as it leaves the machine that wrote it.
 *
 * Everything up to here is addressed for one machine. A span names a scroll by
 * a small integer, and that integer means something only in the store that
 * handed it out; a microversion is named "2a4", which is a name among the
 * versions of one document in one store. None of it travels.
 *
 * Publishing is what makes a document sayable elsewhere, and Xanadu asks for
 * three things of it that ordinary publishing does not do:
 *
 *   - The document is an edit decision list, not a copy. What is published is
 *     the list of pointers; the bytes stay where they were, which is what
 *     lets a quotation of this document be recognised as a quotation of the
 *     same content rather than of a similar-looking copy.
 *   - Authorship survives the journey. "The author's links are no different
 *     from anyone else's in implementation (though superior in prestige and
 *     legitimacy)" -- so who published a thing has to be a fact a reader can
 *     check, not a claim in a field. It is a signature over the whole
 *     manifest, and a manifest that does not verify does not decode.
 *   - Links and transclusions reach between documents. A link end is a span,
 *     a span is an offset into a scroll, and a published scroll has a name
 *     that means the same thing on every machine -- so a link made here to a
 *     passage there needs no agreement between the two, and no server.
 *
 * What this file adds is the global spelling of an address and the signed
 * manifest that carries a document's list of them.
 */
#ifndef XUDU_PUBLICATION_H
#define XUDU_PUBLICATION_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "microversion.hpp"
#include "mutable_link.hpp"
#include "ops.hpp"
#include "provenance.hpp"
#include "scroll.hpp"
#include "spool.hpp"
#include "swarm.hpp"

namespace xudu {

class Store;

/**
 * @brief The name of a scroll that means the same thing on every machine.
 *
 * A ScrollId is an index into one store's table. This is what that index
 * stands for, written so that two machines that have never met agree:
 *
 *   - `btpk:<64 hex>:<salt>` for a scroll with a publisher, which is the
 *     usual case and the one that survives re-sealing -- the key does not
 *     change when the torrent carrying the bytes does.
 *   - `file:<40 hex>:<index>` for content that exists only as one fixed
 *     torrent file. Honest rather than convenient: without a publisher there
 *     is nothing to bind two packagings of the same bytes together, so they
 *     are not the same scroll and must not be given the same name.
 *
 * The local spool has no global name at all, which is the point of
 * publishLocalSpool(): a document whose content is only here cannot be
 * published, because a reader would have no way to fetch what it points at.
 */
[[nodiscard]] std::string scrollKey(const Scroll &scroll);

/// The key the local spool would have once published under @p publisher.
[[nodiscard]] std::string scrollKeyFor(const PublicKey &publisher,
                                       const std::string &salt);

/**
 * @brief A span addressed the way another machine can read it.
 *
 * The same triple a PrimediaSpan carries, with the scroll named globally.
 * Comparison is by name and range, so two documents from two machines
 * overlap exactly where they quote the same content -- which is the whole of
 * how transclusion is detected across a network, as it is locally.
 */
struct GlobalSpan {
  std::string scroll;
  std::uint64_t start{};
  std::uint64_t length{};

  [[nodiscard]] std::uint64_t end() const { return start + length; }
  [[nodiscard]] bool empty() const { return 0 == length; }
  /// The part of this span @p other also covers, empty when they do not meet
  /// or are addresses into different scrolls.
  [[nodiscard]] GlobalSpan intersect(const GlobalSpan &other) const;

  bool operator==(const GlobalSpan &) const = default;
  [[nodiscard]] bool operator<(const GlobalSpan &other) const;
};

/// A link with both ends addressed globally. Same shape as Link, which is
/// addressed for one store.
struct GlobalLink {
  LinkType type{LinkType::Comment};
  ProminenceTier tier{ProminenceTier::Author};
  std::string owner;
  std::string curator;
  std::vector<GlobalSpan> left;
  std::vector<GlobalSpan> right;

  [[nodiscard]] bool touches(const GlobalSpan &span) const;
  bool operator==(const GlobalLink &) const = default;
};

/**
 * @brief One state of one document, signed by whoever published it.
 *
 * The name is the publisher's key and a salt, which is a BEP 46 name: the
 * same construction the scrolls use, for the same reason. A reader who has
 * the name can find the newest publication under it without asking anyone in
 * particular, and can tell that what they found is the publisher's.
 */
struct Publication {
  PublicKey publisher;
  /// Which document under that key. One person publishes many.
  std::string salt;
  std::string title;
  /// Which state of the document this is. Carried for the author's sake --
  /// what a reader gets is the list of pieces below, which is that state.
  MicroversionId version;
  /// Which publication of this name, in the sense BEP 44 means: a reader
  /// holding two takes the higher.
  std::int64_t sequence{};
  /// When, in seconds since the epoch. Says nothing about which is newer --
  /// that is the sequence's job -- and is here to be shown to a person.
  std::uint64_t published{};

  /// The document: an edit decision list over published content.
  std::vector<GlobalSpan> pieces;
  /// Links the publisher asserts. Anyone may publish links to this document
  /// without its publisher's involvement; these are the ones that came with
  /// it.
  std::vector<GlobalLink> links;
  /// Where the bytes of every scroll the pieces name can be fetched from, by
  /// global key. Without this a reader has addresses and no way to resolve
  /// them.
  std::map<std::string, Scroll> scrolls;

  /// Where the operations are, in the order they must be read to reconstruct
  /// the history: republishing seals only what is new, so a reader wanting
  /// the whole of it fetches one torrent per segment, in this order --
  /// segment.at and segment.length are operation counts here, not bytes, and
  /// segment.torrent (with segment.path, always sealedOpsName) is where to
  /// find the file. Empty in a manifest published before this existed, which
  /// is not the same as a document with no history -- a reader gets the
  /// pieces either way and no more.
  std::vector<ScrollSegment> opsSegments;

  /// Explicit signed table of withheld and transcopyright-locked ranges.
  std::vector<PublishedHoleRecord> holes;

  Signature signature;

  /// Where a reader looks for the newest publication of this document.
  [[nodiscard]] DhtTarget name() const;
  /// The magnet-shaped reference to hand somebody.
  [[nodiscard]] std::string uri() const;
  /// Total length in bytes of the document this stands for.
  [[nodiscard]] std::uint64_t length() const;
  [[nodiscard]] std::string describe() const;
};

/**
 * @brief The bytes a publication's signature is over.
 *
 * Everything except the signature, bencoded. Canonical because the encoder
 * orders dictionary keys: two machines encoding the same publication produce
 * the same bytes, which is what makes a signature checkable rather than a
 * coincidence.
 */
[[nodiscard]] std::string publicationSigningBuffer(const Publication &pub);

/// The whole publication, bencoded, ready to be written to a file, put in a
/// torrent, or handed to somebody.
[[nodiscard]] std::string encodePublication(const Publication &pub);

/**
 * @brief Read a publication back, and check it.
 *
 * Nothing comes back when the signature does not verify. An unsigned or
 * wrongly signed document is not a weaker document to be shown with a
 * warning: it is somebody's claim to have published what they did not, and
 * the only safe thing to do with it is to fail to read it.
 */
[[nodiscard]] std::optional<Publication>
decodePublication(std::string_view encoded);

/// Whether @p pub's signature really is its publisher's.
[[nodiscard]] bool verifyPublication(const Publication &pub);

/**
 * @brief Seal what this machine has written into a scroll anybody can fetch.
 *
 * The local spool is an append-only sequence of bytes, which is exactly what a
 * scroll is; all it lacks is a name and a carrier. This gives it both: the
 * bytes become a torrent, and the torrent becomes the first segment of a
 * scroll named by @p keys and @p salt. Offsets do not move -- a local span's
 * start is an offset into those same bytes -- so nothing already written has
 * to be rewritten, which is the property that makes sealing safe to do at any
 * time.
 *
 * Sealing again later covers what has been written since as a further
 * segment; the address of everything already sealed is untouched. See
 * scroll.hpp for why the carrier and the address have to be separable.
 *
 * The torrent carries three files: the content, the authorship record, and the
 * OpenPGP signature over that record. The record is signed before any of this
 * happens and is sealed in unchanged, so the info hash covers the content and
 * the claim about who wrote it together -- neither can be substituted for
 * another afterwards without the address changing. The content is file zero
 * and begins at offset zero of the stream, which is what keeps every address
 * already handed out pointing where it always did.
 *
 * The operations are sealed the same way, as a segment of their own -- see
 * sealableOps() -- but addressed by operation count rather than by byte, since
 * an operation has no fixed size. @p opsAlreadySealed is how many the last
 * seal already carried; @p opsSegment comes back empty when nothing has been
 * recorded since, which republishing over no new edits leaves it free to
 * treat as "nothing to add" rather than sealing a segment with nothing in it.
 *
 * @param into Directory to write the torrent and its files into, so that
 *        something can seed them. Nothing is written when it is empty.
 * @param priorScroll The scroll the local spool already is, from the last
 *        seal -- a default-constructed one, with no segments, for a first
 *        seal. Its segments are carried forward; only what has been written
 *        since its last one becomes a new segment.
 * @param opsAlreadySealed How many operations, by Store::opCount(), the last
 *        seal already carried. Zero for a first seal.
 * @return The scroll the local spool now is, every segment included, ready to
 *         be handed to publish(); and the new operations segment, if there
 *         were any operations to seal.
 */
struct SealedScroll {
  Scroll scroll;
  /// The new operations segment this seal produced, if it recorded anything
  /// since @p opsAlreadySealed. Not part of @p scroll: operations are not
  /// addressed by pieces the way primedia is, so nothing above resolves an
  /// offset into them -- this is carried separately, for the manifest's own
  /// ops-segment list, and assembled by historyFromSeal() rather than by
  /// Resolver::read().
  std::optional<ScrollSegment> opsSegment;
  /// The .torrent describing the files, for handing to a seeder.
  std::string torrentFile;
  InfoHash hash;
  /// The authorship record sealed in with the content, and its signature.
  SignedProvenance provenance;
};
[[nodiscard]] SealedScroll sealLocalSpool(
    const Store &store, const MutableKeys &keys, const std::string &salt,
    const std::string &into, const SignedProvenance &provenance,
    const Scroll &priorScroll = {}, std::uint32_t opsAlreadySealed = 0);

/**
 * @brief What a caller needs remembered between one seal and the next, to
 *        make the next one incremental.
 *
 * sealLocalSpool() takes a prior scroll and how many operations were already
 * sealed; publish() takes every operations segment sealed so far. This is
 * the three of them together, since a caller across two runs of the program
 * has nowhere else to keep them.
 */
struct SealState {
  /// The scroll the local spool has become, as of the last seal -- what
  /// @ref sealLocalSpool's priorScroll parameter wants next time.
  Scroll scroll;
  /// How many operations, by Store::opCount(), the last seal already
  /// carried -- what @ref sealLocalSpool's opsAlreadySealed parameter wants
  /// next time.
  std::uint32_t opsAlreadySealed{};
  /// Every operations segment sealed so far, oldest first -- what publish()'s
  /// opsSegments parameter wants next time.
  std::vector<ScrollSegment> opsSegments;
};

/// @p state, encoded so it can be written to a file and read back exactly.
[[nodiscard]] std::string encodeSealState(const SealState &state);

/// The inverse of encodeSealState(), or nothing when @p encoded is not one.
[[nodiscard]] std::optional<SealState>
decodeSealState(std::string_view encoded);

/**
 * @brief Publish @p version of @p store under @p keys.
 *
 * Every piece of the version is rewritten into global coordinates. A piece
 * pointing at content this machine has not published has no global address,
 * so this throws rather than publishing a document that cannot be read: the
 * content goes out first, which is the order Xanadu's model has anyway --
 * documents are lists of pointers into published permascrolls.
 *
 * @param sequence Which publication of this name. Must rise, or readers who
 *        already have an earlier one will keep it.
 * @param opsSegments Every operations segment sealed so far, oldest first --
 *        SealedScroll::opsSegment from this seal and every one before it, the
 *        way @p localSealedAs already carries every primedia segment forward.
 *        Carried into the manifest as-is; see Publication::opsSegments.
 * @throws std::runtime_error if the version points at unpublished content.
 */
[[nodiscard]] Publication
publish(const Store &store, const MicroversionId &version,
        const MutableKeys &keys, std::string salt, std::string title,
        std::int64_t sequence, std::uint64_t published,
        const Scroll *localSealedAs                   = nullptr,
        const std::vector<ScrollSegment> &opsSegments = {});

/**
 * @brief Publish a document under @p documentKeys, incrementally sealing the
 *        user's shared permascroll.
 */
[[nodiscard]] Publication
publishDocument(Store &store, const MicroversionId &version,
                const MutableKeys &documentKeys, std::string salt,
                std::string title, std::int64_t sequence,
                std::uint64_t published,
                const SignedProvenance &permascrollProvenance,
                const SignedProvenance &documentProvenance,
                const std::string &torrentOutputDir = {});

/**
 * @brief The global name of the scroll @p span points into.
 *
 * Empty when it has none, which is the case for content typed here and not yet
 * sealed: it exists at an offset in this machine's spool and nowhere else.
 *
 * @param localSealedAs What the local spool has been sealed as, if it has. Its
 *        offsets are the same bytes, so a local span's address is unchanged --
 *        only the name it is said under is new.
 */
[[nodiscard]] std::string globalKeyOf(const Store &store,
                                      const PrimediaSpan &span,
                                      const Scroll *localSealedAs = nullptr);

/// @p span written so another machine can read it, or nothing when its scroll
/// has no global name.
[[nodiscard]] std::optional<GlobalSpan>
globalise(const Store &store, const PrimediaSpan &span,
          const Scroll *localSealedAs = nullptr);

/**
 * @brief @p span in @p store's own coordinates.
 *
 * The other direction, and the one that lets a document here point into a
 * document published elsewhere: the scroll is recorded in this store if it is
 * not already, and the offsets are carried across untouched, because a scroll
 * offset means the same thing everywhere. That is the whole trick -- an
 * address does not have to be translated, only the name of what it is an
 * address into.
 *
 * @param scrolls Where the scroll can be looked up when this store has never
 *        heard of it, which for a publication is its own scroll table.
 * @return Nothing when the scroll is named by neither, since a span into
 *         content with no way to reach it would read as text that is silently
 *         absent.
 */
[[nodiscard]] std::optional<PrimediaSpan>
localise(Store &store, const GlobalSpan &span,
         const std::map<std::string, Scroll> &scrolls);

/**
 * @brief The operations of a published document, ready to be sealed.
 *
 * Not simply the store's own encoding of them. An operation names content by
 * a ScrollId, which is a small integer meaning something only in the store
 * that handed it out -- and the most common one, zero, means "my own primedia
 * spool". Sealed as they stand, every operation a publisher typed would point
 * a reader at the reader's own spool, and resolve to whatever happened to be
 * at those offsets. So the seal carries the table that says what those
 * integers meant, in global keys.
 *
 * Zero is not in the table. It is the scroll being sealed alongside these
 * operations, whose bytes begin at offset zero of the same piece stream, so a
 * local offset and a scroll offset are the same number and the reader already
 * holds the answer.
 *
 * @param sinceExclusive See Store::opRecords(). A publisher republishing
 *        seals only the operations recorded since their last seal; each one
 *        still names its own parent and source by full microversion, so a
 *        reader who already has everything up to that point needs nothing
 *        else to apply what this adds. The default, zero, seals everything --
 *        what a first publish needs.
 */
[[nodiscard]] std::string sealableOps(const Store &store,
                                      std::uint32_t sinceExclusive = 0);

/**
 * @brief Read back what one or more sealableOps() calls wrote, into a store
 *        of its own.
 *
 * @p segments in the order they were sealed in -- oldest first, the order a
 * scroll's own segments are kept in. Each was built knowing only the
 * operations up to that point, so a later segment's records may name parents
 * an earlier segment produced; applying them in seal order is what makes that
 * resolve. Nothing about a segment's own scroll table survives past it: each
 * is decoded and folded into the same running history before the next one is
 * read, exactly as if the whole thing had been sealed at once.
 *
 * The publisher's names are kept. A microversion is "a name among the
 * versions of one document in one store", so their 2a4 belongs to their
 * document and not to whatever this machine happens to call its own second
 * state -- and a reader who can say "2a4" and mean what the publisher means
 * is the point of carrying the history at all.
 *
 * Which is why this returns a store rather than putting the operations into
 * one: their history is theirs. What links it to this machine's own documents
 * is the content, which both point at by the same global addresses.
 *
 * @param segments the bytes of each seal's operations file, oldest first.
 * @param from     the scroll those operations were sealed beside, which is
 *                 what their ScrollId zero means.
 * @throws std::runtime_error if any segment's bytes are not a sealed
 *         operations file, or name a scroll the seal does not carry.
 */
[[nodiscard]] std::unique_ptr<Store>
historyFromSeal(std::span<const std::string_view> segments, const Scroll &from,
                const std::map<std::string, Scroll> &scrolls);

/// Convenience for the single-seal case: everything was sealed at once.
[[nodiscard]] std::unique_ptr<Store>
historyFromSeal(std::string_view sealed, const Scroll &from,
                const std::map<std::string, Scroll> &scrolls);

/// What taking a publication in produced.
struct Adopted {
  /// The state showing the published document, ready to be opened.
  MicroversionId version;
  /// Links taken in with it. A link the store already had is not taken twice.
  std::size_t links{};
  /// Scrolls this store can now resolve addresses into.
  std::size_t scrolls{};
};

/**
 * @brief Take a published document into @p store so it can be read here.
 *
 * The inverse of publish(), and what makes a published document a first-class
 * thing on a machine that did not write it: its pieces become a version, its
 * scrolls become addresses this store can resolve, and its links become links
 * here. From that point a document typed on this machine can quote it and can
 * be linked to it, by exactly the same operations as if it had been written
 * here -- the addresses are already global, so nothing is translated and no
 * agreement between the two machines is needed.
 *
 * Nothing about the result is published, and nothing has to be. A link's ends
 * are addresses; whether the document holding one has been published decides
 * whether *that* document can travel, not whether the link can be made.
 *
 * Adopting the same publication twice adds a second reading of it and no
 * duplicate links.
 *
 * @throws std::runtime_error when the signature does not verify -- an
 *         unverified manifest is somebody's claim to have published what they
 *         did not -- or when a piece names a scroll the manifest does not
 *         carry, which would be a document with a hole in it rather than a
 *         document.
 */
Adopted adopt(Store &store, const Publication &pub);

/**
 * @brief Documents this machine has, however they arrived.
 *
 * A reader accumulates publications: some fetched by name, some handed over,
 * some its own. What the library adds beyond a list is the question that
 * makes a link between documents drawable -- given a span, which documents
 * show it and whereabouts.
 */
class Library {
public:
  /**
   * @brief Take a publication in.
   *
   * Refused when it does not verify. Refused when an equal or newer
   * publication of the same name is already here: a name moves forward.
   *
   * @return Whether it was taken.
   */
  bool add(Publication pub);

  [[nodiscard]] const Publication *find(const DhtTarget &name) const;
  [[nodiscard]] std::vector<const Publication *> all() const;
  [[nodiscard]] std::size_t size() const { return byName.size(); }

  /// Where a document shows a span: which document, and the byte range of
  /// that document showing it.
  struct Sighting {
    const Publication *document{};
    /// Byte range within the document's own text.
    std::uint32_t start{};
    std::uint32_t end{};
    /// The part of the asked-for span that this covers.
    GlobalSpan shared;
  };

  /**
   * @brief Every place any known document shows any of @p span.
   *
   * This is the transclusion query, across documents rather than within one:
   * the same comparison of addresses, over a set of manifests. It is what
   * turns "this link points at that passage" into "and that passage is on
   * screen, there".
   */
  [[nodiscard]] std::vector<Sighting> showing(const GlobalSpan &span) const;

  /// Every link, in every document here, with an end touching @p span.
  struct FoundLink {
    const Publication *document{};
    const GlobalLink *link{};
    /// Whether @p span was found on the link's left end. A link is
    /// traversable both ways; which way this crossing goes is the difference.
    bool onLeft{};
  };
  [[nodiscard]] std::vector<FoundLink>
  linksTouching(const GlobalSpan &span) const;

private:
  std::map<DhtTarget, Publication> byName;
};

} // namespace xudu

#endif // XUDU_PUBLICATION_H
