/**
 * @file sql_backend.hpp
 * @brief SqlBackend: the pluggable backend port for SqlLens
 * (design/sql-to-zigzag.md §7.1).
 *
 * SqlLens is backend-agnostic; everything SQLite- or PostgreSQL-shaped lives
 * behind this port, with <sqlite3.h>/<libpq-fe.h> confined to backend
 * translation units. One backend owns one connection and serializes its
 * window cursors; threading is the app's (backends are single-threaded by
 * contract).
 */
#ifndef COMMON_XANADU_ZIGZAG_SQL_BACKEND_HPP
#define COMMON_XANADU_ZIGZAG_SQL_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace zigzag {

/// One column of a result or a schema table, as the backend reports it.
struct SqlColumn {
  std::string name;
  std::string declType;                 ///< declared type, "" if none
  std::optional<std::string> baseTable; ///< set iff attributable (spec §4.3)
  bool keyColumn{false};                ///< declared PK member
  std::uint32_t keyPosition{0};         ///< its position in the PK (spec §4.3)
};

struct SqlTable {         ///< for the schema projection (spec §3.6)
  std::string schemaName; ///< "" for sqlite "main"; pg schema
  std::string name;       ///< canonical case (spec §4.4)
  bool isView{false};
  /// An implicit rowid exists (a plain rowid table -- false for WITHOUT
  /// ROWID tables and views; probed at schema() time).
  bool hasRowid{false};
  std::vector<SqlColumn> columns;
};

/// A backend-typed value. monostate is SQL NULL (spec §3.4); bytes is a blob
/// fetched up to the backend's byte budget (spec §3.5).
using SqlValue = std::variant<std::monostate, std::int64_t, double, std::string,
                              std::vector<std::byte>>;

struct SqlCapabilities {
  bool keysetPaging{false};     ///< row-value tuple predicates
  bool columnProvenance{false}; ///< base-table attribution per column
  bool snapshots{false};        ///< consistent-read scope (WAL / txn cursor)
  bool blobReopen{false};       ///< rowid point-select / sqlite3_blob_open
};

/// One active window cursor: RAII over a prepared read statement. Move-only.
/// fetchNext reads the cursor forward -- the live-window path that preserves
/// the query's own ORDER BY. A cursor prepared while a SqlSnapshot is live
/// runs inside that snapshot's scope.
class SqlCursor {
public:
  virtual ~SqlCursor()                                                = default;
  [[nodiscard]] virtual const std::vector<SqlColumn> &columns() const = 0;
  [[nodiscard]] virtual std::expected<std::vector<std::vector<SqlValue>>,
                                      std::string>
  fetchNext(std::size_t limit) = 0;
};

/// A consistent-read scope (spec §5.3 replay-3): RAII transaction guard.
/// Outlives sequential cursors; any cursor prepared while it is live joins
/// its scope.
class SqlSnapshot {
public:
  virtual ~SqlSnapshot() = default;
};

class SqlBackend {
public:
  virtual ~SqlBackend() = default;

  [[nodiscard]] virtual std::expected<std::vector<SqlTable>, std::string>
  schema() = 0;

  /// Prepare a read-only statement for WINDOW use. Refuses anything that
  /// writes, anything that isn't a data statement (BEGIN/ROLLBACK/&c. --
  /// spec §7.2), and multi-statement input (tail must be empty). Refuses
  /// with unexpected while another WINDOW cursor is live -- one connection
  /// serializes its windows. Internal reads (schema(), pragmas, blob
  /// point-selects) take a separate internal path and don't count.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlCursor>, std::string>
  prepare(std::string_view sql, std::span<const SqlValue> params) = 0;

  /// Re-prepare a bounded window for replay: the backend owns the wrapping
  /// (subquery+LIMIT/OFFSET for ordinal; tuple predicate for keyset) so the
  /// user's SQL text is never text-surgered. Restricted to SELECT/VALUES/
  /// WITH..SELECT input -- EXPLAIN and PRAGMA can't nest inside FROM (..)
  /// and report the parse error honestly.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlCursor>, std::string>
  replayWindow(std::string_view sql, std::span<const SqlValue> params,
               std::span<const SqlValue> lo, std::span<const SqlValue> hi,
               std::size_t offset, std::size_t limit) = 0;

  /// Consistent-read scope for snapshot replay; refused where unsupported
  /// (rollback-journal SQLite). Scopes any cursor prepared while it lives.
  [[nodiscard]] virtual std::expected<std::unique_ptr<SqlSnapshot>, std::string>
  snapshot() = 0;

  /// One write statement back through the source -- the lens's
  /// applyContentChange half. Params bind as prepare() does; returns
  /// rows changed. Default: refuse (read-only backend).
  [[nodiscard]] virtual std::expected<std::uint64_t, std::string>
  execWrite(std::string_view /*sql*/, std::span<const SqlValue> /*params*/) {
    return std::unexpected("backend is read-only");
  }

  [[nodiscard]] virtual SqlCapabilities capabilities() const noexcept = 0;
};

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SQL_BACKEND_HPP
