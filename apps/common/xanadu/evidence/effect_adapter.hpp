#ifndef XANADU_EVIDENCE_EFFECT_ADAPTER_HPP
#define XANADU_EVIDENCE_EFFECT_ADAPTER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/evidence/capture.hpp"
#include "common/xanadu/evidence/fediverse_transport.hpp"
#include "common/xanadu/evidence/irc_transport.hpp"
#include "common/xanadu/evidence/mail_transport.hpp"

namespace xanadu::evidence {

enum class EffectKind {
  DelayedFake,
  SqlPatch,
  Process,
  FediversePost,
  IrcSend,
  MailSubmit
};
enum class EffectOutcome {
  Completed,
  Rejected,
  Failed,
  Cancelled,
  Unknown,
  Partial,
  Accepted
};

struct EffectLimits {
  std::uint64_t timeoutMs{5000};
  std::uint64_t maxOutputBytes{1024 * 1024};
  std::uint64_t maxInputBytes{64 * 1024};
  std::uint64_t maxFields{4096};
  std::uint64_t terminateGraceMs{100};
};

struct SqlGuard {
  std::string query;
  std::vector<Value> parameters;
  Value expected;
};

struct IrcSendRequest {
  std::string serverAuthority;
  std::string networkIncarnation;
  std::string connectionIncarnation;
  std::string target;
  std::string payload;
  std::vector<std::string> audience;
};

struct MailSubmitRequest {
  SmtpSubmissionProfile profile;
  SerializedMailDraft draft;
};

// Owned values only: workers never receive a Store, arena, or cell reference.
struct EffectRequest {
  EffectKind kind{EffectKind::DelayedFake};
  std::string target;
  EffectLimits limits;
  // Arguments only; the adapter supplies argv[0] from the canonical target.
  std::vector<std::string> argv;
  std::string cwd;
  std::map<std::string, std::string> environment;
  std::string stdinBytes;
  std::string sql;
  std::vector<Value> parameters;
  std::optional<SqlGuard> guard;
  std::uint64_t expectedAffectedRows{1};
  std::uint64_t fakeDelayMs{};
  bool fakeUnknown{};
  std::optional<FediversePostRequest> fediversePost;
  std::optional<IrcSendRequest> ircSend;
  std::optional<MailSubmitRequest> mailSubmit;
};

struct EffectResult {
  EffectOutcome outcome{EffectOutcome::Failed};
  std::string resolvedTarget;
  std::string detail;
  std::string stdoutBytes;
  std::string stderrBytes;
  std::optional<int> exitCode;
  std::optional<int> signal;
  std::optional<std::uint64_t> affectedRows;
  bool mayHaveExecuted{};
  std::optional<FediversePostResult> fediversePost;
  std::optional<IrcSendReceipt> ircSend;
  std::optional<SmtpSubmissionResult> smtpSubmission;
};

// Host-bound authority: retaining an ID never recreates this object on restart.
// The worker rechecks revocation immediately before its first external action.
struct EffectAuthority {
  EffectKind operation;
  std::string canonicalTarget;
  std::atomic_bool revoked{};
  std::string targetFingerprint;
  std::chrono::steady_clock::time_point expiresAt{
      std::chrono::steady_clock::time_point::max()};
};

[[nodiscard]] std::optional<std::string>
effectProblem(const EffectRequest &request);
[[nodiscard]] std::string canonicalEffectTarget(const EffectRequest &request);
[[nodiscard]] std::string effectTargetFingerprint(const EffectRequest &request);
[[nodiscard]] std::optional<std::string>
effectAuthorityProblem(const EffectRequest &request,
                       const EffectAuthority &authority,
                       const std::atomic_bool &cancel);
[[nodiscard]] std::string_view effectKindName(EffectKind kind);
[[nodiscard]] std::string_view outcomeName(EffectOutcome outcome);
[[nodiscard]] EffectResult executeSqlPatch(const EffectRequest &request,
                                           const EffectAuthority &authority,
                                           const std::atomic_bool &cancel);
[[nodiscard]] EffectResult executeProcess(const EffectRequest &request,
                                          const EffectAuthority &authority,
                                          const std::atomic_bool &cancel);

} // namespace xanadu::evidence
#endif
