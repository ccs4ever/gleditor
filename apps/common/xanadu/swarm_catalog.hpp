/**
 * @file swarm_catalog.hpp
 * @brief Catalog data coordinator for Swarm Telescope discovery.
 */
#ifndef XUDU_SWARM_CATALOG_HPP
#define XUDU_SWARM_CATALOG_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "publication_ledger.hpp"
#include "swarm_catalog_index.hpp"

namespace xanadu {

enum class CatalogCategory : std::uint8_t {
  RecentLocal,
  FollowedAuthors,
  TopicSwarms
};

struct AuthorNode {
  std::string name;
  std::string fingerprint;
  std::string pubKeyHex;
  int seederNodes{0};
  std::size_t publicationCount{0};
};

struct TopicSwarmNode {
  std::string topic;
  std::string infoHash;
  int activePeers{0};
  std::size_t publicationCount{0};
};

/**
 * @class SwarmCatalog
 * @brief Coordinates publication metadata, author hubs, topic swarms, and
 * search.
 */
class SwarmCatalog {
public:
  SwarmCatalog();
  explicit SwarmCatalog(const std::string &cacheDbPath);
  ~SwarmCatalog();

  SwarmCatalog(const SwarmCatalog &)            = delete;
  SwarmCatalog &operator=(const SwarmCatalog &) = delete;
  SwarmCatalog(SwarmCatalog &&) noexcept;
  SwarmCatalog &operator=(SwarmCatalog &&) noexcept;

  /// Retrieve followed author nodes.
  [[nodiscard]] std::vector<AuthorNode> followedAuthors() const;

  /// Retrieve active topic swarm nodes.
  [[nodiscard]] std::vector<TopicSwarmNode> topicSwarms() const;

  /// Search publications using Xanadulogical Query Language (XQL).
  [[nodiscard]] std::vector<SearchResult>
  search(std::string_view query,
         std::optional<CatalogCategory> category = std::nullopt,
         std::size_t limit                       = 50) const;

  /// Retrieve a specific publication by info hash.
  [[nodiscard]] std::optional<PublicationEntry>
  findPublication(std::string_view infoHash) const;

  /// Add a publication entry to the catalog.
  void addPublication(PublicationEntry entry, int seeders = 10, int peers = 2,
                      bool verified = true);

  /// Access underlying search index.
  [[nodiscard]] SwarmCatalogIndex &index();
  [[nodiscard]] const SwarmCatalogIndex &index() const;

  /// Access underlying Merkle publication ledger.
  [[nodiscard]] const PublicationLedger &ledger() const;

private:
  void seedDefaultDocuverse();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu

#endif // XUDU_SWARM_CATALOG_HPP
