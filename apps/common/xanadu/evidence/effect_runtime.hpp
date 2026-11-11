#ifndef XANADU_EVIDENCE_EFFECT_RUNTIME_HPP
#define XANADU_EVIDENCE_EFFECT_RUNTIME_HPP

#include "common/xanadu/evidence/effect_adapter.hpp"
#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/fediverse_transport.hpp"
#include "common/xanadu/evidence/irc_transport.hpp"
#include "common/xanadu/evidence/mail_transport.hpp"

namespace xanadu::evidence {

struct EffectGrantPolicy {
  std::chrono::milliseconds lifetime{std::chrono::minutes(5)};
};

struct EffectRuntimeOptions {
  FediverseTransportPolicy fediverse;
};

// One owner mutates cases. Workers consume owned requests and return owned
// receipts; closing a case never grants authority or restarts an attempt.
class EffectRuntime {
public:
  EffectRuntime(EvidenceCase &activeCase,
                std::shared_ptr<UserPermascroll> permascroll,
                EffectRuntimeOptions options = {});
  ~EffectRuntime();
  EffectRuntime(const EffectRuntime &)            = delete;
  EffectRuntime &operator=(const EffectRuntime &) = delete;
  [[nodiscard]] RecordId draft(const EffectRequest &request);
  [[nodiscard]] EffectRequest request(RecordId intent) const;
  void
  bindFediverseCredential(RecordId intent,
                          std::shared_ptr<FediverseCredentialLease> credential);
  void bindIrcSession(RecordId intent, std::shared_ptr<IrcSession> session);
  void bindMailCredential(RecordId intent,
                          std::shared_ptr<MailCredentialLease> credential);
  [[nodiscard]] RecordId authorize(RecordId intent,
                                   EffectGrantPolicy policy = {});
  void revoke(RecordId authority);
  void revokeAll();
  [[nodiscard]] RecordId dispatch(RecordId intent, RecordId authority,
                                  bool retry                  = false,
                                  bool acknowledgeDuplication = false);
  void cancel();
  [[nodiscard]] bool busy() const;
  // Ingests late results into their original case, never into a new case.
  // A conflicting case owner defers ingestion rather than discarding a result.
  bool poll();
  void recover();
  [[nodiscard]] std::vector<RecordId> importHistory(std::string_view history,
                                                    std::string_view source);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace xanadu::evidence
#endif
