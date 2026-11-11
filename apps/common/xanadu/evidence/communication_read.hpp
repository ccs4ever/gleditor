#ifndef XANADU_EVIDENCE_COMMUNICATION_READ_HPP
#define XANADU_EVIDENCE_COMMUNICATION_READ_HPP

#include <memory>
#include <optional>

#include "common/xanadu/evidence/evidence_case.hpp"
#include "common/xanadu/evidence/fediverse_transport.hpp"

namespace xanadu::evidence {

// The worker owns only request data and capability leases. EvidenceCase and
// Store access stay on the thread that polls and retains the result.
class CommunicationReadJob {
public:
  explicit CommunicationReadJob(FediverseTransportPolicy policy = {});
  ~CommunicationReadJob();
  CommunicationReadJob(const CommunicationReadJob &)            = delete;
  CommunicationReadJob &operator=(const CommunicationReadJob &) = delete;

  bool start(FediverseReadRequest request,
             std::shared_ptr<FediverseCredentialLease> credential = {},
             std::shared_ptr<FediverseAuthority> authority        = {});
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<FediverseTransportResult> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Retains typed semantic records once, then their response-page artifacts and
// gaps. A redacted page is explicitly retained as withheld metadata.
[[nodiscard]] CommunicationRetention
retainFediverseRead(EvidenceCase &evidence,
                    const FediverseTransportResult &result);

} // namespace xanadu::evidence
#endif
