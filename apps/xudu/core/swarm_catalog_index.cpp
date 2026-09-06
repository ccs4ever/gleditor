/**
 * @file swarm_catalog_index.cpp
 * @brief Implementation of SQLite FTS5 search index for publications and
 * topics.
 */
#include "swarm_catalog_index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace xudu {

namespace {

std::string topicsToString(const std::vector<std::string> &topics) {
  std::ostringstream ss;
  for (std::size_t i = 0; i < topics.size(); ++i) {
    if (i > 0) {
      ss << ",";
    }
    ss << topics[i];
  }
  return ss.str();
}

std::vector<std::string> stringToTopics(std::string_view str) {
  std::vector<std::string> topics;
  std::size_t start = 0;
  while (start < str.size()) {
    const auto comma = str.find(',', start);
    const auto token = (comma == std::string_view::npos)
                           ? str.substr(start)
                           : str.substr(start, comma - start);
    // Trim whitespace
    const auto first = token.find_first_not_of(" \t");
    const auto last  = token.find_last_not_of(" \t");
    if (first != std::string_view::npos && last != std::string_view::npos) {
      topics.emplace_back(token.substr(first, last - first + 1));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return topics;
}

std::string sanitizeFtsToken(std::string_view token) {
  std::string out;
  out.reserve(token.size());
  for (const char c : token) {
    if (c == '"') {
      out.push_back('"'); // escape quote
      out.push_back('"');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

} // namespace

void SwarmCatalogIndex::parseXanadulogicalQuery(std::string_view query,
                                                std::string &ftsMatch,
                                                std::string &sqlFilter) {
  ftsMatch.clear();
  sqlFilter.clear();

  std::vector<std::string> ftsTerms;
  std::size_t i       = 0;
  const std::size_t n = query.size();

  while (i < n) {
    // Skip whitespace
    while (i < n && std::isspace(static_cast<unsigned char>(query[i]))) {
      ++i;
    }
    if (i >= n) {
      break;
    }

    // Check for quoted phrase
    if (query[i] == '"') {
      const auto endQuote = query.find('"', i + 1);
      if (endQuote != std::string_view::npos) {
        const auto phrase = query.substr(i + 1, endQuote - i - 1);
        ftsTerms.push_back("\"" + sanitizeFtsToken(phrase) + "\"");
        i = endQuote + 1;
        continue;
      }
    }

    // Find end of current token
    const auto start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(query[i]))) {
      ++i;
    }
    const auto token = query.substr(start, i - start);

    // Hashtag topic filter: #quantum-computing -> topics:quantum-computing
    if (token.starts_with('#') && token.size() > 1) {
      const auto tag = token.substr(1);
      ftsTerms.push_back("topics:\"" + sanitizeFtsToken(tag) + "\"");
      continue;
    }

    // Author filter: author:nelson -> authorName:nelson
    if (token.starts_with("author:") && token.size() > 7) {
      const auto author = token.substr(7);
      ftsTerms.push_back("authorName:\"" + sanitizeFtsToken(author) + "\"");
      continue;
    }

    // Topic filter: topic:hypertext -> topics:hypertext
    if (token.starts_with("topic:") && token.size() > 6) {
      const auto tag = token.substr(6);
      ftsTerms.push_back("topics:\"" + sanitizeFtsToken(tag) + "\"");
      continue;
    }

    // Provenance filter: is:verified -> isVerified = 1
    if (token == "is:verified") {
      sqlFilter += " AND m.isVerified = 1";
      continue;
    }

    // Transcopyright filter: has:transcopyright -> hasTranscopyright = 1
    if (token == "has:transcopyright") {
      sqlFilter += " AND m.hasTranscopyright = 1";
      continue;
    }

    // Quotes / Backlink filter: quotes:btpk:...
    if (token.starts_with("quotes:") && token.size() > 7) {
      const auto uri = token.substr(7);
      sqlFilter += " AND (m.bep46Uri LIKE '%" + sanitizeFtsToken(uri) +
                   "%' OR m.abstractText LIKE '%" + sanitizeFtsToken(uri) +
                   "%')";
      continue;
    }

    // Proximity NEAR operator: NEAR(term1 term2, dist)
    if (token.starts_with("NEAR(") || token.starts_with("near(")) {
      ftsTerms.emplace_back(token);
      continue;
    }

    // Boolean operators
    if (token == "AND" || token == "OR" || token == "NOT") {
      ftsTerms.emplace_back(token);
      continue;
    }

    // Standard bare word
    if (token.ends_with('*')) {
      ftsTerms.push_back(std::string(token.substr(0, token.size() - 1)) + "*");
    } else {
      ftsTerms.push_back("\"" + sanitizeFtsToken(token) + "\"");
    }
  }

  // Join FTS terms with AND if no explicit operator is present
  std::ostringstream ss;
  for (std::size_t idx = 0; idx < ftsTerms.size(); ++idx) {
    if (idx > 0 && ftsTerms[idx] != "AND" && ftsTerms[idx] != "OR" &&
        ftsTerms[idx] != "NOT" && ftsTerms[idx - 1] != "AND" &&
        ftsTerms[idx - 1] != "OR" && ftsTerms[idx - 1] != "NOT") {
      ss << " AND ";
    } else if (idx > 0) {
      ss << " ";
    }
    ss << ftsTerms[idx];
  }
  ftsMatch = ss.str();
}

struct SwarmCatalogIndex::Impl {
  sqlite3 *db{nullptr};

  explicit Impl(const std::string &dbPath) {
    const int rc = sqlite3_open_v2(dbPath.c_str(), &db,
                                   SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                       SQLITE_OPEN_NOMUTEX,
                                   nullptr);
    if (rc != SQLITE_OK) {
      const std::string msg = sqlite3_errmsg(db);
      sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error("Failed to open SQLite database: " + msg);
    }
    initSchema();
  }

  ~Impl() {
    if (db) {
      sqlite3_close(db);
    }
  }

  void initSchema() {
    static constexpr const char *kSchema = R"(
      CREATE VIRTUAL TABLE IF NOT EXISTS publications_fts USING fts5(
          infoHash UNINDEXED,
          title,
          authorName,
          topics,
          abstract,
          bep46Uri UNINDEXED,
          tokenize = 'porter unicode61'
      );

      CREATE TABLE IF NOT EXISTS publications_meta (
          infoHash TEXT PRIMARY KEY,
          title TEXT,
          authorName TEXT,
          authorFingerprint TEXT,
          topics TEXT,
          abstractText TEXT,
          bep46Uri TEXT,
          timestamp INTEGER,
          sequence INTEGER,
          totalBytes INTEGER,
          microversions INTEGER,
          isVerified INTEGER,
          hasTranscopyright INTEGER,
          transcopyrightTerms TEXT,
          merkleRoot TEXT,
          signature TEXT,
          seederCount INTEGER,
          peerCount INTEGER
      );
      CREATE INDEX IF NOT EXISTS idx_meta_seeders ON publications_meta(seederCount);
      CREATE INDEX IF NOT EXISTS idx_meta_time ON publications_meta(timestamp);
    )";

    char *errmsg = nullptr;
    if (sqlite3_exec(db, kSchema, nullptr, nullptr, &errmsg) != SQLITE_OK) {
      std::string err = errmsg ? errmsg : "unknown error";
      sqlite3_free(errmsg);
      throw std::runtime_error("Failed to init SwarmCatalogIndex schema: " +
                               err);
    }
  }
};

SwarmCatalogIndex::SwarmCatalogIndex() : SwarmCatalogIndex(":memory:") {}

SwarmCatalogIndex::SwarmCatalogIndex(const std::string &dbPath)
    : impl_(std::make_unique<Impl>(dbPath)) {}

SwarmCatalogIndex::~SwarmCatalogIndex()                             = default;
SwarmCatalogIndex::SwarmCatalogIndex(SwarmCatalogIndex &&) noexcept = default;
SwarmCatalogIndex &
SwarmCatalogIndex::operator=(SwarmCatalogIndex &&) noexcept = default;

void SwarmCatalogIndex::indexPublication(const PublicationEntry &entry,
                                         const int seederCount,
                                         const int peerCount,
                                         const bool isVerified) {
  if (!impl_ || !impl_->db || entry.infoHash.empty()) {
    return;
  }

  removePublication(entry.infoHash);

  const auto topicsStr = topicsToString(entry.topics);
  const auto rootHex   = toHex32(entry.merkleRoot);

  // Insert into FTS5
  static constexpr const char *kInsertFts =
      "INSERT INTO publications_fts(infoHash, title, authorName, topics, "
      "abstract, bep46Uri) VALUES(?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmtFts = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kInsertFts, -1, &stmtFts, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmtFts, 1, entry.infoHash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtFts, 2, entry.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtFts, 3, entry.authorName.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtFts, 4, topicsStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtFts, 5, entry.abstractText.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtFts, 6, entry.bep46Uri.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmtFts);
    sqlite3_finalize(stmtFts);
  }

  // Insert into Meta table
  static constexpr const char *kInsertMeta =
      "INSERT OR REPLACE INTO publications_meta(infoHash, title, authorName, "
      "authorFingerprint, topics, abstractText, bep46Uri, timestamp, sequence, "
      "totalBytes, microversions, isVerified, hasTranscopyright, "
      "transcopyrightTerms, merkleRoot, signature, seederCount, peerCount) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmtMeta = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kInsertMeta, -1, &stmtMeta, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmtMeta, 1, entry.infoHash.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 2, entry.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 3, entry.authorName.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 4, entry.authorFingerprint.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 5, topicsStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 6, entry.abstractText.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 7, entry.bep46Uri.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmtMeta, 8,
                       static_cast<sqlite3_int64>(entry.timestamp));
    sqlite3_bind_int64(stmtMeta, 9, static_cast<sqlite3_int64>(entry.sequence));
    sqlite3_bind_int64(stmtMeta, 10,
                       static_cast<sqlite3_int64>(entry.totalBytes));
    sqlite3_bind_int(stmtMeta, 11, static_cast<int>(entry.microversions));
    sqlite3_bind_int(stmtMeta, 12, isVerified ? 1 : 0);
    sqlite3_bind_int(stmtMeta, 13, entry.hasTranscopyright ? 1 : 0);
    sqlite3_bind_text(stmtMeta, 14, entry.transcopyrightTerms.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 15, rootHex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmtMeta, 16, entry.signature.c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMeta, 17, seederCount);
    sqlite3_bind_int(stmtMeta, 18, peerCount);
    sqlite3_step(stmtMeta);
    sqlite3_finalize(stmtMeta);
  }
}

