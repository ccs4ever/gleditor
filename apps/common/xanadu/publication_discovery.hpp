#ifndef XUDU_PUBLICATION_DISCOVERY_HPP
#define XUDU_PUBLICATION_DISCOVERY_HPP

#include "author_catalog.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <stop_token>

namespace xanadu {
enum class DiscoveryPhase : std::uint8_t { Queued, Searching, Ready, Failed };
struct PublicationDiscoveryStatus {
  std::string query;
  bool author{};
  DiscoveryPhase phase{DiscoveryPhase::Queued};
  std::string error;
  std::vector<SignedAuthorCatalog> catalogs;
};
class PublicationDiscoveryTransport {
public:
  virtual ~PublicationDiscoveryTransport() = default;
  virtual std::string author(const PublicKey &key, std::stop_token stop) = 0;
  virtual std::vector<std::string> topic(std::string_view topic,
                                         std::stop_token stop)           = 0;
};
/// Explicit discovery runs on one worker. Stored signatures/high-water marks
/// survive restart; cached metadata is never presented as a fresh DHT lookup.
class PublicationDiscovery {
public:
  struct Options {
    std::filesystem::path directory;
    std::function<std::unique_ptr<PublicationDiscoveryTransport>()>
        makeTransport;
    std::size_t maximumRequests{128};
  };
  explicit PublicationDiscovery(Options options);
  ~PublicationDiscovery();
  PublicationDiscovery(const PublicationDiscovery &)            = delete;
  PublicationDiscovery &operator=(const PublicationDiscovery &) = delete;
  std::string submit(std::string_view query, bool author);
  [[nodiscard]] PublicationDiscoveryStatus status(std::string_view id) const;
  [[nodiscard]] std::vector<SignedAuthorCatalog> cachedCatalogs() const;
  [[nodiscard]] std::vector<PublicKey> followedAuthors() const;
  [[nodiscard]] bool waitFor(std::string_view id, DiscoveryPhase phase,
                             std::chrono::milliseconds timeout) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::function<std::unique_ptr<PublicationDiscoveryTransport>()>
publicationDiscoverySwarmTransport(
    SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes,
    std::filesystem::path scratchDirectory,
    std::chrono::milliseconds timeout = std::chrono::seconds{30});
} // namespace xanadu
#endif
