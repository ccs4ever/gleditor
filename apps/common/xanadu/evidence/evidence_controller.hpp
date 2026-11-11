#ifndef XANADU_EVIDENCE_CONTROLLER_HPP
#define XANADU_EVIDENCE_CONTROLLER_HPP

#include "common/xanadu/evidence/communication_read.hpp"
#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/irc_read_job.hpp"
#include "common/xanadu/evidence/mail_transport.hpp"

namespace xanadu::evidence {

struct Snapshot {
  bool open{};
  bool busy{};
  std::string title;
  std::string path;
  std::string status;
  std::vector<Record> records;
  std::optional<RecordId> selected;
  std::vector<std::string> details;
  std::optional<SpanSelection> span;
  std::optional<RecordId> spanTarget;
};

// Commands and completion ingestion run on the caller/UI owner. CaptureJob
// performs bounded I/O independently and returns owned data through poll().
class EvidenceController {
public:
  explicit EvidenceController(std::shared_ptr<UserPermascroll> permascroll,
                              FediverseTransportPolicy readPolicy = {});
  ~EvidenceController();
  EvidenceController(const EvidenceController &)            = delete;
  EvidenceController &operator=(const EvidenceController &) = delete;
  bool execute(std::string_view command);
  // Credentials bypass command text and persisted records. Closing the case
  // revokes every read lease issued by this owner.
  bool bindMailReadCredential(std::string_view connectionName,
                              std::string username, std::string password,
                              std::chrono::milliseconds lifetime);
  bool bindMailSubmitCredential(RecordId intent, std::string username,
                                std::string password,
                                std::chrono::milliseconds lifetime);
  bool revokeMailSubmitCredential(RecordId intent);
  bool bindIrcSaslCredential(std::string_view connectionName,
                             std::string username, std::string password,
                             std::chrono::milliseconds lifetime);
  bool revokeIrcSaslCredential(std::string_view connectionName);
  bool poll();
  void select(RecordId id);
  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] const EvidenceCase &evidenceCase() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence
#endif
