/**
 * @file pg_backend.cpp
 * @brief PgBackend: libpq behind the GLEDITOR_SQL_PG probe (spec section
 *        7.3). One TU, two halves: GLEDITOR_HAVE_PG compiles the real
 *        backend; without it the same entry point refuses, so sql:pg:
 *        URIs always parse and always get an actionable answer.
 *
 * NOTE: the libpq half is written against libpq's documented API but
 * cannot be compiled on hosts without libpq -- first build under
 * GLEDITOR_SQL_PG=1 is where it meets a compiler.
 */

#include "common/xanadu/zigzag/pg_backend.hpp"

#ifdef GLEDITOR_HAVE_PG

#include <charconv>
#include <cstring>
#include <utility>
#include <vector>

#include <libpq-fe.h>

namespace zigzag {

namespace {

/// Values render as text (PQgetvalue); the GUCs pinned at connect keep
/// the renderings parse-stable (spec section 7.3 "Values").
SqlValue textToValue(const char *s, const bool isNull, const Oid type) {
  if (isNull) {
    return std::monostate{};
  }
  const std::string_view v{s};
  switch (type) {
  case 16: // bool -> int64 (SqlValue has no bool rung)
    return std::int64_t{v == "t" ? 1 : 0};
  case 20:   // int8
  case 21:   // int2
  case 23: { // int4
    std::int64_t n     = 0;
    const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
    if (ec == std::errc{} && p == v.data() + v.size()) {
      return n;
    }
    return std::string{v}; // out-of-range integer keys as t:
  }
  case 700:   // float4
  case 701: { // float8
    double d           = 0;
    const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), d);
    if (ec == std::errc{} && p == v.data() + v.size()) {
      return d;
    }
    if (v == "NaN") {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return std::string{v};
  }
  case 17: { // bytea: \x<hex> -> bytes
    if (v.starts_with("\\x")) {
      std::vector<std::byte> out;
      out.reserve((v.size() - 2) / 2);
      for (std::size_t i = 2; i + 1 < v.size(); i += 2) {
        const auto hex = v.substr(i, 2);
        unsigned byte  = 0;
        (void)std::from_chars(hex.data(), hex.data() + hex.size(), byte, 16);
        out.push_back(static_cast<std::byte>(byte));
      }
      return out;
    }
    return std::string{v};
  }
  default:
    // numeric/timestamp/interval/&c. keep their text -- pinned GUCs
    // make it stable, and t: is the honest key class (spec 7.3).
    return std::string{v};
  }
}

} // namespace

class PgBackend;

/// One statement's streamed rows: PQsendQueryParams + single-row mode
/// gives a server-side pull without hand-quoting literals -- $n binds
/// through the extended protocol, rows arrive one PQgetResult at a
/// time, and the open READ ONLY transaction scopes them (spec 7.3:
/// portals bound to their transaction). One live cursor per connection:
/// single-row mode is the connection's, so a cursor that stops early
/// drains its tail and releases the txn before anything else prepares.
class PgCursor final : public SqlCursor {
public:
  PgCursor(PgBackend *be, PGconn *conn, PGresult *head,
           std::vector<SqlColumn> cols, const bool ownsTxn)
      : be_(be), conn_(conn), head_(head), cols_(std::move(cols)),
        ownsTxn_(ownsTxn) {}
  ~PgCursor() override { finish(); }
  PgCursor(PgCursor &&o) noexcept
      : be_(o.be_), conn_(o.conn_), head_(o.head_), cols_(std::move(o.cols_)),
        ownsTxn_(o.ownsTxn_), done_(o.done_) {
    o.head_    = nullptr;
    o.done_    = true;
    o.ownsTxn_ = false;
  }
  PgCursor &operator=(PgCursor &&) = delete;

  [[nodiscard]] const std::vector<SqlColumn> &columns() const override {
    return cols_;
  }

