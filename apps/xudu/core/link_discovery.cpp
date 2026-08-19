/**
 * @file link_discovery.cpp
 * @brief Implementation of link package discovery, ranking, and prominence filtering.
 */
#include "link_discovery.hpp"

#include <algorithm>

namespace xudu {

bool LinkDiscoveryEngine::addBlessing(Blessing blessing) {
  if (!verifyBlessing(blessing)) {
    return false;
  }
  registeredBlessings.push_back(std::move(blessing));
  return true;
}

std::vector<const Blessing *>
LinkDiscoveryEngine::blessingsFor(const std::string &targetDocument,
                                 const std::string &packageKey) const {
  std::vector<const Blessing *> found;
  for (const auto &blessing : registeredBlessings) {
    if (blessing.targetDocument == targetDocument &&
        blessing.endorsedPackage == packageKey) {
      found.push_back(&blessing);
    }
  }
  return found;
}

std::vector<RankedLinkPackage>
LinkDiscoveryEngine::rankPackages(
    const std::vector<const LinkPackage *> &candidates,
    const PublicKey &documentAuthor,
    const std::string &targetDocumentKey,
    const std::vector<PublicKey> &transcludedAuthors) const {

  std::vector<RankedLinkPackage> curatedPackages;
  std::vector<RankedLinkPackage> publicPackages;

  for (const auto *pkg : candidates) {
    if (nullptr == pkg) {
      continue;
    }

    RankedLinkPackage ranked;
    ranked.package = pkg;

    const bool followed = isFollowed(pkg->curator);
    if (followed) {
      ranked.tier  = ProminenceTier::Curated;
      ranked.score = 1000 + pkg->sequence;
    } else {
      ranked.tier  = ProminenceTier::Public;
      ranked.score = pkg->sequence;
    }

    const auto bList = blessingsFor(targetDocumentKey, pkg->packageKey());
    ranked.blessings = bList;

    for (const auto *blessing : bList) {
      if (blessing->author == documentAuthor) {
        ranked.score += 500;
        ranked.hasAuthorBlessing = true;
      } else if (std::ranges::find(transcludedAuthors, blessing->author) !=
                 transcludedAuthors.end()) {
        ranked.score += 200;
      } else if (isFollowed(blessing->author)) {
        ranked.score += 100;
      } else {
        ranked.score += 20;
      }
    }

    if (ranked.tier == ProminenceTier::Curated) {
      curatedPackages.push_back(std::move(ranked));
    } else {
      publicPackages.push_back(std::move(ranked));
    }
  }

  const auto comparator = [](const RankedLinkPackage &a,
                             const RankedLinkPackage &b) {
    return a.score > b.score;
  };

  std::ranges::sort(curatedPackages, comparator);
  std::ranges::sort(publicPackages, comparator);

  if (publicPackages.size() > maxPublicPackages) {
    publicPackages.resize(maxPublicPackages);
  }

  std::vector<RankedLinkPackage> combined;
  combined.reserve(curatedPackages.size() + publicPackages.size());
  for (auto &item : curatedPackages) {
    combined.push_back(std::move(item));
  }
  for (auto &item : publicPackages) {
    combined.push_back(std::move(item));
  }

  return combined;
}

} // namespace xudu
