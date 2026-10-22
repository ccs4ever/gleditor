/**
 * @file sqlite_backend.hpp
 * @brief SqliteBackend: the round-one SqlBackend implementation over a
 * read-only sqlite3 connection (design/sql-to-zigzag.md §7.2).
 */
#ifndef COMMON_XANADU_ZIGZAG_SQLITE_BACKEND_HPP
#define COMMON_XANADU_ZIGZAG_SQLITE_BACKEND_HPP

#include "common/xanadu/zigzag/sql_backend.hpp"

#include <string>
#include <string_view>

namespace zigzag {

/// Open-time knobs. blobBytes is the per-value fetch budget (spec §3.5);
/// progressSliceMs bounds any single statement's wall time before the
/// progress handler interrupts it (a runaway query returns unexpected, not a
/// hang).
struct SqliteOpenOptions {
  std::size_t blobBytes{64 * 1024};
  unsigned progressSliceMs{5000};
};

/// Open a database read-only. The path is percent-encoded into a file: URI
/// (a literal '?'/'#' in a filename would otherwise parse as URI query and
/// smuggle flags); an input already starting with "file:" is used verbatim.
/// Refuses on SQLite < 3.30 (NULLS FIRST|LAST syntax + the read-only hot-wal
/// fallback floor, spec §7.2) and on open errors.
[[nodiscard]] std::expected<std::unique_ptr<SqlBackend>, std::string>
openSqliteBackend(std::string_view path, const SqliteOpenOptions &opts = {});

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SQLITE_BACKEND_HPP
