/**
 * @file sql_lens.cpp
 * @brief SqlLens browse mode (design/sql-to-zigzag.md §3-§5): schema
 * projection, generated keyset/ordinal windows, sightings vs subjects, and
 * the pk:/rid:/h: key ladder.
 */
#include "common/xanadu/zigzag/sql_lens.hpp"

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

#include "common/xanadu/zigzag/sql_keys.hpp"
#include "common/xanadu/zigzag/sqlite_backend.hpp"

namespace zigzag {

namespace {

constexpr std::string_view kBackendSqlite = "sqlite";

std::string quotedIdent(std::string_view id) {
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

/// The sql-values in select order joined by U+0000 -- the rowhash input
/// (spec §4.1; column names excluded, qhash scopes the keyspace).
std::string rowHashInput(std::span<const SqlValue> values) {
  std::string out;
  for (const auto &v : values) {
    out += encodeSqlValue(v);
    out.push_back('\0');
  }
  return out;
}

const char *keyStab(SqlKeyClass cls) {
  switch (cls) {
  case SqlKeyClass::Pk:
    return "pk";
  case SqlKeyClass::Rid:
    return "rid";
  case SqlKeyClass::Hash:
    return "hash";
  }
  return "hash";
}

/// NodeContent for a column value: typed scalars stay typed (R6), a NULL is
/// monostate + d.null (§3.4), a blob renders size-only (§3.5).
NodeContent valueContent(const SqlValue &v) {
  return std::visit(
      [](const auto &x) -> NodeContent {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return std::monostate{};
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return x;
        } else if constexpr (std::is_same_v<T, double>) {
          return x;
        } else if constexpr (std::is_same_v<T, std::string>) {
          return x;
        } else {
          return displaySqlValue(SqlValue{x});
        }
      },
      v);
}

} // namespace

SqlLens::SqlLens(std::unique_ptr<SqlBackend> backend, std::string dbid,
                 std::string backendName, std::string displayPath,
                 std::size_t pageSize)
    : backend_(std::move(backend)), dbid_(std::move(dbid)),
      backendName_(std::move(backendName)),
      displayPath_(std::move(displayPath)), pageSize_(pageSize) {
  if (auto s = backend_->schema(); s) {
    schema_ = std::move(*s);
  }
  produced_.emplace(connKey(), 0);
  for (const auto &t : schema_) {
    const auto tk = "t:" + sqlKeyEncode(t.name);
    produced_.emplace("sql:" + backendName_ + ":" + dbid_ + ":" + tk, 0);
    for (const auto &c : t.columns) {
      produced_.emplace("sql:" + backendName_ + ":" + dbid_ + ":" + tk +
                            ":c:" + sqlKeyEncode(c.name),
                        0);
    }
  }
}

ExternalKey SqlLens::connKey() const {
  return "sql:" + backendName_ + ":" + dbid_;
}

ExternalKey SqlLens::sightKey(const SqlPage &page, std::size_t ordinal) const {
  return page.key + ":@:o:" + std::to_string(ordinal);
}

ExternalKey SqlLens::valueKeyFor(const ExternalKey &rowKey,
                                 std::string_view colName) const {
  return rowKey + ":v:" + sqlKeyEncode(colName);
}

ExternalKey SqlLens::rowKeyFor(const SqlQuery &q,
                               const std::vector<SqlValue> &row) const {
  const std::string prefix =
      "sql:" + backendName_ + ":" + dbid_ + ":" + q.rowScope;
  switch (q.cls) {
  case SqlKeyClass::Pk: {
    std::vector<SqlValue> keys;
    keys.reserve(q.pkCols.size());
    for (const int i : q.pkCols) {
      keys.push_back(row[static_cast<std::size_t>(i)]);
    }
    return prefix + ":pk:" + encodeSqlValues(keys);
  }
  case SqlKeyClass::Rid:
    return prefix +
           ":rid:" + encodeSqlValue(row[static_cast<std::size_t>(q.ridCol)]);
  case SqlKeyClass::Hash:
    return prefix + ":h:" + sqlHashHex(rowHashInput(row));
  }
  return {};
}

// -- planning -----------------------------------------------------------------

std::expected<SqlQuery, std::string>
SqlLens::planBrowse(const SqlTable &t) const {
  SqlQuery q;
  q.stable               = t.name;
  q.tableKey             = "t:" + sqlKeyEncode(t.name);
  const std::string base = "sql:" + backendName_ + ":" + dbid_ + ":";

  // Visible columns in declared order.
  std::string select;
  for (const auto &c : t.columns) {
    if (!select.empty()) {
      select += ',';
    }
    select += quotedIdent(c.name);
    q.colNames.push_back(c.name);
    q.vnames.push_back(c.name);
    q.colTypes.push_back(c.declType);
    q.colKeys.push_back(base + q.tableKey + ":c:" + sqlKeyEncode(c.name));
  }

  // Key class: declared PK -> pk:; a rowid table -> rid:; else h: + ordinal.
  std::vector<int> pkOrder;
  {
    std::vector<std::pair<std::uint32_t, int>> keyed;
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
      if (t.columns[i].keyColumn) {
        keyed.emplace_back(t.columns[i].keyPosition, static_cast<int>(i));
      }
    }
    std::ranges::sort(keyed);
    for (const auto &[pos, i] : keyed) {
      pkOrder.push_back(i);
    }
  }
  q.pkCols = pkOrder;

