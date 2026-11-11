#ifndef XANADU_EVIDENCE_IRC_READ_JOB_HPP
#define XANADU_EVIDENCE_IRC_READ_JOB_HPP

#include <memory>
#include <optional>
#include <variant>

#include "common/xanadu/evidence/irc_transport.hpp"

namespace xanadu::evidence {

struct IrcConnectOperation {};
struct IrcLiveReadRequest {
  std::string target;
  std::size_t maxMessages{100};
};

using IrcReadOperation = std::variant<IrcConnectOperation, IrcJoinAuthority,
                                      IrcLiveReadRequest, IrcHistoryRequest>;

struct IrcReadCompletion {
  bool cancelled{};
  bool connected{};
  IrcConnectionIdentity identity;
  IrcPendingReadBounds pendingBounds;
  IrcObservation observation;
  std::optional<IrcJoinReceipt> join;
  std::optional<std::string> readTicket;
};

// Workers return only owned observations. The host retains the session
// capability separately for binding and explicit close/revocation.
class IrcReadJob {
public:
  IrcReadJob();
  ~IrcReadJob();
  IrcReadJob(const IrcReadJob &)            = delete;
  IrcReadJob &operator=(const IrcReadJob &) = delete;

  [[nodiscard]] bool start(std::shared_ptr<IrcSession> session,
                           IrcReadOperation operation);
  void cancel();
  [[nodiscard]] bool busy() const;
  [[nodiscard]] std::optional<IrcReadCompletion> poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace xanadu::evidence

#endif
