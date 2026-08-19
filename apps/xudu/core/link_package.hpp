/**
 * @file link_package.hpp
 * @brief Standalone, signed bundles of links published independently of
 * documents.
 */
#ifndef XUDU_LINK_PACKAGE_HPP
#define XUDU_LINK_PACKAGE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mutable_link.hpp"
#include "ops.hpp"
#include "publication.hpp"
#include "scroll.hpp"

namespace xudu {

class Store;

/**
 * @struct LinkPackage
 * @brief A standalone, signed bundle of links published independently by any
 * party.
 *
 * In Xanadu's pluralistic link architecture, anyone can curate and publish a
 * link package that connects spans across documents. Link packages can be
 * adopted into a viewing session in any order without modifying the original
 * documents.
 */
struct LinkPackage {
  PublicKey curator;
  std::string salt;
  std::string title;
  std::int64_t sequence{1};
  std::uint64_t published{};

  std::vector<GlobalLink> links;
  std::map<std::string, Scroll> scrolls;

  Signature signature;

  [[nodiscard]] DhtTarget name() const;
  [[nodiscard]] std::string uri() const;
  [[nodiscard]] std::string packageKey() const;
  [[nodiscard]] std::string describe() const;
};

[[nodiscard]] std::string linkPackageSigningBuffer(const LinkPackage &pkg);
[[nodiscard]] std::string encodeLinkPackage(const LinkPackage &pkg);
[[nodiscard]] std::optional<LinkPackage>
decodeLinkPackage(std::string_view encoded);
[[nodiscard]] bool verifyLinkPackage(const LinkPackage &pkg);

[[nodiscard]] LinkPackage
publishLinkPackage(const MutableKeys &keys, std::string salt, std::string title,
                   std::int64_t sequence, std::uint64_t published,
                   std::vector<GlobalLink> links,
                   std::map<std::string, Scroll> scrolls);

/// Result of adopting a standalone link package into a store.
struct AdoptedLinksResult {
  std::size_t linksAdopted{};
  std::size_t scrollsAdded{};
};

AdoptedLinksResult
adoptLinkPackage(Store &store, const LinkPackage &pkg,
                 ProminenceTier tier = ProminenceTier::Curated);

/**
 * @brief Deterministic DHT rendezvous target for link package discovery.
 *
 * Computes SHA1("xanalinks:" + scrollKey) so that anyone looking for links
 * referencing a given scroll/document queries this target in the DHT.
 */
[[nodiscard]] DhtTarget
linkPackageRendezvousTarget(const std::string &scrollKey);

} // namespace xudu

#endif // XUDU_LINK_PACKAGE_HPP
