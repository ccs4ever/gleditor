/**
 * @file link_discovery.hpp
 * @brief Multi-tier link package discovery and ranking engine.
 */
#ifndef XUDU_LINK_DISCOVERY_HPP
#define XUDU_LINK_DISCOVERY_HPP

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "blessing.hpp"
#include "link_package.hpp"
#include "mutable_link.hpp"
#include "ops.hpp"

namespace xudu {

/**
 * @struct RankedLinkPackage
 * @brief A link package evaluated and ranked by prominence and blessing weights.
 */
struct RankedLinkPackage {
  const LinkPackage *package{nullptr};
  ProminenceTier tier{ProminenceTier::Public};
  std::int64_t score{0};
  bool hasAuthorBlessing{false};
  std::vector<const Blessing *> blessings;
};

/**
 * @class LinkDiscoveryEngine
 * @brief Manages curator subscriptions, author blessings, and bounded public discovery ranking.
 */
class LinkDiscoveryEngine {
public:
  LinkDiscoveryEngine() = default;

  /// Curator subscriptions
  void followCurator(const PublicKey &curator) { followedCurators.insert(curator); }
  void unfollowCurator(const PublicKey &curator) { followedCurators.erase(curator); }
  [[nodiscard]] bool isFollowed(const PublicKey &curator) const {
    return followedCurators.contains(curator);
  }
  [[nodiscard]] const std::set<PublicKey> &curators() const { return followedCurators; }

  /// Blessings registry
  bool addBlessing(Blessing blessing);
  [[nodiscard]] std::vector<const Blessing *>
  blessingsFor(const std::string &targetDocument,
               const std::string &packageKey) const;

  /// Public packages bound
  void setMaxPublicPackages(const std::size_t maxCount) { maxPublicPackages = maxCount; }
  [[nodiscard]] std::size_t getMaxPublicPackages() const { return maxPublicPackages; }

  /**
   * @brief Evaluate and rank candidate link packages for a document.
   *
   * @param candidates All candidate link packages (from subscriptions + DHT).
   * @param documentAuthor The author of the active document.
   * @param targetDocumentKey The full key of the document (e.g. "btpk:...").
   * @param transcludedAuthors Authors of sources quoted in the active document.
   * @return Ordered list of ranked link packages, with public ones bounded by maxPublicPackages.
   */
  [[nodiscard]] std::vector<RankedLinkPackage>
  rankPackages(const std::vector<const LinkPackage *> &candidates,
               const PublicKey &documentAuthor,
               const std::string &targetDocumentKey,
               const std::vector<PublicKey> &transcludedAuthors = {}) const;

private:
  std::set<PublicKey> followedCurators;
  std::vector<Blessing> registeredBlessings;
  std::size_t maxPublicPackages{10};
};

} // namespace xudu

#endif // XUDU_LINK_DISCOVERY_HPP
