/**
 * @file version.hpp
 * @brief A document as a list of pointers, not as its text.
 *
 * "The operative unit is the version, not the document... A version is
 * typically virtual, a list of pieces." Each piece names a run of the primedia
 * spool; the text is what you get by following them, and is never what is
 * stored. This is the same shape as the edit decision list a film edit
 * produces -- the deliverable is the list of shots, not a new copy of the
 * footage.
 *
 * Two things fall out of it that a plain string cannot do.
 *
 * Every byte of a version knows where it came from. So "which parts of these
 * two documents are the same content" is answered by comparing addresses, not
 * by searching for matching text, and it stays answerable after both have been
 * edited around the shared part.
 *
 * Quoting costs a pointer. Transcluding a passage inserts a piece aimed at the
 * address the original already uses, so there is one copy and both documents
 * show it -- Nelson's "conceptually there is only one copy of anything".
 */
#ifndef XUDU_VERSION_H
#define XUDU_VERSION_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "spool.hpp"

namespace xudu {

/// Where in a version a range of content sits, as a half-open byte range.
struct Extent {
  std::uint32_t start{};
  std::uint32_t end{};
  [[nodiscard]] bool empty() const { return end <= start; }
  bool operator==(const Extent &) const = default;
};

/**
 * @class Version
 * @brief One state of a document, as an ordered list of primedia pieces.
 *
 * Offsets taken and returned are byte offsets into the text the version would
 * produce, which is what an editor's caret and selection are measured in.
 */
class Version {
public:
  /// Insert a pointer to @p span at @p at, splitting whatever piece it lands
  /// inside. This is OSMIC's INSERT, and also how TRANSCLUDE puts content from
  /// another version into this one -- the operation differs only in where the
  /// span came from.
  void insert(std::uint32_t at, const PrimediaSpan &span);

  /**
   * @brief Remove the pieces covering [@p at, @p at + @p length).
   *
   * OSMIC calls deletion "REARRANGE TO LIMBO": the content is not destroyed,
   * it stops being pointed at. Nothing is removed from the spool, so every
   * earlier version that pointed at it still resolves.
   *
   * @return What was removed, as the spans it pointed at, so that the caller
   *         can put it back somewhere -- which is what a rearrangement is.
   */
  std::vector<PrimediaSpan> remove(std::uint32_t at, std::uint32_t length);

  /**
   * @brief Move [@p at, @p at + @p length) so that it begins at @p to.
   *
   * REARRANGE proper. @p to is a position in the version as it is now, before
   * the move; a destination inside the moved range is a no-op, since there is
   * nowhere for it to go.
   */
  void rearrange(std::uint32_t at, std::uint32_t length, std::uint32_t to);

  /// Insert pointers to @p spans, in order, at @p at.
  void insertSpans(std::uint32_t at, const std::vector<PrimediaSpan> &spans);

  /**
   * @brief The text this version stands for.
   *
   * Read through @p reader rather than out of a spool, because a version's
   * pieces may point at content this machine never typed and may not hold. A
   * piece the reader cannot produce contributes nothing, so a document quoting
   * something unreachable still has the rest of itself.
   */
  [[nodiscard]] std::string materialize(const SpanReader &reader) const;

  /// Total length in bytes of that text.
  [[nodiscard]] std::uint32_t length() const;

  [[nodiscard]] const std::vector<PrimediaSpan> &pieces() const { return runs; }

  /// The primedia address a byte of this version came from, or nothing when
  /// the offset is past the end.
  [[nodiscard]] std::optional<std::uint64_t>
  addressAt(std::uint32_t offset) const;

  /// The spans [@p at, @p at + @p length) points at, without removing them.
  [[nodiscard]] std::vector<PrimediaSpan> spansFor(std::uint32_t at,
                                                   std::uint32_t length) const;

  /**
   * @brief Everywhere in this version that @p span's content appears.
   *
   * The transclusion query. A version quoting a passage twice reports it
   * twice, and a version that merely contains the same words reports nothing,
   * because this compares addresses and not text.
   */
  [[nodiscard]] std::vector<Extent>
  occurrencesOf(const PrimediaSpan &span) const;

private:
  /**
   * @brief Split whatever piece covers @p offset so that a boundary falls
   *        there, and return the index of the piece that then begins at it.
   *
   * Never leaves an empty piece behind: an offset landing on a boundary splits
   * nothing, and one landing inside a piece cuts it into two non-empty halves.
   * That, with empty spans refused on the way in, is why no method here has to
   * go looking for empties afterwards.
   */
  std::size_t splitAt(std::uint32_t offset);

  /// Join the piece at @p index with the one after it when they name
  /// consecutive addresses in the same scroll.
  void joinFollowing(std::size_t index);

  std::vector<PrimediaSpan> runs;

  /**
   * @brief Total length of @p runs, maintained rather than counted.
   *
   * Every mutation here asked length() for the end of the document, and
   * length() added up every piece to answer. That made one insertion cost a
   * pass over the whole version, and replaying a history cost a pass per
   * operation -- so rebuilding a document was quadratic in the number of edits
   * it had ever had, and rebuilding is what happens on every keystroke.
   *
   * Kept in step by the four methods that change runs. Nothing else can: runs
   * is private and pieces() hands out a const reference.
   */
  std::uint32_t total{};
};

} // namespace xudu

#endif // XUDU_VERSION_H
