/**
 * @file sql_lens.hpp
 * @brief SqlLens: the windowed SQL lens (design/sql-to-zigzag.md).
 *
 * Phase-3 scope: browse mode over one table -- the schema projection
 * (conn -> sql.table -> sql.col), generated keyset/ordinal window queries,
 * sql.page / sql.sight / sql.row / sql.val cells, the pk:/rid:/h: key ladder,
 * and the stub horizon behind materializePage(). Query mode (arbitrary SQL)
 * and promotion arrive in phases 4 and 5.
 */
#ifndef COMMON_XANADU_ZIGZAG_SQL_LENS_HPP
#define COMMON_XANADU_ZIGZAG_SQL_LENS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"
#include "common/xanadu/zigzag/sql_backend.hpp"

namespace zigzag {

class ArenaManifold;

/// Which rung of the identity ladder a result's rows sit on (spec §4.2).
/// ord: is reserved, never emitted in round one.
enum class SqlKeyClass : std::uint8_t { Pk, Rid, Hash };

/// A row subject: identity + the union of observed column values. Rows float
/// outside page structure; sightings carry the position (spec §3.7).
struct SqlSubject {
  ExternalKey key;      ///< t:/q:-scoped row subject key
  ExternalKey queryKey; ///< the producing query's q:<qhash>/t:<stable>
  SqlKeyClass cls;
  std::string display;               ///< display key (cell content)
  std::vector<SqlValue> values;      ///< select-order values (visible cols)
  std::vector<std::string> colNames; ///< the query's visible columns
  std::vector<std::string> colTypes; ///< declared types, select order
  /// v: key suffixes matching colNames (from the producing query).
  std::vector<std::string> vnames;
  std::vector<ExternalKey> sightings; ///< d.subject rank order (mint order)
};

/// One window of a query, materialized or deferred (stub). A stub's key IS
/// the page it becomes: materializePage(stubKey) restates it live in place.
struct SqlPage {
  ExternalKey key;   ///< q:<qh>:p:<winid>
  std::string winid; ///< k:start | k:<bound> | o:<offset>
  bool stub{true};
  std::size_t mintSeq{0}; ///< materialization epoch (rewind boundary)
  std::string winlo;      ///< exclusive fetch bound, typed encoding
  std::string winhi;      ///< continuation bound ("" = unknown/none)
  /// The typed bound this window fetches after (keyset only; empty for
  /// k:start and ordinal pages).
  std::vector<SqlValue> bound;
  std::size_t offset{0};                   ///< ordinal pages
  std::vector<std::vector<SqlValue>> rows; ///< empty on a stub
  std::vector<ExternalKey> rowKeys;        ///< subject per row
};

/// One browse result: generated SQL + paging state.
struct SqlQuery {
  ExternalKey key; ///< q:<qhash>
  std::string qhash;
  std::string sql; ///< display + qhash input
  /// SELECT .. FROM part; window SQL inserts WHERE between it and sqlOrder.
  std::string sqlSelect;
  std::string sqlOrder; ///< "ORDER BY .." or ""
  std::string stable;   ///< canonical table name
  std::string tableKey; ///< t:<stable> ("" in query mode)
  /// The subject-key scope: t:<stable> for browse, q:<qhash> for query.
  std::string rowScope;
  /// Bound parameters of a query-mode statement; feeds qhash + replay.
  std::vector<SqlValue> params;
  SqlKeyClass cls{SqlKeyClass::Hash};
  bool ordinal{false};
  std::vector<std::string> colNames; ///< visible cols, select order
  /// v: key suffix per visible col: the name, or "@<n>" after a duplicate
  /// within this result (spec section 4.1's collision rule).
  std::vector<std::string> vnames;
  std::vector<std::string> colTypes;  ///< declType per visible col
  std::vector<std::string> colKeys;   ///< schema-col keys, select order
  std::vector<int> pkCols;            ///< result-col idx, PK order
  int ridCol{-1};                     ///< result-col idx of __sql_rid
  std::vector<int> sortCols;          ///< result-col idx of sort key
  std::vector<std::string> sortNames; ///< ORDER BY column expressions
  std::size_t mintSeq{0};             ///< the epoch browse() created it in
  std::vector<SqlPage> pages;
};

/// The lens. The session owns it after construction; the app keeps a
/// non-owning pointer to drive browse()/materializePage() (spec §5.2).
class SqlLens final : public ProjectionLens, public WindowedLens {
public:
  SqlLens(std::unique_ptr<SqlBackend> backend, std::string dbid,
          std::string backendName, std::string displayPath,
          std::size_t pageSize = 64);

  // -- app-driven entry points ------------------------------------------------

  /// Start a browse of a schema table: emits a query node and materializes
  /// page 0 (keyset on a sortable table, ordinal on a view). The session's
  /// next refresh() projects the new materialization.
  [[nodiscard]] std::expected<void, std::string>
  browse(std::string_view tableName);

  /// Query mode (spec section 5.1): run arbitrary read-only SQL as an
  /// ordinal-paged query. Rows are hashed subjects under q:<qhash> (pk
  /// attribution needs SQLITE_ENABLE_COLUMN_METADATA, absent here), and
  /// windows replay via SELECT * FROM (<sql>) LIMIT/OFFSET.
  [[nodiscard]] std::expected<void, std::string>
  query(std::string_view sql, std::span<const SqlValue> params);

  // -- promotion (spec section 6)
  // -------------------------------------------------

