/**
 * @file result_slice.hpp
 * @brief Inspectable result rows shared by the query and array REPLs.
 */
#ifndef COMMON_XANADU_RESULT_SLICE_HPP
#define COMMON_XANADU_RESULT_SLICE_HPP

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"

namespace xanadu {

/// Bytes that already have an address: spans in @ref store's own scrolls,
/// with the bytes a query put between them (a join's separator). A span of
/// `scratchScroll` names `[start, start + length)` of @ref scratch and is
/// written as typed text; every other span is transcluded.
struct QuotedSpans {
  const Store *store{nullptr};
  std::vector<PrimediaSpan> spans;
  std::string scratch;
};

/// Where the bytes of arena cell @p master (a clone master) already live, so
/// a row written from it transcludes them: the run `quotedRun` names, or, for
/// a store-less cell whose spans are all of the author's permascroll (a
/// capture), the same spans against @p captureStore. Scratch spans (a join's
/// separators) are copied into `scratch` and typed either way; a cell with
/// nothing but scratch quotes nothing.
[[nodiscard]] std::optional<QuotedSpans>
quotedSpansOf(const zigzag::ArenaManifold &arena, zigzag::CellRef master,
              const Store *captureStore);

struct ResultRow {
  /// What the row reads as. Typed only when there is no @ref quote: a value
  /// the query constructed, such as a count, has no address to point at.
  std::string text;
  /// A local store path and CellRef, or an earlier result row's source.
  std::string source;
  /// Where the row's text already lives. A row written from one transcludes
  /// those bytes rather than typing a copy of them.
  std::optional<QuotedSpans> quote;

  /// Rows are equal by what they say; a row read back from a store quotes
  /// that store, which the row it was written from did not.
  bool operator==(const ResultRow &other) const {
    return text == other.text && source == other.source;
  }
};

/**
 * @brief Write rows on d.result; d.source hangs an inspectable provenance
 *        cell from each row.
 *
 * @return The store's designated current version.
 * @throws std::invalid_argument when a row quotes bytes @p store cannot name
 *         -- a scroll another permascroll holds -- rather than copying them.
 */
MicroversionId writeResultSlice(Store &store, std::span<const ResultRow> rows);

/// Read the public result rank, preserving row order and provenance. Each row
/// quotes its cell's spans in @p store, which must outlive the rows.
[[nodiscard]] std::vector<ResultRow> readResultSlice(const Store &store,
                                                     MicroversionId version);

} // namespace xanadu

#endif // COMMON_XANADU_RESULT_SLICE_HPP