  if (!t.isView && !pkOrder.empty()) {
    q.cls      = SqlKeyClass::Pk;
    q.sortCols = pkOrder;
    for (const int i : pkOrder) {
      q.sortNames.push_back(
          quotedIdent(q.colNames[static_cast<std::size_t>(i)]));
    }
  } else if (!t.isView && t.hasRowid) {
    q.cls = SqlKeyClass::Rid;
    // First unshadowed rowid name (table_xinfo already in t.columns).
    const char *cands[] = {"rowid", "oid", "_rowid_"};
    std::string ridName;
    for (const char *cand : cands) {
      const bool shadowed = std::ranges::any_of(
          q.colNames, [&](const std::string &n) { return n == cand; });
      if (!shadowed) {
        ridName = cand;
        break;
      }
    }
    if (ridName.empty()) {
      q.cls = SqlKeyClass::Hash; // all three names taken by real columns
    } else {
      select += "," + quotedIdent(ridName) + " AS __sql_rid";
      q.ridCol   = static_cast<int>(q.colNames.size());
      q.sortCols = {q.ridCol};
      // WHERE can't see select aliases -- use the rowid identifier itself.
      q.sortNames.push_back(quotedIdent(ridName));
    }
  } else {
    q.cls = SqlKeyClass::Hash;
  }

  if (q.cls == SqlKeyClass::Hash) {
    // Ordinal windows, whole-row order for determinism.
    q.ordinal = true;
    for (std::size_t i = 0; i < q.colNames.size(); ++i) {
      q.sortCols.push_back(static_cast<int>(i));
      q.sortNames.push_back(quotedIdent(q.colNames[i]));
    }
  }

  std::string orderBy;
  for (const auto &n : q.sortNames) {
    orderBy += (orderBy.empty() ? "" : ",") + n;
  }
  q.sqlSelect = "SELECT " + select + " FROM " + quotedIdent(t.name);
  q.sqlOrder =
      orderBy.empty() ? "" : " ORDER BY " + orderBy + " ASC NULLS LAST";
  q.sql   = q.sqlSelect + q.sqlOrder;
  q.qhash = sqlHashHex(q.sql);
  q.key   = base + "q:" + q.qhash;
  // Browse-scoped subjects unify across queries that see the same row
  // (spec section 4.4 cross-query unification).
  q.rowScope = q.tableKey;
  return q;
}