void SwarmCatalogIndex::indexLedger(const PublicationLedger &ledger) {
  for (const auto &entry : ledger.entries()) {
    indexPublication(entry, 8, 3, true);
  }
}

void SwarmCatalogIndex::removePublication(std::string_view infoHash) {
  if (!impl_ || !impl_->db || infoHash.empty()) {
    return;
  }

  static constexpr const char *kDelFts =
      "DELETE FROM publications_fts WHERE infoHash = ?;";
  sqlite3_stmt *stmtFts = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kDelFts, -1, &stmtFts, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmtFts, 1, infoHash.data(),
                      static_cast<int>(infoHash.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmtFts);
    sqlite3_finalize(stmtFts);
  }

  static constexpr const char *kDelMeta =
      "DELETE FROM publications_meta WHERE infoHash = ?;";
  sqlite3_stmt *stmtMeta = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kDelMeta, -1, &stmtMeta, nullptr) ==
      SQLITE_OK) {
    sqlite3_bind_text(stmtMeta, 1, infoHash.data(),
                      static_cast<int>(infoHash.size()), SQLITE_TRANSIENT);
    sqlite3_step(stmtMeta);
    sqlite3_finalize(stmtMeta);
  }
}

std::vector<SearchResult>
SwarmCatalogIndex::search(std::string_view xqlQuery,
                          const std::size_t limit) const {
  std::vector<SearchResult> results;
  if (!impl_ || !impl_->db) {
    return results;
  }

  std::string ftsMatch;
  std::string sqlFilter;
  parseXanadulogicalQuery(xqlQuery, ftsMatch, sqlFilter);

  sqlite3_stmt *stmt = nullptr;
  if (ftsMatch.empty()) {
    // No full-text match constraint: query metadata table directly
    const std::string sql =
        "SELECT m.infoHash, m.title, m.authorName, m.authorFingerprint, "
        "m.topics, m.abstractText, m.bep46Uri, m.timestamp, m.sequence, "
        "m.totalBytes, m.microversions, m.isVerified, m.hasTranscopyright, "
        "m.transcopyrightTerms, m.merkleRoot, m.signature, m.seederCount, "
        "m.peerCount, 0.0 AS score, '' AS snippet "
        "FROM publications_meta m WHERE 1=1" +
        sqlFilter + " ORDER BY m.seederCount DESC, m.timestamp DESC LIMIT ?;";

    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
      return results;
    }
    sqlite3_bind_int(stmt, 1, static_cast<int>(limit));
  } else {
    // FTS5 MATCH with BM25 ranking
    const std::string sql =
        "SELECT m.infoHash, m.title, m.authorName, m.authorFingerprint, "
        "m.topics, m.abstractText, m.bep46Uri, m.timestamp, m.sequence, "
        "m.totalBytes, m.microversions, m.isVerified, m.hasTranscopyright, "
        "m.transcopyrightTerms, m.merkleRoot, m.signature, m.seederCount, "
        "m.peerCount, f.rank AS score, "
        "snippet(publications_fts, 4, '⟦', '⟧', '...', 16) AS snippet "
        "FROM publications_fts f "
        "JOIN publications_meta m ON f.infoHash = m.infoHash "
        "WHERE publications_fts MATCH ?" +
        sqlFilter + " ORDER BY f.rank LIMIT ?;";

    if (sqlite3_prepare_v2(impl_->db, sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
      return results;
    }
    sqlite3_bind_text(stmt, 1, ftsMatch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, static_cast<int>(limit));
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SearchResult res;
    res.entry.infoHash =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    res.entry.title =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    res.entry.authorName =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
    res.entry.authorFingerprint =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
    res.entry.topics = stringToTopics(
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4)));
    res.entry.abstractText =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
    res.entry.bep46Uri =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
    res.entry.timestamp =
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 7));
    res.entry.sequence =
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 8));
    res.entry.totalBytes =
        static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 9));
    res.entry.microversions =
        static_cast<std::uint32_t>(sqlite3_column_int(stmt, 10));
    res.isVerified              = (sqlite3_column_int(stmt, 11) != 0);
    res.entry.hasTranscopyright = (sqlite3_column_int(stmt, 12) != 0);
    res.entry.transcopyrightTerms =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 13));
    if (const auto opt = fromHex32(
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 14)))) {
      res.entry.merkleRoot = *opt;
    }
    res.entry.signature =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 15));
    res.seederCount = sqlite3_column_int(stmt, 16);
    res.peerCount   = sqlite3_column_int(stmt, 17);
    res.score       = static_cast<float>(sqlite3_column_double(stmt, 18));
    if (const char *snip =
            reinterpret_cast<const char *>(sqlite3_column_text(stmt, 19))) {
      res.snippet = snip;
    }
    results.push_back(std::move(res));
  }
  sqlite3_finalize(stmt);

  return results;
}

