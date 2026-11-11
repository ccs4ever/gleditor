#ifndef XANADU_EVIDENCE_MAIL_TRANSPORT_HPP
#define XANADU_EVIDENCE_MAIL_TRANSPORT_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/evidence/communications.hpp"

namespace xanadu::evidence {

struct ImapReadProfile;
struct ImapReadRequest;
struct SmtpSubmissionProfile;
struct SerializedMailDraft;
struct MailSubmissionAuthority;
struct SmtpSubmissionResult;

enum class MailCredentialOperation { Read, Submit };

struct MailCredentialScope {
  std::string serverAuthority;
  std::string accountScope;
  MailCredentialOperation operation{MailCredentialOperation::Read};
};

class MailCredentialLease {
public:
  MailCredentialLease() = default;
  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool matches(const MailCredentialScope &scope) const;
  [[nodiscard]] std::chrono::steady_clock::time_point expiresAt() const;

private:
  struct State;
  explicit MailCredentialLease(std::shared_ptr<State> state);
  std::shared_ptr<State> state_;
  friend class MailCredentialBroker;
  friend MailObservation readImapMailbox(const struct ImapReadProfile &,
                                         const struct ImapReadRequest &,
                                         const MailCredentialLease &,
                                         const std::atomic_bool &);
  friend struct SmtpSubmissionResult
  submitSmtp(const struct SmtpSubmissionProfile &,
             const struct SerializedMailDraft &, const MailCredentialLease &,
             const struct MailSubmissionAuthority &, const std::atomic_bool &);
  friend struct MailCredentialInternalAccess;
};

class MailCredentialBroker {
public:
  [[nodiscard]] MailCredentialLease
  issue(MailCredentialScope scope, std::string username, std::string password,
        std::chrono::steady_clock::duration lifetime) const;
  void revoke(const MailCredentialLease &lease) const;
};

struct MailSubmissionAuthority {
  const std::string serverAuthority;
  const std::string accountScope;
  const std::string draftFingerprint;
  const std::string envelopeFrom;
  const std::vector<std::string> envelopeRecipients;
  const std::chrono::steady_clock::time_point expiresAt;
  const std::shared_ptr<std::atomic_bool> revoked;
};

struct ImapReadProfile {
  // URL must be imaps:// or imap://; user-info is forbidden.
  std::string serverUrl;
  std::string serverAuthority;
  std::string accountScope;
  std::string mailbox;
  std::string caBundlePath;
  bool allowCleartextLoopbackWithoutCredentials{};
};

struct ImapReadRequest {
  std::uint32_t afterUid{};
  std::optional<std::uint64_t> afterUidValidity;
  std::size_t maxMessages{100};
  std::size_t maxMessageBytes{1024 * 1024};
  std::size_t maxTotalBytes{8 * 1024 * 1024};
  long timeoutMilliseconds{15000};
  CommunicationBounds retentionBounds{};
};

[[nodiscard]] MailObservation
readImapMailbox(const ImapReadProfile &profile, const ImapReadRequest &request,
                const MailCredentialLease &credential,
                const std::atomic_bool &cancellation);

[[nodiscard]] std::optional<std::string>
imapReadProblem(const ImapReadProfile &profile, const ImapReadRequest &request);
[[nodiscard]] std::optional<std::string>
imapServerAuthority(std::string_view serverUrl);

struct MailDraftAttachment {
  std::string filename;
  std::string contentType;
  std::string bytes;
};

struct MailDraftInput {
  std::string from;
  std::vector<MailRecipientOccurrence> recipients;
  std::string subject;
  std::optional<std::string> inReplyTo;
  std::vector<std::string> references;
  std::string textBody;
  std::optional<std::string> htmlBody;
  std::vector<MailDraftAttachment> attachments;
};

struct SerializedMailDraft {
  std::string messageId;
  std::string envelopeFrom;
  std::vector<std::string> envelopeRecipients;
  std::string bytes;
  // Local correlation only; SMTP provides no general idempotency guarantee.
  std::string correlationId;
};

[[nodiscard]] SerializedMailDraft buildMimeDraft(const MailDraftInput &draft,
                                                 std::string correlationId);

struct SmtpSubmissionProfile {
  // URL must be smtps:// or smtp://; user-info is forbidden.
  std::string serverUrl;
  std::string serverAuthority;
  std::string accountScope;
  std::string caBundlePath;
  bool allowCleartextLoopbackWithoutCredentials{};
  long timeoutMilliseconds{30000};
};

enum class SmtpRecipientState { Accepted, Rejected, Unknown };
struct SmtpRecipientOutcome {
  std::string address;
  SmtpRecipientState state{SmtpRecipientState::Unknown};
  int responseCode{};
};

enum class SmtpTransactionState {
  Accepted,
  Rejected,
  Failed,
  OutcomeUnknown,
  Cancelled
};

struct SmtpSubmissionResult {
  std::string correlationId;
  SmtpTransactionState state{SmtpTransactionState::Failed};
  bool dataStarted{};
  std::vector<SmtpRecipientOutcome> recipients;
  int finalResponseCode{};
  std::string safeDiagnostic;
};

[[nodiscard]] std::string
mailDraftFingerprint(const SerializedMailDraft &draft);

[[nodiscard]] SmtpSubmissionResult
submitSmtp(const SmtpSubmissionProfile &profile,
           const SerializedMailDraft &draft,
           const MailCredentialLease &credential,
           const MailSubmissionAuthority &authority,
           const std::atomic_bool &cancellation);

[[nodiscard]] std::optional<std::string>
smtpSubmissionProblem(const SmtpSubmissionProfile &profile,
                      const SerializedMailDraft &draft);
[[nodiscard]] std::optional<std::string>
smtpServerAuthority(std::string_view serverUrl);

} // namespace xanadu::evidence

#endif