std::expected<SqlQuery, std::string>
SqlLens::planQuery(std::string_view sql, std::span<const SqlValue> params) {
  SqlQuery q;
  q.sql     = std::string(sql);
  q.ordinal = true;
  q.params.assign(params.begin(), params.end());
  // qhash = sha256(raw text || typed bound params) -- raw text, NOT a
  // normalized form: literal-vs-param spellings name different result
  // sets (spec section 4.1).
  q.qhash = sqlHashHex(q.sql + "\x1e" + encodeSqlValues(q.params));

  // Result columns come from the statement itself: prepare once purely to
  // describe it, then release (the one-live-cursor rule applies per fetch).
  auto cur = backend_->prepare(sql, params);
  if (!cur) {
    return std::unexpected(cur.error());
  }
  const auto cols = (*cur)->columns();
  cur->reset();
  const std::string base = "sql:" + backendName_ + ":" + dbid_ + ":";
  q.key                  = base + "q:" + q.qhash;
  q.rowScope             = "q:" + q.qhash;
  for (std::size_t i = 0; i < cols.size(); ++i) {
    const auto &c = cols[i];
    q.colNames.push_back(c.name);
    q.colTypes.push_back(c.declType);
    q.colKeys.push_back(q.key + ":c:" + std::to_string(i));
    // v: names the field: a duplicate result name collides in (subject,
    // name) keyspace, so later occurrences key positionally (spec 4.1).
    const auto seen = static_cast<std::size_t>(
        std::ranges::count(q.colNames.begin(), q.colNames.end() - 1, c.name));
    q.vnames.push_back(seen == 0 ? c.name : "@" + std::to_string(i));
  }
  if (q.colNames.empty()) {
    return std::unexpected("statement returns no columns");
  }
  // pk: eligibility needs a single attributable base table whose declared
  // key is fully selected; the metadata that proves that
  // (origin_name/table_name) is unavailable in this build, so query-mode
  // rows fall to h: scoped to the query (spec section 4.3's degradation).
  q.cls = SqlKeyClass::Hash;
  // Kinds the ordinal replay can't wrap (EXPLAIN, PRAGMA) are refused by
  // the first fetch: replayWindow's own kind check answers honestly.
  return q;
}

// -- fetching -----------------------------------------------------------------

std::expected<void, std::string> SqlLens::fetchWindow(SqlQuery &q,
                                                      SqlPage &page) {
  std::expected<std::unique_ptr<SqlCursor>, std::string> cur;
  if (q.ordinal) {
    const auto startOff = page.offset;
    cur =
        backend_->replayWindow(q.sql, q.params, {}, {}, page.offset, pageSize_);
    if (!cur) {
      return std::unexpected(cur.error());
    }
    page.winlo = startOff == 0 ? "" : "o:" + std::to_string(startOff);
  } else {
    // Keyset: OR-of-prefix-equalities over the bound (ASC NULLS LAST --
    // a NULL bound means nothing follows, so that clause is FALSE). The
    // WHERE goes between SELECT..FROM and ORDER BY.
    std::string sql = q.sqlSelect;
    std::vector<SqlValue> params;
    if (!page.bound.empty()) {
      std::string pred;
      const auto k = q.sortCols.size();
      for (std::size_t i = 0; i < k; ++i) {
        std::string clause;
        for (std::size_t j = 0; j < i; ++j) {
          clause += (clause.empty() ? "(" : " AND ");
          clause += q.sortNames[j] + " IS ?";
          params.push_back(page.bound[j]);
        }
        const bool boundNull =
            std::holds_alternative<std::monostate>(page.bound[i]);
        if (clause.empty()) {
          clause = "(";
        } else {
          clause += " AND ";
        }
        clause += boundNull ? "0"
                            : "(" + q.sortNames[i] + " > ? OR " +
                                  q.sortNames[i] + " IS NULL)";
        clause += ")";
        if (!boundNull) {
          params.push_back(page.bound[i]);
        }
        pred += (pred.empty() ? "" : " OR ") + clause;
      }
      sql += " WHERE " + pred;
    }
    sql += q.sqlOrder;
    sql += " LIMIT " + std::to_string(pageSize_);
    cur = backend_->prepare(sql, params);
  }
  if (!cur) {
    return std::unexpected(cur.error());
  }
  page.rows.clear();
  for (;;) {
    auto batch = (*cur)->fetchNext(pageSize_);
    if (!batch) {
      return std::unexpected(batch.error());
    }
    if (batch->empty()) {
      break;
    }
    for (auto &r : *batch) {
      page.rows.push_back(std::move(r));
    }
  }
  page.stub    = false;
  page.mintSeq = ++seqCounter_;
  page.rowKeys.clear();
  page.rowKeys.reserve(page.rows.size());
  for (const auto &r : page.rows) {
    const auto rk = rowKeyFor(q, r);
    page.rowKeys.push_back(rk);
    if (!subjects_.contains(rk)) {
      SqlSubject s;
      s.key     = rk;
      s.cls     = q.cls;
      s.display = displaySqlValues(r);
      s.values  = r;
      subjects_.emplace(rk, std::move(s));
      produced_.emplace(rk, page.mintSeq);
      for (const auto &vn : q.vnames) {
        produced_.emplace(valueKeyFor(rk, vn), page.mintSeq);
      }
    } else {
      // Re-sighted (new epoch after a discard, or a second window): the
      // subject keeps its identity but shows the freshly fetched values.
      auto &s   = subjects_.find(rk)->second;
      s.display = displaySqlValues(r);
      s.values  = r;
    }
    auto &sn    = subjects_.find(rk)->second;
    sn.colNames = q.colNames;
    sn.vnames   = q.vnames;
    subjects_.find(rk)->second.sightings.push_back(
        sightKey(page, page.rowKeys.size() - 1));
  }
  registerPageKeys(q, page);
  // Continuation bound: the last row's sort key (keyset only); ordinal
  // pages advance the fetch offset instead.
  if (q.ordinal) {
    page.offset += page.rows.size();
    page.winhi = page.rows.empty() ? "" : "o:" + std::to_string(page.offset);
  } else if (!page.rows.empty()) {
    const auto &last = page.rows.back();
    std::vector<SqlValue> bound;
    for (const int i : q.sortCols) {
      bound.push_back(last[static_cast<std::size_t>(i)]);
    }
    page.winhi = "k:" + encodeSqlValues(bound);
  }
  return {};
}

