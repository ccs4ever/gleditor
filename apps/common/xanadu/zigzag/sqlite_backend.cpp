/**
 * @file sqlite_backend.cpp
 * @brief SqliteBackend: read-only SQLite implementation of the SqlBackend
 * port (design/sql-to-zigzag.md §7.2).
 *
 * Layered read-only guarantee, in the spec's order: SQLITE_OPEN_READONLY at
 * the file boundary; PRAGMA query_only as belt; the statement-kind gate and
 * sqlite3_stmt_readonly at prepare time; and the authorizer as the
 * user-vs-internal boundary -- a user's BEGIN/ROLLBACK/SAVEPOINT passes
 * stmt_readonly, so statement kind and the internal flag do the separating
 * the C API cannot.
 */
#include "common/xanadu/zigzag/sqlite_backend.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zigzag {

namespace {

// Check the VM periodically so the elapsed-time limit can interrupt long
// statements without invoking the callback for every SQLite opcode.
constexpr int kProgressInstructionInterval{8192};
// Let short-lived writer locks clear, then return control instead of waiting
// indefinitely while a read lens is used from the UI.
constexpr auto kSqliteBusyWait = std::chrono::milliseconds{250};
// NULLS FIRST/LAST syntax and the read-only hot-WAL fallback both require
// SQLite 3.30 or newer.
constexpr int kMinimumSqliteVersion{3'030'000};

/// Percent-encode a filesystem path for a file: URI. '/' is the path
/// separator and passes through; RFC-3986 unreserved bytes pass; everything
/// else encodes as %XX. A literal '?', '#' or '%' in a filename would
/// otherwise be misread by the URI parser.
std::string encodePathForUri(std::string_view path) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(path.size() + 8);
  for (const unsigned char c : path) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~' || c == '/';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

/// Skip whitespace and SQL comments; return the offset of the first
/// statement byte.
std::size_t skipTrivia(std::string_view sql) {
  std::size_t i = 0;
  for (;;) {
    while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) {
      ++i;
    }
    if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
      const auto nl = sql.find('\n', i);
      i             = (nl == std::string_view::npos) ? sql.size() : nl + 1;
      continue;
    }
    if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
      const auto end = sql.find("*/", i + 2);
      if (end == std::string_view::npos) {
        return sql.size();
      }
      i = end + 2;
      continue;
    }
    return i;
  }
}

/// The first keyword of a statement, uppercased. Empty when the input has no
/// statement (whitespace/comments only).
std::string firstKeyword(std::string_view sql) {
  const std::size_t i = skipTrivia(sql);
  std::size_t j       = i;
  while (j < sql.size() && std::isalpha(static_cast<unsigned char>(sql[j]))) {
    ++j;
  }
  std::string kw(sql.substr(i, j - i));
  for (auto &c : kw) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return kw;
}

bool isDataStatementKind(std::string_view kw) {
  return kw == "SELECT" || kw == "VALUES" || kw == "WITH" || kw == "EXPLAIN" ||
         kw == "PRAGMA";
}

bool isWrappableKind(std::string_view kw) {
  return kw == "SELECT" || kw == "VALUES" || kw == "WITH";
}

/// Functions the authorizer always refuses -- extension loading and file
/// access do not belong inside a read-only projection.
bool isDeniedFunction(std::string_view name) {
  return name == "load_extension" || name == "writefile" ||
         name == "readfile" || name == "fts3_tokenizer";
}

/// Intrinsically read-only pragmas; the argument position is a table/index
/// name, not a value to write.
bool isReadPragma(std::string_view name) {
  static const char *kRead[] = {
      "table_xinfo", "table_info",        "foreign_key_list", "index_list",
      "index_xinfo", "data_version",      "database_list",    "schema_version",
      "page_count",  "page_size",         "compile_options",  "integrity_check",
      "quick_check", "foreign_key_check",
  };
  return std::ranges::any_of(kRead, [&](const char *p) { return name == p; });
}