  /// Value-cell writeback (the spec's write phase): a `<row>:v:<col>`
  /// key -> `UPDATE <table> SET <col>=? WHERE <pk-address>` on
  /// pk/rid-addressed rows. Row cells, h:-keyed rows, and q:-scoped
  /// rows refuse.
  [[nodiscard]] std::expected<ExternalKey, std::string>
  applyContentChange(const ExternalKey &node,
                     std::string_view newContent) override;

  /// The ranks a promote envelope writes -- declare these in the
  /// annotation store before the staging overlay is read, or the
  /// envelope's dims land as anonymous cells (spec section 6.1).
  static constexpr std::array<std::string_view, 9> envelopeDims = {
      "d.contains", "d.seq",  "d.role", "d.extkey",   "d.keystab",
      "d.column",   "d.type", "d.null", "d.promoted",
  };

  /// Mint a disconnected sql.annot envelope in @p staging -- copies of the
  /// subject (or every subject of the page) @p key names, content-level,
  /// never refs (spec section 6.1's cross-arena rule). The caller
  /// sequences the gesture and owns the d.promoted membership link.
  [[nodiscard]] std::expected<CellRef, std::string>
  buildEnvelope(ArenaManifold &staging, const ExternalKey &key) const;

  // -- WindowedLens
  // ------------------------------------------------------------

  /// Materialize the deferred page (stub) @p windowKey names -- fetch its
  /// rows and append the next stub if the window filled.
  [[nodiscard]] std::expected<void, std::string>
  materializePage(const ExternalKey &windowKey) override;

  /// The next deferred page's key (the horizon's first stub) -- the
  /// next-page gesture's target.
  [[nodiscard]] std::optional<ExternalKey> nextStub() const override;

  /// Drop every page materialized at or after the epoch @p boundaryKey was
  /// minted in -- the lens-side half of ProjectionSession::rewind (spec
  /// §5.5): no buffered window survives a toss it was inside.
  void discardMaterializedAfter(const ExternalKey &boundaryKey) override;

  // -- ProjectionLens
  // ----------------------------------------------------------

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;
  void forEachNode(
      gleditor::cpp26::function_ref<void(const SourceNode &)>) const override;
  void forEachEdge(
      gleditor::cpp26::function_ref<void(const SourceEdge &)>) const override;
  [[nodiscard]] std::vector<std::pair<std::string, std::string>>
  staleAttributes(const ExternalKey &key) const override;
  bool forEachRoot(
      gleditor::cpp26::function_ref<void(const ExternalKey &)>) const override;

  [[nodiscard]] const std::string &dbid() const noexcept { return dbid_; }
  [[nodiscard]] const SqlBackend &backend() const noexcept { return *backend_; }

private:
  ExternalKey connKey() const;
  ExternalKey sightKey(const SqlPage &page, std::size_t ordinal) const;
  ExternalKey rowKeyFor(const SqlQuery &q,
                        const std::vector<SqlValue> &row) const;
  ExternalKey valueKeyFor(const ExternalKey &rowKey,
                          std::string_view colName) const;

  /// Emit the conn cell + schema (tables, cols).
  void emitSchema(
      gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const;
  /// Emit one query subtree: query -> pages -> sights, and subjects/values on
  /// first sighting.
  void
  emitQuery(const SqlQuery &q,
            gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const;

  /// Generate the browse SQL + paging spec for a table (keyset or ordinal).
  std::expected<SqlQuery, std::string> planBrowse(const SqlTable &t) const;
  /// Plan a query-mode result: prepare for the column list, hash the raw
  /// text + params into qhash, scope rows under q:<qhash>.
  std::expected<SqlQuery, std::string>
  planQuery(std::string_view sql, std::span<const SqlValue> params);
  /// Fetch one window into @p page (keyset or ordinal per @p q).
  std::expected<void, std::string> fetchWindow(SqlQuery &q, SqlPage &page);
  /// Append the trailing stub after the last materialized page, if the
  /// horizon may continue.
  void appendStub(SqlQuery &q);
  /// Register every node key a page's materialization produces, for the
  /// rewind-boundary lookup.
  void registerPageKeys(SqlQuery &q, const SqlPage &page);

  std::unique_ptr<SqlBackend> backend_;
  std::string dbid_;
  std::string backendName_;
  std::string displayPath_;
  std::size_t pageSize_;
  std::vector<SqlTable> schema_;
  std::vector<SqlQuery> queries_;
  /// subject key -> subject. Ordered emission is key-sorted, not map order.
  std::unordered_map<ExternalKey, SqlSubject> subjects_;
  /// Every node key the lens can emit -> the materialization epoch that
  /// produced it (0 = baseline schema cells).
  std::unordered_map<ExternalKey, std::size_t> produced_;
  std::size_t seqCounter_{0};
};

/// Open a sqlite database read-only and wrap it in a SqlLens.
[[nodiscard]] std::expected<std::unique_ptr<SqlLens>, std::string>
openSqliteLens(std::string_view path, std::size_t pageSize = 64);

/// Open the user-facing lens URI (spec section 7.4):
///   sql:sqlite:///abs/path.db     -- sqlite (a bare path is shorthand)
///   sql:pg://host:port/db         -- recognized, backend lands in phase 7
[[nodiscard]] std::expected<std::unique_ptr<SqlLens>, std::string>
openSqlUri(std::string_view uri, std::size_t pageSize = 64);

} // namespace zigzag

#endif // COMMON_XANADU_ZIGZAG_SQL_LENS_HPP