void SqlLens::appendStub(SqlQuery &q) {
  const auto full = [&q, this] {
    return !q.pages.empty() && !q.pages.back().stub &&
           q.pages.back().rows.size() == pageSize_;
  };
  if (!full()) {
    return;
  }
  const SqlPage &last = q.pages.back();
  SqlPage stub;
  stub.stub    = true;
  stub.mintSeq = last.mintSeq; // the stub rides its page's epoch
  if (q.ordinal) {
    stub.winid  = "o:" + std::to_string(last.offset);
    stub.offset = last.offset;
  } else {
    const auto &backRow = last.rows.back();
    for (const int i : q.sortCols) {
      stub.bound.push_back(backRow[static_cast<std::size_t>(i)]);
    }
    stub.winid = "k:" + encodeSqlValues(stub.bound);
    stub.winlo = stub.winid;
  }
  stub.key = q.key + ":p:" + stub.winid;
  produced_.emplace(stub.key, stub.mintSeq);
  q.pages.push_back(std::move(stub));
}

void SqlLens::registerPageKeys(SqlQuery & /*q*/, const SqlPage &page) {
  produced_[page.key] = page.mintSeq;
  for (std::size_t i = 0; i < page.rowKeys.size(); ++i) {
    produced_[sightKey(page, i)] = page.mintSeq;
  }
}

// -- app API ------------------------------------------------------------------

std::expected<void, std::string> SqlLens::browse(std::string_view tableName) {
  const auto it = std::ranges::find_if(
      schema_, [&](const SqlTable &t) { return t.name == tableName; });
  if (it == schema_.end()) {
    return std::unexpected("no such table or view: " + std::string(tableName));
  }
  auto q = planBrowse(*it);
  if (!q) {
    return std::unexpected(q.error());
  }
  q->mintSeq = ++seqCounter_;
  produced_.emplace(q->key, q->mintSeq);
  SqlPage first;
  first.stub  = true;
  first.winid = q->ordinal ? "o:0" : "k:start";
  first.key   = q->key + ":p:" + first.winid;
  q->pages.push_back(std::move(first));
  queries_.push_back(std::move(*q));
  auto &back = queries_.back();
  if (auto r = fetchWindow(back, back.pages.back()); !r) {
    return r;
  }
  appendStub(back);
  return {};
}

