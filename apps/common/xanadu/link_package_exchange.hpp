#ifndef XUDU_LINK_PACKAGE_EXCHANGE_HPP
#define XUDU_LINK_PACKAGE_EXCHANGE_HPP

#include "author_catalog.hpp"
#include "publication_inbox.hpp"

namespace xanadu {
enum class LinkPackagePhase : std::uint8_t {
  Queued,
  NeedsVerification,
  Seeding,
  Downloading,
  AwaitingDht,
  Published,
  Ready,
  Failed,
  Cancelled,
  Superseded
};
struct LinkPackageStatus {
  std::string id;
  LinkPackagePhase phase{LinkPackagePhase::Queued};
  PublicationIdentity identity{PublicationIdentity::Unknown};
  LinkPackage package;
  InfoHash hash;
  bool received{};
  bool announce{};
  std::string error;
};
class LinkPackageExchangeUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
/// Retains independent signed packages without adopting links into a store.
/// Network sessions and immutable seed creation belong to one worker.
class LinkPackageExchange {
public:
  struct Options {
    std::filesystem::path directory;
    std::function<PublicationIdentity(const LinkPackage &)> verifyIdentity;
    std::function<std::unique_ptr<PublicationTransport>()> makePublisher;
    std::function<std::unique_ptr<PublicationDownloadTransport>()>
        makeDownloader;
    std::chrono::milliseconds retryInterval{std::chrono::seconds{2}};
    std::size_t maximumRequests{128};
  };
  explicit LinkPackageExchange(Options options);
  ~LinkPackageExchange();
  LinkPackageExchange(const LinkPackageExchange &)            = delete;
  LinkPackageExchange &operator=(const LinkPackageExchange &) = delete;
  std::string submit(const LinkPackage &package, bool announce);
  std::string fetch(const SignedAuthorCatalog &catalog,
                    const AuthorCatalogEntry &entry,
                    std::vector<std::string> queriedScrolls = {});
  void retry(std::string_view id);
  void cancel(std::string_view id);
  [[nodiscard]] LinkPackageStatus status(std::string_view id) const;
  [[nodiscard]] std::vector<LinkPackageStatus> statuses() const;
  [[nodiscard]] bool waitFor(std::string_view id, LinkPackagePhase phase,
                             std::chrono::milliseconds timeout) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::string_view linkPackagePhaseName(LinkPackagePhase phase);
} // namespace xanadu
#endif