  /// Pull up to @p limit rows; single-row mode delivers one result per
  /// PQgetResult, a null result ends the stream.
  [[nodiscard]] std::expected<std::vector<std::vector<SqlValue>>, std::string>
  fetchNext(const std::size_t limit) override {
    std::vector<std::vector<SqlValue>> rows;
    if (done_) {
      return rows;
    }
    while (rows.size() < limit) {
      // The prepare-time head may itself hold a row (the first
      // SINGLE_TUPLE); serve it before pulling further results.
      PGresult *r = head_;
      if (r != nullptr) {
        head_ = nullptr;
      } else {
        r = PQgetResult(conn_);
      }
      if (r == nullptr) {
        finish();
        return rows;
      }
      if (PQresultStatus(r) == PGRES_SINGLE_TUPLE) {
        std::vector<SqlValue> row;
        row.reserve(cols_.size());
        for (int c = 0; c < PQnfields(r); ++c) {
          row.push_back(textToValue(PQgetvalue(r, 0, c), PQgetisnull(r, 0, c),
                                    PQftype(r, c)));
        }
        rows.push_back(std::move(row));
        PQclear(r);
        continue;
      }
      const auto status = PQresultStatus(r);
      if (status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK) {
        PQclear(r);
        continue; // the empty tail result that ends single-row mode
      }
      std::string msg = PQresultErrorMessage(r);
      PQclear(r);
      finish();
      return std::unexpected(msg);
    }
    return rows;
  }

private:
  /// Drain any unread tail and hand the txn back -- the connection is
  /// free again only when every queued result has been consumed.
  void finish() {
    if (done_) {
      return;
    }
    done_ = true;
    if (head_ != nullptr) {
      PQclear(head_);
      head_ = nullptr;
    }
    while (PGresult *r = PQgetResult(conn_)) {
      PQclear(r);
    }
    release(); // backend's cursorClosed
  }
  void release();

  PgBackend *be_;
  PGconn *conn_;
  PGresult *head_{nullptr}; ///< unserved first result
  std::vector<SqlColumn> cols_;
  bool ownsTxn_{false};
  bool done_{false};
};

/// One open READ ONLY transaction scope; cursors run inside it and the
/// snapshot guard (or cursor teardown) COMMITs.
class PgSnapshot final : public SqlSnapshot {
public:
  explicit PgSnapshot(PgBackend *be) : be_(be) {}
  ~PgSnapshot() override;
  PgSnapshot(PgSnapshot &&o) noexcept : be_(o.be_) { o.be_ = nullptr; }

private:
  PgBackend *be_; ///< owns the txn flag; defined after PgBackend
};

class PgBackend final : public SqlBackend {
  // The cursor and snapshot drive its txn/cursor bookkeeping.
  friend class PgCursor;
  friend class PgSnapshot;

public:
  explicit PgBackend(PGconn *conn, const bool writable = false)
      : conn_(conn), writable_(writable) {}
  ~PgBackend() override {
    // A cursor/snapshot teardown that never ran leaves a live txn --
    // commit it only when one's actually open.
    if (inTxn_) {
      (void)execOk("COMMIT");
    }
    PQfinish(conn_);
  }