std::expected<void, std::string>
SqlLens::query(std::string_view sql, std::span<const SqlValue> params) {
  auto q = planQuery(sql, params);
  if (!q) {
    return std::unexpected(q.error());
  }
  q->mintSeq = ++seqCounter_;
  produced_.emplace(q->key, q->mintSeq);
  // Result-column cells are minted with the query.
  for (const auto &ck : q->colKeys) {
    produced_.emplace(ck, q->mintSeq);
  }
  SqlPage first;
  first.stub  = true;
  first.winid = "o:0";
  first.key   = q->key + ":p:" + first.winid;
  q->pages.push_back(std::move(first));
  queries_.push_back(std::move(*q));
  auto &back = queries_.back();
  if (auto r = fetchWindow(back, back.pages.back()); !r) {
    return r;
  }
  appendStub(back);
  return {};
}

std::expected<void, std::string>
SqlLens::materializePage(const ExternalKey &windowKey) {
  for (auto &q : queries_) {
    for (auto &p : q.pages) {
      if (p.key != windowKey || !p.stub) {
        continue;
      }
      if (auto r = fetchWindow(q, p); !r) {
        return r;
      }
      appendStub(q);
      return {};
    }
  }
  return std::unexpected("no deferred page with key: " + windowKey);
}

void SqlLens::discardMaterializedAfter(const ExternalKey &boundaryKey) {
  const auto it = produced_.find(boundaryKey);
  if (it == produced_.end() || it->second == 0) {
    return; // baseline schema keys can't bound a toss; foreign keys aren't ours
  }
  const std::size_t boundary = it->second;
  // Drop whole queries minted at/after the boundary.
  std::erase_if(queries_,
                [&](const SqlQuery &q) { return q.mintSeq >= boundary; });
  // Forget every produced key at/after the boundary, then drop the pages
  // that minted them and prune dangling sighting refs.
  for (auto i = produced_.begin(); i != produced_.end();) {
    i = (i->second >= boundary) ? produced_.erase(i) : std::next(i);
  }
  for (auto &q : queries_) {
    std::erase_if(q.pages,
                  [&](const SqlPage &p) { return p.mintSeq >= boundary; });
    if (q.pages.empty()) {
      // The query survived but its first window didn't: the page-0 stub
      // must return so a materialize can re-fetch it.
      SqlPage first;
      first.winid = q.ordinal ? "o:0" : "k:start";
      first.key   = q.key + ":p:" + first.winid;
      q.pages.push_back(std::move(first));
    } else {
      appendStub(q);
    }
  }
  for (auto &[k, s] : subjects_) {
    std::erase_if(s.sightings, [&](const ExternalKey &sk) {
      return !produced_.contains(sk);
    });
  }
}

// -- the projection
// ------------------------------------------------------------

std::vector<std::pair<std::string, std::optional<GenericDim>>>
SqlLens::dimensions() const {
  return {
      {"d.contains", GenericDim::Containment},
      {"d.seq", GenericDim::Sequence},
      {"d.ref", GenericDim::Reference},
      {"d.clone", GenericDim::Clone},
      {"d.role", std::nullopt},
      {"d.extkey", std::nullopt},
      {"d.subject", std::nullopt},
      {"d.conforms", std::nullopt},
      {"d.ofcol", std::nullopt},
      {"d.colof", std::nullopt},
      {"d.subjects", std::nullopt},
      {"d.column", std::nullopt},
      {"d.type", std::nullopt},
      {"d.pk", std::nullopt},
      {"d.notnull", std::nullopt},
      {"d.null", std::nullopt},
      {"d.ordinal", std::nullopt},
      {"d.keystab", std::nullopt},
      {"d.qhash", std::nullopt},
      {"d.params", std::nullopt},
      {"d.winlo", std::nullopt},
      {"d.winhi", std::nullopt},
      {"d.wincount", std::nullopt},
      {"d.snapscope", std::nullopt},
      {"d.state", std::nullopt},
  };
}

void SqlLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  emit({.key        = connKey(),
        .content    = backendName_ + ":" + displayPath_,
        .attributes = {{"d.role", "sql.conn"}}});
  emitSchema(emit);
  for (const auto &q : queries_) {
    emitQuery(q, emit);
  }
}

void SqlLens::emitSchema(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  const std::string base = "sql:" + backendName_ + ":" + dbid_ + ":";
  for (const auto &t : schema_) {
    const auto tk = "t:" + sqlKeyEncode(t.name);
    emit({.key        = base + tk,
          .content    = t.name,
          .attributes = {{"d.role", "sql.table"}}});
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
      const auto &c                                          = t.columns[i];
      std::vector<std::pair<std::string, std::string>> attrs = {
          {"d.role", "sql.col"},
          {"d.type", c.declType.empty() ? "ANY" : c.declType},
          {"d.ordinal", std::to_string(i)},
      };
      if (c.keyColumn) {
        attrs.emplace_back("d.pk", std::to_string(c.keyPosition));
      }
      emit({.key        = base + tk + ":c:" + sqlKeyEncode(c.name),
            .content    = c.name,
            .attributes = std::move(attrs)});
    }
  }
}

void SqlLens::emitQuery(
    const SqlQuery &q,
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  {
    std::vector<std::pair<std::string, std::string>> qattrs = {
        {"d.role", "sql.query"},
        {"d.qhash", q.qhash},
    };
    if (!q.params.empty()) {
      qattrs.emplace_back("d.params", encodeSqlValues(q.params));
    }
    emit({.key = q.key, .content = q.sql, .attributes = qattrs});
  }
  // Result-column cells (query mode: q:<qhash>:c:<n>; browse's colKeys are
  // the schema columns, already emitted by emitSchema).
  for (std::size_t i = 0; i < q.colKeys.size(); ++i) {
    if (q.colKeys[i].rfind(q.key + ":", 0) != 0) {
      continue;
    }
    emit({.key        = q.colKeys[i],
          .content    = q.colNames[i],
          .attributes = {
              {"d.role", "sql.col"},
              {"d.column", q.colNames[i]},
              {"d.type", q.colTypes[i].empty() ? "ANY" : q.colTypes[i]}}});
  }
  std::unordered_set<ExternalKey> emittedSubjects;
  for (const auto &p : q.pages) {
    std::vector<std::pair<std::string, std::string>> pattrs = {
        {"d.role", "sql.page"},
        {"d.state", p.stub ? "deferred" : "live"},
        {"d.wincount", std::to_string(p.rows.size())},
        {"d.snapscope", "none"},
    };
    if (!p.winlo.empty()) {
      pattrs.emplace_back("d.winlo", p.winlo);
    }
    if (!p.winhi.empty()) {
      pattrs.emplace_back("d.winhi", p.winhi);
    }
    emit({.key = p.key, .content = p.winid, .attributes = pattrs});
    for (std::size_t i = 0; i < p.rowKeys.size(); ++i) {
      const auto &rk  = p.rowKeys[i];
      const auto sk   = sightKey(p, i);
      const auto sit  = subjects_.find(rk);
      const auto disp = sit != subjects_.end() ? sit->second.display : rk;
      emit({.key        = sk,
            .content    = disp,
            .attributes = {{"d.role", "sql.sight"},
                           {"d.ordinal", std::to_string(i)}}});
      if (sit != subjects_.end() && emittedSubjects.insert(rk).second) {
        const SqlSubject &s = sit->second;
        emit({.key        = s.key,
              .content    = s.display,
              .attributes = {{"d.role", "sql.row"},
                             {"d.extkey", s.key},
                             {"d.keystab", keyStab(s.cls)}}});
        for (std::size_t c = 0; c < s.colNames.size() && c < s.values.size();
             ++c) {
          const auto &v                                           = s.values[c];
          std::vector<std::pair<std::string, std::string>> vattrs = {
              {"d.role", "sql.val"},
              {"d.column", s.colNames[c]},
              {"d.type", c < q.colTypes.size()
                             ? (q.colTypes[c].empty() ? "ANY" : q.colTypes[c])
                             : "ANY"},
          };
          if (std::holds_alternative<std::monostate>(v)) {
            vattrs.emplace_back("d.null", "1");
          }
          emit({.key        = valueKeyFor(rk, s.vnames[c]),
                .content    = valueContent(v),
                .attributes = std::move(vattrs)});
        }
      }
    }
  }
}

void SqlLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  const std::string base = "sql:" + backendName_ + ":" + dbid_ + ":";

  // conn -> firstborn; tables then queries on the shared d.seq rank.
  std::vector<ExternalKey> children;
  for (const auto &t : schema_) {
    children.push_back(base + "t:" + sqlKeyEncode(t.name));
  }
  for (const auto &q : queries_) {
    children.push_back(q.key);
  }
  for (std::size_t i = 0; i < children.size(); ++i) {
    if (i == 0) {
      emit({.from      = connKey(),
            .to        = children[0],
            .dimension = std::string{genericDimName(GenericDim::Containment)}});
    } else {
      emit({.from      = children[i - 1],
            .to        = children[i],
            .dimension = std::string{genericDimName(GenericDim::Sequence)}});
    }
  }
  // table -> col0 contains; cols d.seq.
  for (const auto &t : schema_) {
    const auto tk = "t:" + sqlKeyEncode(t.name);
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
      const auto ck = base + tk + ":c:" + sqlKeyEncode(t.columns[i].name);
      if (i == 0) {
        emit({.from = base + tk,
              .to   = ck,
              .dimension =
                  std::string{genericDimName(GenericDim::Containment)}});
      } else {
        const auto prev =
            base + tk + ":c:" + sqlKeyEncode(t.columns[i - 1].name);
        emit({.from      = prev,
              .to        = ck,
              .dimension = std::string{genericDimName(GenericDim::Sequence)}});
      }
    }
  }
  // query -> page0 contains; pages d.seq. page -> sight0; sights d.seq.
  for (const auto &q : queries_) {
    for (std::size_t i = 0; i < q.pages.size(); ++i) {
      if (i == 0) {
        emit({.from = q.key,
              .to   = q.pages[0].key,
              .dimension =
                  std::string{genericDimName(GenericDim::Containment)}});
      } else {
        emit({.from      = q.pages[i - 1].key,
              .to        = q.pages[i].key,
              .dimension = std::string{genericDimName(GenericDim::Sequence)}});
      }
      const auto &p = q.pages[i];
      for (std::size_t s = 0; s < p.rowKeys.size(); ++s) {
        const auto sk = sightKey(p, s);
        if (s == 0) {
          emit({.from = p.key,
                .to   = sk,
                .dimension =
                    std::string{genericDimName(GenericDim::Containment)}});
        } else {
          emit(
              {.from      = sightKey(p, s - 1),
               .to        = sk,
               .dimension = std::string{genericDimName(GenericDim::Sequence)}});
        }
      }
    }
  }
  // Membership ranks (posward = toward the one), each member set sorted for
  // determinism. Members chain toward the head; the last member +dim-> head.
  auto emitRank = [&](std::vector<ExternalKey> members, const ExternalKey &head,
                      std::string_view dim) {
    if (members.empty()) {
      return;
    }
    members.push_back(head);
    for (std::size_t i = 0; i + 1 < members.size(); ++i) {
      emit({.from      = members[i],
            .to        = members[i + 1],
            .dimension = std::string{dim}});
    }
  };
  // sightings -> subject (d.subject), in each subject's sight order
  // (sightings append at fetch time; forEachEdge stays read-only).
  {
    std::vector<ExternalKey> subjKeys;
    for (const auto &[k, s] : subjects_) {
      if (!s.sightings.empty()) {
        subjKeys.push_back(k);
      }
    }
    std::ranges::sort(subjKeys);
    for (const auto &k : subjKeys) {
      emitRank(subjects_.find(k)->second.sightings, k, "d.subject");
    }
    // subjects -> conn (d.subjects): ever-seen index.
    emitRank(subjKeys, connKey(), "d.subjects");
  }
  // row -> first value contains; the row's values chain on d.seq, in the
  // query's visible-column order. Emitted per live subject, key-sorted so
  // the chain is stable across refreshes.
  {
    std::vector<ExternalKey> live;
    for (const auto &[k, s] : subjects_) {
      if (!s.sightings.empty()) {
        live.push_back(k);
      }
    }
    std::ranges::sort(live);
    for (const auto &rk : live) {
      const auto &s = subjects_.find(rk)->second;
      std::vector<ExternalKey> vks;
      vks.reserve(s.vnames.size());
      for (const auto &vn : s.vnames) {
        vks.push_back(valueKeyFor(rk, vn));
      }
      for (std::size_t i = 0; i < vks.size(); ++i) {
        if (i == 0) {
          emit({.from = rk,
                .to   = vks[0],
                .dimension =
                    std::string{genericDimName(GenericDim::Containment)}});
        } else {
          emit(
              {.from      = vks[i - 1],
               .to        = vks[i],
               .dimension = std::string{genericDimName(GenericDim::Sequence)}});
        }
      }
    }
  }
  // rows -> table (d.conforms) and values -> schema col (d.ofcol): key-sorted.
  for (const auto &q : queries_) {
    std::vector<ExternalKey> rows;
    std::unordered_map<std::string, std::vector<ExternalKey>> byCol;
    for (const auto &p : q.pages) {
      for (const auto &rk : p.rowKeys) {
        if (subjects_.contains(rk)) {
          rows.push_back(rk);
        }
      }
    }
    std::ranges::sort(rows);
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    // d.conforms heads at the schema table; a query-mode result conforms
    // to no single base table, so the rank is absent there.
    if (!q.tableKey.empty()) {
      emitRank(rows, base + q.tableKey, "d.conforms");
    }
    // The query's result columns belong to it on d.colof.
    emitRank(q.colKeys, q.key, "d.colof");
    for (const auto &rk : rows) {
      for (std::size_t c = 0; c < q.colNames.size(); ++c) {
        byCol[q.colKeys[c]].push_back(valueKeyFor(rk, q.vnames[c]));
      }
    }
    for (auto &[ck, vks] : byCol) {
      std::ranges::sort(vks);
      emitRank(vks, ck, "d.ofcol");
    }
  }
}

