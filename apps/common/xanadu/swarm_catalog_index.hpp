/**
 * @file swarm_catalog_index.hpp
 * @brief Embedded SQLite FTS5 search engine for swarm publications and topics.
 */
#ifndef XUDU_SWARM_CATALOG_INDEX_HPP
#define XUDU_SWARM_CATALOG_INDEX_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "publication_ledger.hpp"

namespace xanadu {

/**
 * @struct SearchResult
 * @brief Result item returned from SwarmCatalogIndex search.
 */
struct SearchResult {
  PublicationEntry entry;
  int seederCount{0};
  int peerCount{0};
  bool isVerified{false};
  float score{0.0F};
  std::string snippet;
};

/**
 * @class SwarmCatalogIndex
 * @brief In-process SQLite FTS5 search index with Xanadulogical Query Language
 * support.
 */
class SwarmCatalogIndex {
public:
  SwarmCatalogIndex();
  explicit SwarmCatalogIndex(const std::string &dbPath);
  ~SwarmCatalogIndex();

  SwarmCatalogIndex(const SwarmCatalogIndex &)            = delete;
  SwarmCatalogIndex &operator=(const SwarmCatalogIndex &) = delete;
  SwarmCatalogIndex(SwarmCatalogIndex &&) noexcept;
  SwarmCatalogIndex &operator=(SwarmCatalogIndex &&) noexcept;

  /// Index or update a publication entry.
  void indexPublication(const PublicationEntry &entry, int seederCount = 0,
                        int peerCount = 0, bool isVerified = true);

  /// Index an entire publication ledger.
  void indexLedger(const PublicationLedger &ledger);

  /// Remove a publication from the index.
  void removePublication(std::string_view infoHash);

  /// Execute an XQL (Xanadulogical Query Language) search.
  [[nodiscard]] std::vector<SearchResult> search(std::string_view xqlQuery,
                                                 std::size_t limit = 50) const;

  /// Retrieve top topic tags with publication frequencies.
  [[nodiscard]] std::vector<std::pair<std::string, std::size_t>>
  topTopics(std::size_t limit = 30) const;

  /// Total count of indexed publications.
  [[nodiscard]] std::size_t count() const;

  /// Parse XQL query into FTS5 MATCH clause and SQL WHERE filter.
  static void parseXanadulogicalQuery(std::string_view query,
                                      std::string &ftsMatch,
                                      std::string &sqlFilter);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu

#endif // XUDU_SWARM_CATALOG_INDEX_HPP