/// Dual-mode pragmas allowed only in read form (second argument NULL).
bool isDualPragma(std::string_view name) {
  return name == "journal_mode" || name == "query_only";
}

/// Double-quote an SQL identifier, doubling embedded quotes.
std::string quoteIdent(std::string_view id) {
  std::string out;
  out.reserve(id.size() + 2);
  out.push_back('"');
  for (const char c : id) {
    if (c == '"') {
      out.push_back('"');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

/// Single-quote an SQL string literal, doubling embedded quotes.
std::string quoteLiteral(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (const char c : s) {
    if (c == '\'') {
      out.push_back('\'');
    }
    out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

} // namespace

/// The concrete backend. Defined here so <sqlite3.h> stays in this TU (the
/// SwarmCatalogIndex::Impl pattern, spec §2.3).
class SqliteBackend final : public SqlBackend {
public:
  SqliteBackend(sqlite3 *db, SqliteOpenOptions opts, std::string journalMode)
      : db_(db), opts_(opts), journalMode_(std::move(journalMode)) {
    sqlite3_progress_handler(db_, kProgressInstructionInterval,
                             &SqliteBackend::progressThunk, this);
  }
  ~SqliteBackend() override {
    // Finalize any live window cursor before closing.
    windowCursorLive_ = false;
    sqlite3_close_v2(db_);
  }

  SqliteBackend(const SqliteBackend &)            = delete;
  SqliteBackend &operator=(const SqliteBackend &) = delete;

  // -- the authorizer boundary ------------------------------------------------

  /// pArg for sqlite3_set_authorizer; internal_ flips during the backend's
  /// own transaction/pragma work so the authorizer can tell it apart from
  /// user SQL.
  static int authorizeThunk(void *pArg, int code, const char *z1,
                            const char *z2, const char * /*dbName*/,
                            const char * /*cause*/) {
    const auto *self = static_cast<const SqliteBackend *>(pArg);
    switch (code) {
    case SQLITE_SELECT:
    case SQLITE_READ:
    case SQLITE_RECURSIVE:
      return SQLITE_OK;
    case SQLITE_TRANSACTION:
    case SQLITE_SAVEPOINT:
      return self->internal_ ? SQLITE_OK : SQLITE_DENY;
    case SQLITE_FUNCTION:
      return (z2 && isDeniedFunction(z2)) ? SQLITE_DENY : SQLITE_OK;
    case SQLITE_PRAGMA: {
      const std::string_view name = z1 ? z1 : "";
      if (isReadPragma(name)) {
        return SQLITE_OK;
      }
      if (isDualPragma(name) && z2 == nullptr) {
        return SQLITE_OK;
      }
      return SQLITE_DENY;
    }
    case SQLITE_UPDATE:
    case SQLITE_INSERT:
    case SQLITE_DELETE:
      // The write half opens these only on a writable handle; the
      // read-cursor path's own statement-kind gate still refuses user
      // writes regardless.
      return self->opts_.writable ? SQLITE_OK : SQLITE_DENY;
    case SQLITE_ATTACH:
    case SQLITE_DETACH:
    case SQLITE_ANALYZE:
    case SQLITE_REINDEX:
      return SQLITE_DENY;
    default:
      return SQLITE_DENY; // all DDL/DML codes fall through
    }
  }

  void installAuthorizer() {
    sqlite3_set_authorizer(db_, &SqliteBackend::authorizeThunk, this);
  }

  /// RAII: marks an operation as backend-internal for the authorizer.
  struct InternalScope {
    SqliteBackend &be;
    explicit InternalScope(SqliteBackend &b) : be(b) { be.internal_ = true; }
    ~InternalScope() { be.internal_ = false; }
  };

  /// Run internal SQL that must survive the authorizer (BEGIN/ROLLBACK for
  /// snapshots, schema/pragma reads). Returns the exec error text.
  std::string internalExec(std::string_view sql) {
    InternalScope guard(*this);
    char *err = nullptr;
    std::string body(sql);
    const int rc    = sqlite3_exec(db_, body.c_str(), nullptr, nullptr, &err);
    std::string out = err ? err : "";
    sqlite3_free(err);
    if (rc != SQLITE_OK && out.empty()) {
      out = sqlite3_errmsg(db_);
    }
    return out;
  }

  /// Internal row query: returns every row as text (the schema/pragma
  /// shapes are small and stringly by nature).
  std::expected<std::vector<std::vector<std::string>>, std::string>
  internalQuery(std::string_view sql) {
    InternalScope guard(*this);
    sqlite3_stmt *st = nullptr;
    const std::string body(sql);
    std::vector<std::vector<std::string>> rows;
    if (sqlite3_prepare_v2(db_, body.c_str(), -1, &st, nullptr) != SQLITE_OK) {
      return std::unexpected(sqlite3_errmsg(db_));
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      std::vector<std::string> row;
      const int n = sqlite3_column_count(st);
      row.reserve(static_cast<std::size_t>(n));
      for (int i = 0; i < n; ++i) {
        const auto *t = sqlite3_column_text(st, i);
        row.emplace_back(t ? reinterpret_cast<const char *>(t) : "");
      }
      rows.push_back(std::move(row));
    }
    sqlite3_finalize(st);
    return rows;
  }

  // -- value <-> bind
  // ----------------------------------------------------------

  static void bindValue(sqlite3_stmt *st, int index, const SqlValue &v) {
    std::visit(
        [&](const auto &x) {
          using T = std::decay_t<decltype(x)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            sqlite3_bind_null(st, index);
          } else if constexpr (std::is_same_v<T, std::int64_t>) {
            sqlite3_bind_int64(st, index, static_cast<sqlite3_int64>(x));
          } else if constexpr (std::is_same_v<T, double>) {
            sqlite3_bind_double(st, index, x);
          } else if constexpr (std::is_same_v<T, std::string>) {
            sqlite3_bind_text(st, index, x.data(), static_cast<int>(x.size()),
                              SQLITE_TRANSIENT);
          } else {
            sqlite3_bind_blob(st, index, x.data(), static_cast<int>(x.size()),
                              SQLITE_TRANSIENT);
          }
        },
        v);
  }

  SqlValue columnValue(sqlite3_stmt *st, int i) const {
    switch (sqlite3_column_type(st, i)) {
    case SQLITE_INTEGER:
      return SqlValue{static_cast<std::int64_t>(sqlite3_column_int64(st, i))};
    case SQLITE_FLOAT:
      return SqlValue{sqlite3_column_double(st, i)};
    case SQLITE_TEXT: {
      const auto *t =
          reinterpret_cast<const char *>(sqlite3_column_text(st, i));
      const auto n = sqlite3_column_bytes(st, i);
      return SqlValue{std::string(t ? t : "", static_cast<std::size_t>(n))};
    }
    case SQLITE_BLOB: {
      const auto *p  = sqlite3_column_blob(st, i);
      const auto n   = sqlite3_column_bytes(st, i);
      const auto cap = static_cast<int>(
          std::min<std::size_t>(opts_.blobBytes, static_cast<std::size_t>(n)));
      std::vector<std::byte> bytes(static_cast<std::size_t>(cap));
      if (cap > 0 && p) {
        std::memcpy(bytes.data(), p, static_cast<std::size_t>(cap));
      }
      return SqlValue{std::move(bytes)};
    }
    default:
      return SqlValue{std::monostate{}};
    }
  }

  // -- PK provenance
  // ------------------------------------------------------------

  /// Lazily-cached PK membership per table: column name -> 1-based position.
  const std::unordered_map<std::string, std::uint32_t> &
  pkOf(const std::string &table) {
    if (const auto it = pkCache_.find(table); it != pkCache_.end()) {
      return it->second;
    }
    std::unordered_map<std::string, std::uint32_t> m;
    const std::string sql = "PRAGMA table_xinfo(" + quoteLiteral(table) + ")";
    if (auto rows = internalQuery(sql); rows) {
      // table_xinfo columns: cid, name, type, notnull, dflt_value, pk, hidden
      for (const auto &r : *rows) {
        if (r.size() > 5 && !r[5].empty() && r[5] != "0") {
          m.emplace(r[1], static_cast<std::uint32_t>(
                              std::strtoul(r[5].c_str(), nullptr, 10)));
        }
      }
    }
    const auto [it, _] = pkCache_.emplace(table, std::move(m));
    return it->second;
  }

  // -- the cursor
  // ----------------------------------------------------------------

  class Cursor final : public SqlCursor {
  public:
    Cursor(SqliteBackend &be, sqlite3_stmt *st, std::vector<SqlColumn> cols)
        : be_(&be), st_(st), cols_(std::move(cols)) {}
    ~Cursor() override {
      sqlite3_finalize(st_);
      if (be_) {
        be_->windowCursorLive_ = false;
      }
    }
    Cursor(Cursor &&o) noexcept
        : be_(o.be_), st_(o.st_), cols_(std::move(o.cols_)) {
      o.be_ = nullptr;
      o.st_ = nullptr;
    }
    Cursor &operator=(Cursor &&o) noexcept {
      if (this != &o) {
        sqlite3_finalize(st_);
        if (be_) {
          be_->windowCursorLive_ = false;
        }
        be_   = o.be_;
        st_   = o.st_;
        cols_ = std::move(o.cols_);
        o.be_ = nullptr;
        o.st_ = nullptr;
      }
      return *this;
    }
    Cursor(const Cursor &)            = delete;
    Cursor &operator=(const Cursor &) = delete;

    [[nodiscard]] const std::vector<SqlColumn> &columns() const override {
      return cols_;
    }

    [[nodiscard]] std::expected<std::vector<std::vector<SqlValue>>, std::string>
    fetchNext(std::size_t limit) override {
      if (!be_) {
        return std::unexpected("cursor moved-from");
      }
      if (done_) {
        return std::vector<std::vector<SqlValue>>{};
      }
      be_->deadline_ = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(be_->opts_.progressSliceMs);
      std::vector<std::vector<SqlValue>> rows;
      const int ncols = sqlite3_column_count(st_);
      while (rows.size() < limit) {
        const int rc = sqlite3_step(st_);
        if (rc == SQLITE_DONE) {
          done_ = true;
          break;
        }
        if (rc != SQLITE_ROW) {
          return std::unexpected(sqlite3_errmsg(be_->db_));
        }
        std::vector<SqlValue> row;
        row.reserve(static_cast<std::size_t>(ncols));
        for (int i = 0; i < ncols; ++i) {
          row.push_back(be_->columnValue(st_, i));
        }
        rows.push_back(std::move(row));
      }
      return rows;
    }

  private:
    SqliteBackend *be_;
    sqlite3_stmt *st_;
    std::vector<SqlColumn> cols_;
    /// sqlite3_step() past SQLITE_DONE re-runs the statement rather than
    /// erroring -- latch it or a paged reader loops forever.
    bool done_{false};
  };

  // -- prepare
  // -------------------------------------------------------------------

  std::expected<sqlite3_stmt *, std::string>
  prepareGated(std::string_view sql, std::span<const SqlValue> params) {
    const std::string kw = firstKeyword(sql);
    if (kw.empty()) {
      return std::unexpected("empty statement");
    }
    if (!isDataStatementKind(kw)) {
      return std::unexpected("statement kind not allowed: " + kw);
    }
    const std::string body(sql);
    sqlite3_stmt *st = nullptr;
    const char *tail = nullptr;
    const int prc    = sqlite3_prepare_v2(
        db_, body.c_str(), static_cast<int>(body.size()) + 1, &st, &tail);
    if (prc != SQLITE_OK) {
      return std::unexpected(sqlite3_errmsg(db_));
    }
    if (st == nullptr) {
      return std::unexpected("empty statement");
    }
    // Single-statement rule: the tail may only be trivia.
    if (!firstKeyword(tail).empty()) {
      sqlite3_finalize(st);
      return std::unexpected("multiple statements not allowed");
    }
    if (!sqlite3_stmt_readonly(st)) {
      sqlite3_finalize(st);
      return std::unexpected("statement is not read-only");
    }
    for (int i = 0; i < static_cast<int>(params.size()); ++i) {
      bindValue(st, i + 1, params[static_cast<std::size_t>(i)]);
    }
    return st;
  }

  std::vector<SqlColumn> describe(sqlite3_stmt *st) {
    std::vector<SqlColumn> cols;
    const int n = sqlite3_column_count(st);
    cols.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      SqlColumn c;
      const char *name = sqlite3_column_name(st, i);
      c.name           = name ? name : "";
      const char *dt   = sqlite3_column_decltype(st, i);
      c.declType       = dt ? dt : "";
#if defined(SQLITE_ENABLE_COLUMN_METADATA)
      const char *tn = sqlite3_column_table_name(st, i);
      if (tn) {
        c.baseTable    = std::string(tn);
        const auto &pk = pkOf(*tn);
        if (const auto it = pk.find(c.name); it != pk.end()) {
          c.keyPosition = it->second;
        }
      }
#endif
      cols.push_back(std::move(c));
    }
    return cols;
  }

  // -- SqlBackend
  // -----------------------------------------------------------------

  std::expected<std::vector<SqlTable>, std::string> schema() override {
    auto rows = internalQuery(
        "SELECT name, type FROM sqlite_schema WHERE type IN ('table','view') "
        "AND name NOT LIKE 'sqlite_%' ORDER BY name");
    if (!rows) {
      return std::unexpected(rows.error());
    }
    std::vector<SqlTable> tables;
    for (const auto &r : *rows) {
      if (r.size() < 2) {
        continue;
      }
      SqlTable t;
      t.schemaName = "main";
      t.name       = r[0];
      t.isView     = (r[1] == "view");
      if (!t.isView) {
        // rowid probe: WITHOUT ROWID tables answer "no such column".
        const auto rid = internalQuery("SELECT rowid FROM " +
                                       quoteIdent(t.name) + " LIMIT 0");
        t.hasRowid     = rid.has_value();
      }
      const auto cols =
          internalQuery("PRAGMA table_xinfo(" + quoteLiteral(t.name) + ")");
      if (cols) {
        for (const auto &cr : *cols) {
          if (cr.size() < 6) {
            continue;
          }
          SqlColumn c;
          c.name     = cr[1];
          c.declType = cr[2];
          if (cr[5] != "0") {
            c.keyPosition = static_cast<std::uint32_t>(
                std::strtoul(cr[5].c_str(), nullptr, 10));
          }
          t.columns.push_back(std::move(c));
        }
      }
      tables.push_back(std::move(t));
    }
    return tables;
  }

  std::expected<std::unique_ptr<SqlCursor>, std::string>
  prepare(std::string_view sql, std::span<const SqlValue> params) override {
    if (windowCursorLive_) {
      return std::unexpected("a window cursor is already live");
    }
    auto st = prepareGated(sql, params);
    if (!st) {
      return std::unexpected(st.error());
    }
    windowCursorLive_ = true;
    return std::unique_ptr<SqlCursor>(new Cursor(*this, *st, describe(*st)));
  }

  std::expected<std::unique_ptr<SqlCursor>, std::string>
  replayWindow(std::string_view sql, std::span<const SqlValue> params,
               std::span<const SqlValue> lo, std::span<const SqlValue> hi,
               std::size_t offset, std::size_t limit) override {
    if (windowCursorLive_) {
      return std::unexpected("a window cursor is already live");
    }
    const std::string kw = firstKeyword(sql);
    if (!isWrappableKind(kw)) {
      return std::unexpected(
          "replay only wraps SELECT/VALUES/WITH..SELECT input");
    }
    // Strip a trailing ';' -- it would terminate the subquery early.
    std::string body(sql);
    const auto last = body.find_last_not_of(" \t\n\r");
    if (last != std::string::npos && body[last] == ';') {
      body.erase(last);
    }
    std::string wrapped = "SELECT * FROM (" + body + ") LIMIT ? OFFSET ?";
    sqlite3_stmt *st    = nullptr;
    const int prc =
        sqlite3_prepare_v2(db_, wrapped.c_str(),
                           static_cast<int>(wrapped.size()) + 1, &st, nullptr);
    if (prc != SQLITE_OK || st == nullptr) {
      return std::unexpected(sqlite3_errmsg(db_));
    }
    int idx = 1;
    for (const auto &v : params) {
      bindValue(st, idx++, v);
    }
    for (const auto &v : lo) {
      bindValue(st, idx++, v);
    }
    for (const auto &v : hi) {
      bindValue(st, idx++, v);
    }
    sqlite3_bind_int64(st, idx++, static_cast<sqlite3_int64>(limit));
    sqlite3_bind_int64(st, idx, static_cast<sqlite3_int64>(offset));
    windowCursorLive_ = true;
    return std::unique_ptr<SqlCursor>(new Cursor(*this, st, describe(st)));
  }

  // -- snapshots (WAL only, spec §5.3/§7.2)
  // --------------------------------------

  class Snapshot final : public SqlSnapshot {
  public:
    explicit Snapshot(SqliteBackend &be) : be_(&be) {}
    ~Snapshot() override {
      if (be_) {
        (void)be_->internalExec("ROLLBACK");
        be_->snapshotLive_ = false;
      }
    }
    Snapshot(const Snapshot &)            = delete;
    Snapshot &operator=(const Snapshot &) = delete;

  private:
    SqliteBackend *be_;
  };

  std::expected<std::unique_ptr<SqlSnapshot>, std::string> snapshot() override {
    if (journalMode_ != "wal") {
      return std::unexpected(
          "snapshot replay needs WAL mode (journal_mode=" + journalMode_ + ")");
    }
    if (snapshotLive_) {
      return std::unexpected("a snapshot is already live");
    }
    if (const auto err = internalExec("BEGIN DEFERRED"); !err.empty()) {
      return std::unexpected(err);
    }
    snapshotLive_ = true;
    return std::unique_ptr<SqlSnapshot>(new Snapshot(*this));
  }

  /// The write half (spec's later phase): UPDATE/INSERT/DELETE through
  /// the same authorizer, on a writable handle only. Single statement;
  /// the tail check is sqlite3_prepare_v2's own.
  [[nodiscard]] std::expected<std::uint64_t, std::string>
  execWrite(std::string_view sql,
            const std::span<const SqlValue> params) override {
    if (!opts_.writable) {
      return std::unexpected("backend is read-only");
    }
    const std::string text{sql};
    sqlite3_stmt *st = nullptr;
    const char *tail = nullptr;
    const int rc     = sqlite3_prepare_v2(
        db_, text.c_str(), static_cast<int>(text.size()), &st, &tail);
    if (rc != SQLITE_OK || st == nullptr) {
      return std::unexpected(sqlite3_errmsg(db_));
    }
    if (tail != nullptr && *tail != '\0') {
      sqlite3_finalize(st);
      return std::unexpected("multi-statement input refused");
    }
    for (int i = 0; i < static_cast<int>(params.size()); ++i) {
      bindValue(st, i + 1, params[static_cast<std::size_t>(i)]);
    }
    const int step = sqlite3_step(st);
    if (step != SQLITE_DONE && step != SQLITE_ROW) {
      const std::string err = sqlite3_errmsg(db_);
      sqlite3_finalize(st);
      return std::unexpected(err);
    }
    // Rows a RETURNING clause produced are drained by the step loop;
    // one step is enough for plain DML.
    while (sqlite3_step(st) == SQLITE_ROW) {
    }
    const auto changed = static_cast<std::uint64_t>(sqlite3_changes64(db_));
    sqlite3_finalize(st);
    return changed;
  }

  SqlCapabilities capabilities() const noexcept override {
    return SqlCapabilities{
        .keysetPaging = true,
        .columnProvenance =
#if defined(SQLITE_ENABLE_COLUMN_METADATA)
            true,
#else
            false,
#endif
        .snapshots  = journalMode_ == "wal",
        .blobReopen = true,
    };
  }

  // -- progress handler
  // ----------------------------------------------------------

  static int progressThunk(void *pArg) {
    const auto *self = static_cast<const SqliteBackend *>(pArg);
    if (self->deadline_ == std::chrono::steady_clock::time_point::min()) {
      return 0;
    }
    return std::chrono::steady_clock::now() > self->deadline_ ? 1 : 0;
  }

  sqlite3 *db_;
  SqliteOpenOptions opts_;
  std::string journalMode_;
  bool internal_{false};
  bool windowCursorLive_{false};
  bool snapshotLive_{false};
  std::chrono::steady_clock::time_point deadline_{
      std::chrono::steady_clock::time_point::min()};
  std::unordered_map<std::string,
                     std::unordered_map<std::string, std::uint32_t>>
      pkCache_;
};

namespace {

/// Open-time pragma + probe sequence, run before the authorizer is
/// installed. Returns the journal_mode value, or an error.
std::expected<std::string, std::string> openPragmas(sqlite3 *db,
                                                    const bool writable) {
  char *err = nullptr;
  // query_only is the belt of the read-only guarantee; a writable
  // handle drops it but keeps the authorizer's statement gate.
  if (!writable && sqlite3_exec(db, "PRAGMA query_only = ON", nullptr, nullptr,
                                &err) != SQLITE_OK) {
    std::string out = err ? err : "query_only pragma failed";
    sqlite3_free(err);
    return std::unexpected(out);
  }
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, nullptr) !=
      SQLITE_OK) {
    return std::unexpected(sqlite3_errmsg(db));
  }
  std::string mode;
  if (sqlite3_step(st) == SQLITE_ROW) {
    const auto *t = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
    mode          = t ? t : "";
  }
  sqlite3_finalize(st);
  if (mode.empty()) {
    return std::unexpected("could not probe journal_mode");
  }
  return mode;
}

} // namespace

