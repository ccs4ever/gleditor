/**
 * @file link_occurrences.hpp
 * @brief Where each authored member of one link appears, exactly.
 *
 * A link is two ordered lists of primedia spans, and a reader navigating it
 * chooses one member of one side and then one place that member appears. The
 * beam layout answers a different question -- which documents a link runs
 * between -- and to answer it cheaply widens a side's members to one covering
 * extent per document. That extent includes the gaps between disjoint
 * members, and a navigator built on it would highlight content the author
 * never linked. This query keeps every member separate, in stored order, and
 * reports each manifestation of it on its own, so that two quotations of one
 * member stay two targets even when they sit back to back.
 */
#ifndef COMMON_XANADU_LINK_OCCURRENCES_HPP
#define COMMON_XANADU_LINK_OCCURRENCES_HPP

#include <cstdint>
#include <expected>
#include <span>
#include <variant>
#include <vector>

#include "common/xanadu/document_id.hpp"
#include "common/xanadu/microversion.hpp"
#include "common/xanadu/ops.hpp"
#include "common/xanadu/spool.hpp"
#include "common/xanadu/version.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu {

class Store;

/// The two endsets of a link. Names, not directions: neither is the source.
enum class LinkSide : std::uint8_t { Left, Right };

[[nodiscard]] constexpr LinkSide opposite(const LinkSide side) noexcept {
  return LinkSide::Left == side ? LinkSide::Right : LinkSide::Left;
}

/// Whether a manifestation carries all of the member it matched or only part.
enum class Coverage : std::uint8_t { Full, Partial };

/**
 * @brief A link's identity beyond the store that holds it.
 *
 * Link::id is an operation index, meaningful only inside the store that
 * minted it; the store's DocumentId is what makes the pair addressable from a
 * saved visit or another session.
 */
struct LinkKey {
  DocumentId authority;
  zigzag::CellRef id{zigzag::noCell};

  bool operator==(const LinkKey &) const = default;
};

/// One run of consecutive addresses a member occupies within a list of pieces.
struct PieceMatch {
  Extent range;
  Coverage coverage{Coverage::Full};

  bool operator==(const PieceMatch &) const = default;
};

/**
 * @brief Everywhere @p member appears among @p pieces, one entry per
 *        quotation.
 *
 * Offsets are bytes into the text the pieces concatenate to. Neighbouring
 * pieces are joined only when their addresses continue: a passage quoted twice
 * back to back is two matches here, where Version::occurrencesOf() -- which
 * joins by position alone -- reports one. Serves document versions and cell
 * content alike, since both are an ordered run of spans.
 */
[[nodiscard]] std::vector<PieceMatch>
exactOccurrences(std::span<const PrimediaSpan> pieces,
                 const PrimediaSpan &member);

/**
 * @brief Everywhere the whole of @p content -- a cell's run of spans, in
 *        order -- appears among @p pieces.
 *
 * Every span must be matched in full and each must carry straight on from
 * the one before, so a document quoting only part of a cell, or its spans in
 * another order, is not a place the cell's content appears.
 */
[[nodiscard]] std::vector<Extent>
contentOccurrences(std::span<const PrimediaSpan> pieces,
                   std::span<const PrimediaSpan> content);

/// A member manifested in one state of a document.
struct DocumentSite {
  DocumentId store;
  MicroversionId version;
  Extent range;

  bool operator==(const DocumentSite &) const = default;
};

/// A member manifested in a cell's content, as bytes into that content.
struct CellSite {
  DocumentId store;
  MicroversionId version;
  zigzag::CellRef cell{zigzag::noCell};
  Extent range;

  bool operator==(const CellSite &) const = default;
};

using OccurrenceSite = std::variant<DocumentSite, CellSite>;

/**
 * @brief Whether @p hit falls on @p site: the same store, state and cell,
 *        and overlapping bytes.
 *
 * An empty @p hit is a caret position, and lands on a site it is inside or at
 * either end of -- a caret just past the last byte of a passage is still
 * reading it.
 */
[[nodiscard]] bool lands(const OccurrenceSite &hit, const OccurrenceSite &site);

/// One exact place a member appears.
struct Occurrence {
  OccurrenceSite site;
  Coverage coverage{Coverage::Full};

  bool operator==(const Occurrence &) const = default;
};

/**
 * @brief One authored member of a link and where it was found.
 *
 * `occurrences` covers only the documents and cells the query was given, so
 * an empty list means "not in view", not "nowhere": the member keeps its place
 * and identity either way.
 */
struct LinkMember {
  LinkSide side{LinkSide::Left};
  /// Position in the stored endset, which is the order the author gave.
  std::uint32_t index{};
  PrimediaSpan span;
  std::vector<Occurrence> occurrences{};

  [[nodiscard]] bool inView() const noexcept { return !occurrences.empty(); }
};

/// A link with both endsets resolved against a set of views.
struct LinkOccurrences {
  LinkKey key;
  /// The record as it was when resolved, so a later revision is detectable.
  Link link;
  std::vector<LinkMember> left;
  std::vector<LinkMember> right;

  [[nodiscard]] const std::vector<LinkMember> &
  members(const LinkSide side) const noexcept {
    return LinkSide::Left == side ? left : right;
  }
};

/// One open state of a document to look for members in.
struct DocumentView {
  DocumentId store;
  MicroversionId version;
  const Version &text;
};

/**
 * @brief Cells of one manifold to look for members in.
 *
 * The caller names the cells -- the visible neighbourhood, or a Spanfilade's
 * candidates -- so that the cost of a query is the caller's to bound.
 */
struct CellView {
  DocumentId store;
  MicroversionId version;
  const zigzag::Manifold &manifold;
  std::span<const zigzag::CellRef> cells;
};

enum class LinkQueryError : std::uint8_t {
  /// The store holds no link with that id.
  LinkNotFound,
};

/**
 * @brief Resolve every member of link @p id in @p store against @p documents
 *        and @p cells.
 *
 * Members keep their stored order on each side and are never merged; each
 * lists its occurrences documents first, in the order given, then cells.
 */
[[nodiscard]] std::expected<LinkOccurrences, LinkQueryError>
resolveLinkOccurrences(const Store &store, zigzag::CellRef id,
                       std::span<const DocumentView> documents,
                       std::span<const CellView> cells);

} // namespace xanadu

#endif // COMMON_XANADU_LINK_OCCURRENCES_HPP
