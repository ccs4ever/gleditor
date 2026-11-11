#ifndef XANADU_EVIDENCE_FEDIVERSE_TRANSPORT_HPP
#define XANADU_EVIDENCE_FEDIVERSE_TRANSPORT_HPP

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/evidence/communications.hpp"

namespace xanadu::evidence {

enum class FediverseAction { ReadStatuses, PublishStatus };

struct FediverseTransportLimits {
  CommunicationBounds records;
  std::size_t maxPages{16};
  long timeoutMs{5000};
  std::size_t maxHeaderBytes{64 * 1024};
};

struct FediverseReadRequest {
  std::string instanceOrigin;
  std::string accountId;
  std::string accountActorUri;
  std::string accountHandle;
  FediverseTransportLimits limits;
  bool authenticated{};
};

struct FediversePostRequest {
  std::string instanceOrigin;
  std::string accountId;
  std::string text;
  std::string contentWarning;
  std::string visibility{"public"};
  std::optional<std::string> replyToRestId;
  // A declared scope for authorization and retained intent. Mastodon applies
  // recipients through status mentions and visibility; this is not a claim
  // that every intended recipient received or delivered the status.
  std::vector<std::string> audience;
  std::string idempotencyKey;
  FediverseTransportLimits limits;
  std::string accountActorUri;
};

class FediverseCredentialLease {
public:
  FediverseCredentialLease(std::string instanceOrigin, std::string accountId,
                           std::string bearerToken,
                           std::chrono::steady_clock::time_point expiresAt);
  ~FediverseCredentialLease();
  FediverseCredentialLease(const FediverseCredentialLease &) = delete;
  FediverseCredentialLease &
  operator=(const FediverseCredentialLease &) = delete;

  FediverseCredentialLease *revoke() noexcept;
  [[nodiscard]] bool revoked() const noexcept;
  [[nodiscard]] bool usable() const noexcept;
  [[nodiscard]] bool matches(std::string_view instanceOrigin,
                             std::string_view accountId) const noexcept;

private:
  friend class FediverseTransport;
  std::string instanceOrigin_;
  std::string accountId_;
  std::string token_;
  std::chrono::steady_clock::time_point expiresAt_;
  std::atomic_bool revoked_{};
};

struct FediverseAuthority {
  FediverseAuthority(std::string instanceOrigin, std::string accountId,
                     FediverseAction action, std::string requestFingerprint,
                     std::vector<std::string> audience,
                     std::chrono::steady_clock::time_point expiresAt);
  FediverseAuthority(const FediverseAuthority &)            = delete;
  FediverseAuthority &operator=(const FediverseAuthority &) = delete;

  const std::string instanceOrigin;
  const std::string accountId;
  const FediverseAction action;
  const std::string requestFingerprint;
  const std::vector<std::string> audience;
  const std::chrono::steady_clock::time_point expiresAt;
  std::atomic_bool revoked{};
};

struct FediverseTransportGap {
  std::string locator;
  std::string reason;
};

struct FediverseTransportResult {
  FediverseObservation observation;
  std::vector<FediverseTransportGap> gaps;
  std::string error;
  // Exact response pages are separate from interpreted objects and identities.
  std::vector<Page> pages;
};

struct FediversePostResult {
  CaptureState state{CaptureState::Failed};
  long httpStatus{};
  bool accepted{};
  // REST status identity is origin-local. `activityUri` is absent because the
  // Mastodon status response does not identify a corresponding Activity URI.
  std::string restId;
  std::optional<std::string> objectUri;
  std::optional<std::string> actorUri;
  std::optional<std::string> activityUri;
  std::string idempotencyKey;
  std::string visibility;
  std::vector<std::string> audience;
  std::string error;
  bool federationDeliveryConfirmed{};
  bool mayHaveExecuted{};
  std::string responseBody;
  bool responseRedacted{};
  std::string actorIdentityBasis{"unresolved"};
};

struct FediverseTransportPolicy {
  // Production defaults to HTTPS only. Tests may explicitly allow loopback
  // HTTP; non-loopback cleartext remains refused.
  bool allowLoopbackHttpForTests{};
};

// Pure target/limit validation before persisting an immutable connection.
[[nodiscard]] std::optional<std::string>
fediverseReadProblem(const FediverseReadRequest &request,
                     FediverseTransportPolicy policy = {});

[[nodiscard]] std::optional<std::string>
fediversePostProblem(const FediversePostRequest &request,
                     FediverseTransportPolicy policy = {});

[[nodiscard]] std::string
fediverseRequestFingerprint(const FediverseReadRequest &request);
[[nodiscard]] std::string
fediverseRequestFingerprint(const FediversePostRequest &request);

class FediverseTransport {
public:
  explicit FediverseTransport(FediverseTransportPolicy policy = {});

  [[nodiscard]] FediverseTransportResult
  read(const FediverseReadRequest &request,
       const std::shared_ptr<FediverseCredentialLease> &credential,
       const std::shared_ptr<FediverseAuthority> &authority,
       const std::atomic_bool &cancel) const;

  [[nodiscard]] FediversePostResult
  post(const FediversePostRequest &request,
       const std::shared_ptr<FediverseCredentialLease> &credential,
       const std::shared_ptr<FediverseAuthority> &authority,
       const std::atomic_bool &cancel) const;

private:
  FediverseTransportPolicy policy_;
};

} // namespace xanadu::evidence
#endif