std::expected<std::unique_ptr<SqlBackend>, std::string>
openSqliteBackend(std::string_view path, const SqliteOpenOptions &opts) {
  if (sqlite3_libversion_number() < kMinimumSqliteVersion) {
    return std::unexpected("SQLite >= 3.30 required (NULLS FIRST|LAST + "
                           "read-only hot-wal fallback)");
  }
  std::string uri = path.starts_with("file:")
                        ? std::string(path)
                        : "file:" + encodePathForUri(path);
  sqlite3 *db     = nullptr;
  const int rc    = sqlite3_open_v2(
      uri.c_str(), &db,
      (opts.writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY) |
          SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
      nullptr);
  if (rc != SQLITE_OK) {
    std::string err = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
    if (db) {
      sqlite3_close_v2(db);
    }
    return std::unexpected(err);
  }
  sqlite3_busy_timeout(db, static_cast<int>(kSqliteBusyWait.count()));
  auto mode = openPragmas(db, opts.writable);
  if (!mode) {
    std::string err = mode.error();
    sqlite3_close_v2(db);
    return std::unexpected(err);
  }
  auto be = std::unique_ptr<SqliteBackend>(
      new SqliteBackend(db, opts, std::move(*mode)));
  be->installAuthorizer();
  return be;
}

} // namespace zigzag