  [[nodiscard]] std::expected<std::vector<SqlTable>, std::string>
  schema() override {
    // information_schema for shape, pg_constraint.conkey for the PK
    // rung of the identity ladder (spec 7.3 "Provenance"); relkind='v'
    // marks views.
    static constexpr std::string_view sql = R"sql(
      SELECT t.table_schema, t.table_name, t.table_type,
             c.column_name, c.data_type, c.ordinal_position,
             (SELECT array_position(k.conkey, c.ordinal_position::int2)
                FROM pg_constraint k
                JOIN pg_class cls ON cls.oid = k.conrelid
                JOIN pg_namespace ns ON ns.oid = cls.relnamespace
               WHERE k.contype = 'p'
                 AND ns.nspname = t.table_schema
                 AND cls.relname = t.table_name) AS keypos
        FROM information_schema.tables t
        JOIN information_schema.columns c
          ON c.table_schema = t.table_schema
         AND c.table_name  = t.table_name
       WHERE t.table_schema NOT IN ('pg_catalog', 'information_schema')
       ORDER BY t.table_schema, t.table_name, c.ordinal_position
    )sql";
    PGresult *r                           = PQexec(conn_, sql.data());
    if (r == nullptr || PQresultStatus(r) != PGRES_TUPLES_OK) {
      std::string msg = r ? PQresultErrorMessage(r) : PQerrorMessage(conn_);
      PQclear(r);
      return std::unexpected(msg);
    }
    std::vector<SqlTable> out;
    std::string cur;
    for (int i = 0; i < PQntuples(r); ++i) {
      const std::string name =
          std::string{PQgetvalue(r, i, 0)} + "." + PQgetvalue(r, i, 1);
      if (name != cur) {
        cur = name;
        SqlTable t;
        t.schemaName = PQgetvalue(r, i, 0);
        t.name       = PQgetvalue(r, i, 1);
        t.isView     = std::string_view{PQgetvalue(r, i, 2)} == "VIEW";
        t.hasRowid   = false; // no rowid analogue: ctid moves on update
        out.push_back(std::move(t));
      }
      SqlColumn col;
      col.name     = PQgetvalue(r, i, 3);
      col.declType = PQgetvalue(r, i, 4);
      if (!PQgetisnull(r, i, 6)) {
        col.keyColumn   = true;
        col.keyPosition = std::stoul(PQgetvalue(r, i, 6));
      }
      out.back().columns.push_back(std::move(col));
    }
    PQclear(r);
    return out;
  }

  /// The only statement-kind boundary libpq can't check at parse: the
  /// backend refuses anything not SELECT/VALUES/WITH..SELECT before a
  /// byte leaves the process (spec 7.3's dual-mode stance), and the
  /// READ ONLY transaction catches what slips past the keyword check
  /// (data-modifying CTEs error at execution).
  [[nodiscard]] std::expected<void, std::string>
  readOnlyCheck(const std::string_view sql) {
    auto p = sql.find_first_not_of(" \t\r\n(");
    if (p == std::string_view::npos) {
      return std::unexpected("empty statement");
    }
    const auto kw  = sql.substr(p, 6);
    const auto kw5 = kw.substr(0, 5);
    if (kw.starts_with("SELECT") || kw5.starts_with("VALUE") ||
        kw.starts_with("VALUES") || kw.substr(0, 4).starts_with("WITH")) {
      return {};
    }
    return std::unexpected("refused: not a read-only statement");
  }

  /// Open a streaming cursor: an open READ ONLY txn per cursor (or the
  /// live snapshot's), PREPARE+Bind+Execute through the extended
  /// protocol so $n never becomes literal text, single-row mode so
  /// fetchNext pulls rather than buffers.
  [[nodiscard]] std::expected<std::unique_ptr<SqlCursor>, std::string>
  prepare(std::string_view sql,
          const std::span<const SqlValue> params) override {
    if (auto chk = readOnlyCheck(sql); !chk) {
      return std::unexpected(chk.error());
    }
    if (liveCursor_) {
      return std::unexpected(
          "a cursor is still live -- drain it first (one live cursor "
          "per connection under single-row mode)");
    }
    bool ownsTxn = false;
    if (!inTxn_) {
      if (auto r = execOk(writable_ ? "BEGIN" : "BEGIN READ ONLY"); !r) {
        return std::unexpected(r.error());
      }
      inTxn_  = true; // the cursor's txn; a live snapshot's scope joins
      ownsTxn = true;
    }
    const std::string text{sql};
    if (PQsendQueryParams(conn_, text.c_str(), static_cast<int>(params.size()),
                          nullptr, paramTexts(params).data(), nullptr, nullptr,
                          0) != 1) {
      if (ownsTxn) {
        endTxn();
      }
      return std::unexpected(PQerrorMessage(conn_));
    }
    PQsetSingleRowMode(conn_);
    // Single-row mode: every result carries its own field metadata, and
    // the first PGRES_SINGLE_TUPLE may arrive immediately -- there's no
    // separate describe result. Take the first row-bearing result (or
    // the TUPLES_OK tail of an empty set) and hand it to the cursor.
    PGresult *head = nullptr;
    std::vector<SqlColumn> cols;
    for (;;) {
      PGresult *r = PQgetResult(conn_);
      if (r == nullptr) {
        if (ownsTxn) {
          endTxn();
        }
        return std::unexpected("no results returned");
      }
      const auto st = PQresultStatus(r);
      if (st == PGRES_SINGLE_TUPLE || st == PGRES_TUPLES_OK) {
        head = r;
        break;
      }
      if (st == PGRES_FATAL_ERROR) {
        std::string msg = PQresultErrorMessage(r);
        PQclear(r);
        if (ownsTxn) {
          endTxn();
        }
        return std::unexpected(msg);
      }
      PQclear(r);
    }
    for (int c = 0; c < PQnfields(head); ++c) {
      SqlColumn col;
      col.name     = PQfname(head, c);
      col.declType = typeName(PQftype(head, c));
      // PQftable/PQftablecol attribute the column to its base table
      // (spec 7.3 provenance); 0 means "not attributable".
      if (const auto t = PQftable(head, c); t != 0) {
        col.baseTable = std::to_string(t); // oid; schema() resolves
      }
      cols.push_back(std::move(col));
    }
    liveCursor_ = true;
    return std::unique_ptr<SqlCursor>{
        new PgCursor(this, conn_, head, std::move(cols), ownsTxn)};
  }

  [[nodiscard]] std::expected<std::unique_ptr<SqlCursor>, std::string>
  replayWindow(std::string_view sql, std::span<const SqlValue> params,
               std::span<const SqlValue> /*lo*/,
               std::span<const SqlValue> /*hi*/, const std::size_t offset,
               const std::size_t limit) override {
    if (auto chk = readOnlyCheck(sql); !chk) {
      return std::unexpected(chk.error());
    }
    // Ordinal replay: the backend wraps -- the user's text is never
    // text-surgered beyond the documented SELECT * FROM (..) LIMIT form.
    const std::string wrapped = "SELECT * FROM (" + std::string{sql} +
                                ") __pglens LIMIT " + std::to_string(limit) +
                                " OFFSET " + std::to_string(offset);
    return prepare(wrapped, params);
  }

  [[nodiscard]] std::expected<std::unique_ptr<SqlSnapshot>, std::string>
  snapshot() override {
    if (inTxn_) {
      return std::unexpected("a transaction scope is already open");
    }
    if (auto r = execOk(writable_ ? "BEGIN ISOLATION LEVEL REPEATABLE READ"
                                  : "BEGIN ISOLATION LEVEL REPEATABLE READ "
                                    "READ ONLY");
        !r) {
      return std::unexpected(r.error());
    }
    inTxn_ = true;
    return std::unique_ptr<SqlSnapshot>{new PgSnapshot(this)};
  }

  [[nodiscard]] SqlCapabilities capabilities() const noexcept override {
    return {.keysetPaging     = true,
            .columnProvenance = true,
            .snapshots        = true,
            .blobReopen       = false}; // large objects are OID refs
  }

  /// DML through PQexecParams: $n binds as prepare()'s read path does;
  /// UPDATE/INSERT/DELETE come back as COMMAND_OK + cmdTuples.
  [[nodiscard]] std::expected<std::uint64_t, std::string>
  execWrite(std::string_view sql,
            const std::span<const SqlValue> params) override {
    if (!writable_) {
      return std::unexpected("backend is read-only");
    }
    if (liveCursor_) {
      return std::unexpected("a cursor is still live -- drain it first");
    }
    const std::string text{sql};
    PGresult *r =
        PQexecParams(conn_, text.c_str(), static_cast<int>(params.size()),
                     nullptr, paramTexts(params).data(), nullptr, nullptr, 0);
    if (r == nullptr) {
      return std::unexpected(PQerrorMessage(conn_));
    }
    const auto status = PQresultStatus(r);
    if (status != PGRES_COMMAND_OK) {
      std::string msg = PQresultErrorMessage(r);
      PQclear(r);
      return std::unexpected(msg);
    }
    const auto changed =
        // Non-DML commands (NOTIFY, SET) carry no tuple count.
        PQcmdTuples(r)[0] == '\0'
            ? 0u
            : static_cast<std::uint64_t>(std::stoull(PQcmdTuples(r)));
    PQclear(r);
    return changed;
  }

  [[nodiscard]] std::string
  paramPlaceholder(const std::size_t n) const override {
    return "$" + std::to_string(n);
  }
  [[nodiscard]] std::string nullSafeEq(std::string_view colSql,
                                       const std::size_t n) const override {
    return std::string{colSql} + " IS NOT DISTINCT FROM " + paramPlaceholder(n);
  }

  /// A pushed source where there is one: libpq's async NOTIFY lands
  /// without a round-trip -- consume input (moves the socket's buffer
  /// in), then drain the notification queue. Any pending NOTIFY is a
  /// change hint; the schema/data diff on refresh() is what confirms it.
  bool pollChanged() override {
    if (!conn_ || !PQconsumeInput(conn_)) {
      return false;
    }
    bool dirty = false;
    while (PGnotify *n = PQnotifies(conn_)) {
      dirty = true;
      PQfreemem(n);
    }
    return dirty;
  }

  /// The connection's socket -- an event loop can wait on it; the
  /// drain in pollChanged() then runs.
  [[nodiscard]] std::optional<int> waitableFd() noexcept override {
    const int s = PQsocket(conn_);
    return s >= 0 ? std::optional<int>{s} : std::nullopt;
  }

  /// LISTEN is a subscription, not a cursor -- execOk'd, not prepared.
  /// Channel names are identifier-quoted; the same sanitization as
  /// column names applies.
  [[nodiscard]] std::expected<void, std::string>
  listenChannel(const std::string_view channel) override {
    std::string name;
    name.reserve(channel.size());
    for (const char c : channel) {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' &&
          c != '-' && c != '.') {
        return std::unexpected("unsafe channel name");
      }
      name += c;
    }
    return execOk(("LISTEN \"" + name + "\"").c_str());
  }