std::vector<std::pair<std::string, std::size_t>>
SwarmCatalogIndex::topTopics(const std::size_t limit) const {
  std::vector<std::pair<std::string, std::size_t>> out;
  if (!impl_ || !impl_->db) {
    return out;
  }

  static constexpr const char *kSql = "SELECT topics FROM publications_meta;";
  sqlite3_stmt *stmt                = nullptr;
  if (sqlite3_prepare_v2(impl_->db, kSql, -1, &stmt, nullptr) != SQLITE_OK) {
    return out;
  }

  std::map<std::string, std::size_t> counts;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *txt =
        reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (txt) {
      for (const auto &t : stringToTopics(txt)) {
        if (!t.empty()) {
          counts[t]++;
        }
      }
    }
  }
  sqlite3_finalize(stmt);

  out.reserve(counts.size());
  for (auto &&[t, c] : counts) {
    out.emplace_back(std::move(t), c);
  }

  std::ranges::sort(out, [](const auto &a, const auto &b) {
    return (a.second > b.second) || (a.second == b.second && a.first < b.first);
  });

  if (out.size() > limit) {
    out.resize(limit);
  }
  return out;
}

std::size_t SwarmCatalogIndex::count() const {
  if (!impl_ || !impl_->db) {
    return 0;
  }
  static constexpr const char *kSql = "SELECT COUNT(*) FROM publications_meta;";
  sqlite3_stmt *stmt                = nullptr;
  std::size_t cnt                   = 0;
  if (sqlite3_prepare_v2(impl_->db, kSql, -1, &stmt, nullptr) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      cnt = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
    }
    sqlite3_finalize(stmt);
  }
  return cnt;
}

} // namespace xudu
