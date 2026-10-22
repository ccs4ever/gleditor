/**
 * @file sql_keys.hpp
 * @brief The normative ExternalKey/value encodings for SqlLens
 * (design/sql-to-zigzag.md §4.1) -- typed sql-values, percent-encoded name
 * components, the <dbid> file hash, and display renderings.
 */
#ifndef COMMON_XANADU_ZIGZAG_SQL_KEYS_HPP
#define COMMON_XANADU_ZIGZAG_SQL_KEYS_HPP

#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/zigzag/sql_backend.hpp"

namespace zigzag {

/// Percent-encode a key component: everything outside [A-Za-z0-9-_~] becomes
/// %XX. The reserved set {%, :, comma, @, NUL, .} is what the productions
/// delimit; over-encoding is safe because keys are compared, never parsed.
[[nodiscard]] std::string sqlKeyEncode(std::string_view component);

/// The <sql-value> production: n:/i:<dec>/f:<16hex bits>/t:<pct>/b:<sha256>.
/// A blob's digest is over the complete fetched value (§3.5) -- the b: key is
/// budget-independent by construction.
[[nodiscard]] std::string encodeSqlValue(const SqlValue &v);

/// A pk-rendering: sql-values joined by "," in declared PK order.
[[nodiscard]] std::string encodeSqlValues(std::span<const SqlValue> values);

/// sha256 hex of @p data -- the qhash/rowhash primitive.
[[nodiscard]] std::string sqlHashHex(std::string_view data);

/// The sqlite <dbid>: sha256 over the realpath(3) of the database file, so
/// two spellings of one file share one keyspace (§4.5).
[[nodiscard]] std::string dbidForPath(std::string_view path);

/// Human-readable value for cell content: ints/rows of text as text, a blob
/// as "blob(N B)" (or ", truncated" over budget), NULL as "NULL".
[[nodiscard]] std::string displaySqlValue(const SqlValue &v,
                                          bool truncated = false);

/// A row's display key: the pk-rendering in readable form (§3.3).
[[nodiscard]] std::string displaySqlValues(std::span<const SqlValue> values);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SQL_KEYS_HPP