private:
  std::expected<void, std::string> execOk(const char *sql) {
    PGresult *r = PQexec(conn_, sql);
    if (r == nullptr || PQresultStatus(r) != PGRES_COMMAND_OK) {
      std::string msg = r ? PQresultErrorMessage(r) : PQerrorMessage(conn_);
      PQclear(r);
      return std::unexpected(msg);
    }
    PQclear(r);
    return {};
  }
  void endTxn() {
    if (inTxn_) {
      (void)execOk("COMMIT");
      inTxn_ = false;
    }
  }
  /// A cursor's tail is drained; release the live flag, and the txn it
  /// owned (a snapshot's txn stays the snapshot's).
  void cursorClosed(const bool ownedTxn) {
    liveCursor_ = false;
    if (ownedTxn) {
      endTxn();
    }
  }
  /// $n parameters render as text; single-row mode reads them back the
  /// same way. Blobs would need binary format -- a later round.
  std::vector<const char *> paramTexts(std::span<const SqlValue> params) {
    texts_.clear();
    ptrs_.clear();
    for (const auto &v : params) {
      std::visit(
          [&](const auto &x) {
            using T = std::remove_cvref_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
              texts_.push_back({});
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
              texts_.push_back(std::to_string(x));
            } else if constexpr (std::is_same_v<T, double>) {
              texts_.push_back(std::to_string(x));
            } else if constexpr (std::is_same_v<T, std::string>) {
              texts_.push_back(x);
            } else {
              // \x hex bytea literal -- the backend-side cast reads it.
              std::string hex{"\\x"};
              for (const auto b : x) {
                char buf[3];
                (void)std::snprintf(buf, sizeof buf, "%02x",
                                    static_cast<unsigned>(b));
                hex += buf;
              }
              texts_.push_back(std::move(hex));
            }
          },
          v);
    }
    for (const auto &t : texts_) {
      ptrs_.push_back(t.empty() ? nullptr : t.c_str());
    }
    return ptrs_;
  }
  static const char *typeName(const Oid t) {
    switch (t) {
    case 16:
      return "bool";
    case 17:
      return "bytea";
    case 20:
      return "int8";
    case 21:
      return "int2";
    case 23:
      return "int4";
    case 700:
      return "float4";
    case 701:
      return "float8";
    case 25:
      return "text";
    case 1043:
      return "varchar";
    case 1700:
      return "numeric";
    case 1114:
      return "timestamp";
    case 1184:
      return "timestamptz";
    default:
      return "any";
    }
  }

  PGconn *conn_;
  bool writable_{false};
  bool inTxn_{false};
  bool liveCursor_{false};
  std::vector<std::string> texts_;
  std::vector<const char *> ptrs_;
};

