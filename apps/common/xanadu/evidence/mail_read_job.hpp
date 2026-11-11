#ifndef XANADU_EVIDENCE_MAIL_READ_JOB_HPP
#define XANADU_EVIDENCE_MAIL_READ_JOB_HPP

#include <memory>
#include <optional>

#include "common/xanadu/evidence/mail_transport.hpp"

namespace xanadu::evidence {

struct MailReadCompletion {
  bool cancelled{};
  MailObservation observation;
};

// The worker owns only transport inputs and returns an observation. Store and
// evidence-case changes remain on the caller's thread.
class MailReadJob {
public:
  MailReadJob();
  ~MailReadJob();
  MailReadJob(const MailReadJob &)            = delete;
  MailReadJob &operator=(const MailReadJob &) = delete;

  [[nodiscard]] bool start(ImapReadProfile profile, ImapReadRequest request,
                           MailCredentialLease credential);
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<MailReadCompletion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence

#endif
