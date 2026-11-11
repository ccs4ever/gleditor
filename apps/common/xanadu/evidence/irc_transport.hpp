#ifndef XANADU_EVIDENCE_IRC_TRANSPORT_HPP
#define XANADU_EVIDENCE_IRC_TRANSPORT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/evidence/communications.hpp"

namespace xanadu::evidence {

struct IrcCredentialLease {
  std::string serverAuthority;
  std::string accountScope;
  std::string saslUsername;
  std::chrono::steady_clock::time_point expiresAt;
  std::shared_ptr<std::atomic_bool> revoked;
  // Called only after verified TLS and SASL capability negotiation.
  std::function<std::optional<std::string>()> takeSecret;
  // Revocation zeroizes a secret that has not yet been consumed.
  std::function<void()> revokeSecret;
};

class IrcCredentialBroker {
public:
  [[nodiscard]] std::optional<IrcCredentialLease>
  issue(std::string serverAuthority, std::string accountScope,
        std::string saslUsername, std::string password,
        std::chrono::steady_clock::duration lifetime) const;
  void revoke(const IrcCredentialLease &lease) const noexcept;
};

struct IrcConnectionConfig {
  std::string host;
  std::uint16_t port{};
  std::string serverAuthority;
  std::string networkIncarnation;
  std::string nick;
  std::string username;
  std::string realName;
  std::string accountScope;
  // Optional explicit trust anchor, primarily for enterprise deployments.
  // System trust remains enabled when this is empty.
  std::string caBundlePath;
  bool useTls{true};
  std::vector<std::string> requestedCapabilities{"server-time", "message-tags",
                                                 "batch", "echo-message",
                                                 "draft/chathistory"};
  std::optional<IrcCredentialLease> sasl;
  std::size_t maxLineBytes{8192};
  std::size_t maxTagBytes{4096};
  std::size_t maxBatchDepth{16};
  std::size_t maxEvents{4096};
  std::size_t maxReadTargets{256};
  // Aggregate, session-wide cache limits for unacknowledged owner-thread reads.
  std::size_t maxPendingReads{64};
  std::size_t maxPendingRecords{4096};
  std::size_t maxPendingBytes{1024 * 1024};
  std::chrono::milliseconds timeout{10000};
};

struct IrcHistoryRequest {
  std::string target;
  std::string afterMessageId;
  std::size_t limit{1000};
};

struct IrcJoinAuthority {
  const std::string serverAuthority;
  const std::string networkIncarnation;
  const std::string target;
  const std::vector<std::string> audience;
  const std::chrono::steady_clock::time_point expiresAt;
  const std::shared_ptr<std::atomic_bool> revoked;
};

struct IrcJoinReceipt {
  bool joined{};
  std::string serverReply;
  std::string detail;
  IrcObservation observation;
};

struct IrcSendAuthority {
  const std::string serverAuthority;
  const std::string networkIncarnation;
  const std::string connectionIncarnation;
  const std::string target;
  const std::string payload;
  const std::vector<std::string> audience;
  const std::chrono::steady_clock::time_point expiresAt;
  const std::shared_ptr<std::atomic_bool> revoked;
};

// EchoAcknowledged means a matching target/payload server echo was observed
// during the active operation. IRC does not prove recipient delivery or
// exactly-once handling through this echo.
enum class IrcSendState { Rejected, EchoAcknowledged, WrittenUnknown };

struct IrcSendReceipt {
  IrcSendState state{IrcSendState::Rejected};
  std::string serverReply;
  std::optional<std::string> echoMessageId;
  std::string detail;
};

struct IrcTransportResult {
  IrcObservation observation;
  bool connected{};
  std::string detail;
  // Opaque session-local token. Async owners acknowledge only after durable
  // retention; retries replay the same immutable operation snapshot.
  std::optional<std::string> readTicket;
};

struct IrcConnectionIdentity {
  std::string serverAuthority;
  std::string networkIncarnation;
  std::string connectionIncarnation;
};

struct IrcPendingReadBounds {
  std::size_t maxPendingReads{};
  std::size_t maxPendingRecords{};
  std::size_t maxPendingBytes{};
};

// Pure preflight helpers. The canonical authority names the configured
// endpoint, not a caller supplied alias.
[[nodiscard]] std::optional<std::string>
ircConnectionProblem(const IrcConnectionConfig &config);
[[nodiscard]] std::optional<std::string>
ircServerAuthority(const IrcConnectionConfig &config);

// A single bounded session. Methods own all data and never touch evidence or a
// store; callers materialize returned observations on their owner thread.
class IrcSession {
public:
  explicit IrcSession(IrcConnectionConfig config);
  ~IrcSession();
  IrcSession(const IrcSession &)            = delete;
  IrcSession &operator=(const IrcSession &) = delete;
  IrcSession(IrcSession &&) noexcept;
  IrcSession &operator=(IrcSession &&) noexcept;

  [[nodiscard]] IrcTransportResult connect(const std::atomic_bool &cancel);
  // Immutable binding identity, available before connection for admission
  // checks. The transport rechecks it again at each send.
  [[nodiscard]] IrcConnectionIdentity identity() const;
  [[nodiscard]] IrcPendingReadBounds pendingReadBounds() const noexcept;
  [[nodiscard]] IrcTransportResult readLive(std::size_t maxMessages,
                                            const std::atomic_bool &cancel);
  // Legacy synchronous reads acknowledge immediately on return. Async owners
  // should use pendingLive/pendingHistory and acknowledge only after retention.
  [[nodiscard]] IrcTransportResult readLive(std::string_view target,
                                            std::size_t maxMessages,
                                            const std::atomic_bool &cancel);
  [[nodiscard]] IrcTransportResult pendingLive(std::string_view target,
                                               std::size_t maxMessages,
                                               const std::atomic_bool &cancel);
  [[nodiscard]] IrcTransportResult history(const IrcHistoryRequest &request,
                                           const std::atomic_bool &cancel);
  [[nodiscard]] IrcTransportResult
  pendingHistory(const IrcHistoryRequest &request,
                 const std::atomic_bool &cancel);
  [[nodiscard]] bool acknowledgeRead(std::string_view ticket);
  [[nodiscard]] IrcJoinReceipt join(const IrcJoinAuthority &authority,
                                    const std::atomic_bool &cancel);
  [[nodiscard]] IrcSendReceipt send(const IrcSendAuthority &authority,
                                    const std::atomic_bool &cancel);
  void cancel() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence
#endif