void PgCursor::release() { be_->cursorClosed(ownsTxn_); }
PgSnapshot::~PgSnapshot() {
  if (be_ != nullptr) {
    be_->endTxn();
  }
}

std::expected<std::unique_ptr<SqlBackend>, std::string>
openPgBackend(const std::string_view conninfo, const PgOpenOptions &opts) {
  const std::string ci{conninfo};
  PGconn *conn = PQconnectdb(ci.c_str());
  if (conn == nullptr) {
    return std::unexpected("libpq could not allocate a connection");
  }
  if (PQstatus(conn) != CONNECTION_OK) {
    const std::string msg = PQerrorMessage(conn);
    PQfinish(conn);
    return std::unexpected(msg);
  }
  // Value rendering is text; these pins make the text stable (spec 7.3).
  // A writable handle drops only the read-only pin.
  std::vector<std::string_view> gucs{
      "SET statement_timeout = '30s'", "SET DateStyle = ISO",
      "SET extra_float_digits = 3", "SET bytea_output = hex",
      "SET IntervalStyle = iso_8601"};
  if (!opts.writable) {
    gucs.insert(gucs.begin(), "SET default_transaction_read_only = on");
  }
  for (const auto guc : gucs) {
    PGresult *r = PQexec(conn, std::string{guc}.c_str());
    if (r == nullptr || PQresultStatus(r) != PGRES_COMMAND_OK) {
      PQclear(r);
      PQfinish(conn);
      return std::unexpected("GUC pinning failed");
    }
    PQclear(r);
  }
  return std::unique_ptr<SqlBackend>{new PgBackend(conn, opts.writable)};
}

} // namespace zigzag

#else // !GLEDITOR_HAVE_PG -- the refusing stub (spec 7.3: always parse)

namespace zigzag {

std::expected<std::unique_ptr<SqlBackend>, std::string>
openPgBackend(const std::string_view /*conninfo*/,
              const PgOpenOptions & /*opts*/) {
  return std::unexpected(
      "sql:pg: this build has no libpq -- rebuild with "
      "GLEDITOR_SQL_PG=1 (the Makefile probes pkg-config libpq)");
}

} // namespace zigzag

#endif // GLEDITOR_HAVE_PG
