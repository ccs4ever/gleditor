#ifndef XUDU_PUBLICATION_OUTBOX_HPP
#define XUDU_PUBLICATION_OUTBOX_HPP

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "publication.hpp"

namespace xanadu {

struct PublicationSeed {
  InfoHash hash;
  std::string metainfo;
  std::filesystem::path savePath;
  std::uint64_t bytes{};
};

/// Check one retained torrent, including paths, lengths and every piece.
[[nodiscard]] PublicationSeed
reviewPublicationSeed(const InfoHash &hash,
                      const std::filesystem::path &directory);

/// Verifies every piece before a seed-mode torrent can be offered to peers.
/// Roots are immutable seed directories containing <hash>/metainfo.torrent.
[[nodiscard]] std::vector<PublicationSeed>
reviewPublicationDependencies(const Publication &publication,
                              const std::vector<std::filesystem::path> &roots);

/// Install a verified complete publication with immutable seeds for offline
/// reopening. The destination must not exist; failure removes staged output.
/// The returned Store owns its retained content source and borrows no roots.
[[nodiscard]] std::unique_ptr<Store>
installPublication(const Publication &publication,
                   const std::vector<std::filesystem::path> &roots,
                   std::shared_ptr<UserPermascroll> readerPermascroll,
                   const std::filesystem::path &destination);

enum class PublicationPhase : std::uint8_t {
  Queued,
  LocalReady,
  NeedsVerification,
  Seeding,
  AwaitingDht,
  Published,
  Failed,
  Superseded,
};

[[nodiscard]] std::string_view publicationPhaseName(PublicationPhase phase);

enum class PublicationIdentity : std::uint8_t {
  Unknown,
  MockVerified,
  Verified
};

struct PublicationJobStatus {
  std::string id;
  std::string title;
  std::int64_t sequence{};
  PublicationPhase phase{PublicationPhase::Queued};
  PublicationIdentity identity{PublicationIdentity::Unknown};
  InfoHash manifestHash;
  std::uint64_t dependencyBytes{};
  std::size_t dependencyCount{};
  std::uint64_t attempts{};
  std::string error;
};

/// Created, used and destroyed exclusively on the outbox's worker thread.
class PublicationTransport {
public:
  virtual ~PublicationTransport()                               = default;
  virtual void seed(const PublicationSeed &seed)                = 0;
  virtual void announce(const Publication &publication,
                        const InfoHash &hash)                   = 0;
  [[nodiscard]] virtual bool acknowledged(const Publication &publication,
                                          const InfoHash &hash) = 0;
  virtual void poll() {}
  [[nodiscard]] virtual std::uint16_t listenPort() const { return 0; }
};

/// The verification callback is a boundary for future Oracle enrollment.
/// A mock result is retained and shown separately from verified identity.
class PublicationOutbox {
public:
  struct Options {
    std::filesystem::path directory;
    std::chrono::milliseconds retryInterval{std::chrono::seconds{2}};
    std::function<PublicationIdentity(const Publication &)> verifyIdentity;
    std::function<std::unique_ptr<PublicationTransport>()> makeTransport;
  };

  explicit PublicationOutbox(Options options);
  ~PublicationOutbox();
  PublicationOutbox(const PublicationOutbox &)            = delete;
  PublicationOutbox &operator=(const PublicationOutbox &) = delete;

  /// Commits the immutable request before returning. No network work occurs
  /// on the caller's thread. A retry retains its signed sequence and hash.
  std::string submit(const Publication &publication,
                     const std::vector<std::filesystem::path> &roots,
                     bool announce);
  void retry(std::string_view id);
  [[nodiscard]] std::vector<PublicationJobStatus> statuses() const;
  [[nodiscard]] bool waitFor(std::string_view id, PublicationPhase phase,
                             std::chrono::milliseconds timeout) const;
  [[nodiscard]] std::uint16_t listenPort() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class PublicationOutboxUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Uses only named bootstrap nodes, with LSD/trackers/UPnP/NAT-PMP disabled.
/// This factory provides transport, never identity verification.
[[nodiscard]] std::function<std::unique_ptr<PublicationTransport>()>
publicationSwarmTransport(
    MutableKeys keys, SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes);

} // namespace xanadu
#endif
