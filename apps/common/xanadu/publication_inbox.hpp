#ifndef XUDU_PUBLICATION_INBOX_HPP
#define XUDU_PUBLICATION_INBOX_HPP

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "publication_outbox.hpp"

namespace xanadu {

enum class PublicationDownloadPhase : std::uint8_t {
  Queued,
  Resolving,
  Downloading,
  Verifying,
  Ready,
  Failed,
  Cancelled
};
[[nodiscard]] std::string_view
publicationDownloadPhaseName(PublicationDownloadPhase phase);

struct PublicationDownloadStatus {
  std::string id;
  std::string uri;
  std::string title;
  PublicationDownloadPhase phase{PublicationDownloadPhase::Queued};
  std::uint64_t completedDependencies{};
  std::uint64_t dependencyCount{};
  std::filesystem::path storePath;
  MicroversionId version;
  std::int64_t sequence{-1};
  InfoHash manifestHash;
  std::string error;
};

/// Constructed, called and destroyed on the inbox worker. Cancellation must
/// interrupt network waits; downloading never accesses the reader's
/// permascroll.
class PublicationDownloadTransport {
public:
  virtual ~PublicationDownloadTransport()              = default;
  virtual MutablePointer resolve(const MutableLink &link,
                                 std::stop_token stop) = 0;
  virtual void fetch(const InfoHash &hash,
                     const std::filesystem::path &directory,
                     std::uint64_t maximumBytes, std::stop_token stop) = 0;
};

/// Downloads are explicit snapshots. No edition changes or subscriptions are
/// inferred from opening a mutable name. Completed native stores remain
/// offline.
class PublicationInbox {
public:
  struct Options {
    std::filesystem::path directory;
    std::uint64_t maximumManifestBytes{16 * 1024 * 1024};
    std::uint64_t maximumDependencyBytes{1024ULL * 1024 * 1024};
    std::size_t maximumDependencies{4096};
    std::function<std::unique_ptr<PublicationDownloadTransport>()>
        makeTransport;
  };
  explicit PublicationInbox(Options options);
  ~PublicationInbox();
  PublicationInbox(const PublicationInbox &)            = delete;
  PublicationInbox &operator=(const PublicationInbox &) = delete;

  /// A subscription pins its observed lower bound; newer signed responses
  /// are admitted, while rollback and equal-sequence conflicts are refused.
  std::string submit(const MutableLink &link,
                     std::optional<MutablePointer> minimum = std::nullopt);
  std::string submitPinned(const PublicationPin &pin);
  void cancel(std::string_view id);
  void retry(std::string_view id);
  [[nodiscard]] std::vector<PublicationDownloadStatus> statuses() const;
  [[nodiscard]] PublicationDownloadStatus status(std::string_view id) const;
  [[nodiscard]] bool waitFor(std::string_view id,
                             PublicationDownloadPhase phase,
                             std::chrono::milliseconds timeout) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Only explicitly named bootstrap nodes and optional direct test peers.
/// Does not verify identity enrollment; signatures authenticate key ownership.
[[nodiscard]] std::function<std::unique_ptr<PublicationDownloadTransport>()>
publicationDownloadSwarmTransport(
    SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes,
    std::vector<std::pair<std::string, std::uint16_t>> peers = {},
    std::chrono::milliseconds timeout = std::chrono::seconds{60});

} // namespace xanadu
#endif
