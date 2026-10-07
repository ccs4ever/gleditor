#ifndef XUDU_PUBLICATION_SUBSCRIPTIONS_HPP
#define XUDU_PUBLICATION_SUBSCRIPTIONS_HPP

#include "publication_inbox.hpp"

namespace xanadu {
enum class PublicationSubscriptionPhase : std::uint8_t {
  Waiting,
  Checking,
  Downloading,
  Failed,
  Paused
};
struct PublicationUpdateNotice {
  std::int64_t sequence{};
  std::int64_t previousSequence{};
  InfoHash hash;
  MicroversionId version;
  MicroversionId previousVersion;
  std::string title;
  std::string snapshotId;
  std::string previousSnapshotId;
  bool delivered{};
  bool acknowledged{};
};
struct PublicationSubscriptionStatus {
  std::string id;
  std::string uri;
  std::string title;
  std::int64_t sequence{};
  InfoHash hash;
  MicroversionId version;
  std::string snapshotId;
  bool enabled{true};
  PublicationSubscriptionPhase phase{PublicationSubscriptionPhase::Waiting};
  std::string error;
  std::vector<PublicationUpdateNotice> notices;
};
class PublicationSubscriptionsUnreadable : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};
/// One polling worker resolves signed names; the inbox verifies and retains
/// independent snapshots. Neither polling nor opening repoints an edition.
class PublicationSubscriptions {
public:
  struct Options {
    std::filesystem::path directory;
    PublicationInbox *inbox{};
    std::function<std::unique_ptr<PublicationDownloadTransport>()>
        makeTransport;
    std::chrono::milliseconds pollInterval{std::chrono::seconds{30}};
    std::chrono::milliseconds retryInterval{std::chrono::seconds{5}};
    std::size_t maximumSubscriptions{128};
    std::size_t maximumNotices{128};
  };
  explicit PublicationSubscriptions(Options options);
  ~PublicationSubscriptions();
  PublicationSubscriptions(const PublicationSubscriptions &) = delete;
  PublicationSubscriptions &
  operator=(const PublicationSubscriptions &) = delete;
  std::string subscribe(std::string_view downloadedId);
  void setEnabled(std::string_view id, bool enabled);
  void checkNow(std::string_view id);
  void acknowledge(std::string_view id, std::int64_t sequence);
  [[nodiscard]] std::vector<PublicationSubscriptionStatus> statuses() const;
  [[nodiscard]] PublicationSubscriptionStatus status(std::string_view id) const;
  /// Delivery is committed before the notice leaves this method. Undismissed
  /// notices remain in the Updates palette even after their toast expires.
  [[nodiscard]] std::vector<std::pair<std::string, PublicationUpdateNotice>>
  takeNotifications(std::size_t maximum = 1);
  [[nodiscard]] bool waitForSequence(std::string_view id, std::int64_t sequence,
                                     std::chrono::milliseconds timeout) const;
  [[nodiscard]] bool waitForPhase(std::string_view id,
                                  PublicationSubscriptionPhase phase,
                                  std::chrono::milliseconds timeout) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::string_view
publicationSubscriptionPhaseName(PublicationSubscriptionPhase phase);
} // namespace xanadu
#endif