std::vector<std::pair<std::string, std::string>>
SqlLens::staleAttributes(const ExternalKey & /*key*/) const {
  return {{"d.state", "stale"}};
}

bool SqlLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey());
  return true;
}

// -- factory ------------------------------------------------------------------

std::expected<std::unique_ptr<SqlLens>, std::string>
openSqliteLens(std::string_view path, std::size_t pageSize) {
  auto be = openSqliteBackend(path);
  if (!be) {
    return std::unexpected(be.error());
  }
  return std::unique_ptr<SqlLens>(new SqlLens(std::move(*be), dbidForPath(path),
                                              std::string{kBackendSqlite},
                                              std::string{path}, pageSize));
}

std::expected<std::unique_ptr<SqlLens>, std::string>
openSqlUri(std::string_view uri, std::size_t pageSize) {
  constexpr std::string_view prefix = "sql:";
  if (uri.rfind(prefix, 0) != 0) {
    // A bare path is sql:sqlite:/// shorthand.
    return openSqliteLens(uri, pageSize);
  }
  const auto rest = uri.substr(prefix.size());
  const auto sep  = rest.find(':');
  if (sep == std::string_view::npos) {
    return std::unexpected("sql: URI needs a backend: " + std::string(uri));
  }
  const auto backend = rest.substr(0, sep);
  if (backend == "sqlite") {
    auto path = rest.substr(sep + 1);
    // sql:sqlite:///abs/path -- strip the URI authority slashes.
    while (path.starts_with('/')) {
      path.remove_prefix(1);
    }
    return openSqliteLens("/" + std::string(path), pageSize);
  }
  if (backend == "pg") {
    return std::unexpected(
        "sql:pg:// is reserved; the libpq backend lands in a later phase");
  }
  return std::unexpected("unknown sql: backend: " + std::string(backend));
}

} // namespace zigzag
