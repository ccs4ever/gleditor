/**
 * @file result_slice.hpp
 * @brief Inspectable result rows shared by the query and array REPLs.
 */
#ifndef COMMON_XANADU_RESULT_SLICE_HPP
#define COMMON_XANADU_RESULT_SLICE_HPP

#include <span>
#include <string>
#include <vector>

#include "common/xanadu/store.hpp"

namespace xanadu {

struct ResultRow {
  std::string text;
  /// A local store path and CellRef, or an earlier result row's source.
  std::string source;

  bool operator==(const ResultRow &) const = default;
};

/// Write rows on d.result; d.source hangs an inspectable provenance cell from
/// each row. The returned version is the store's designated current version.
MicroversionId writeResultSlice(Store &store, std::span<const ResultRow> rows);

/// Read the public result rank, preserving row order and provenance.
[[nodiscard]] std::vector<ResultRow> readResultSlice(const Store &store,
                                                     MicroversionId version);

} // namespace xanadu

#endif // COMMON_XANADU_RESULT_SLICE_HPP
