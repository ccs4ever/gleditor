/**
 * @file cluster_node.cpp
 * @brief Implementation of concurrent cluster actor node.
 */
#include "common/xanadu/vagent/cluster/cluster_node.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <span>

#include "common/xanadu/identity/identity_serialization.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/zigzag/manifold.hpp"

namespace xanadu::vagent::cluster {

namespace {

class UniqueFd {
public:
  UniqueFd() noexcept : fd_(-1) {}
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &)            = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool isValid() const noexcept { return fd_ >= 0; }
  explicit operator bool() const noexcept { return isValid(); }

  int release() noexcept {
    int old = fd_;
    fd_     = -1;
    return old;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

private:
  int fd_{-1};
};

bool setNonBlocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setNoDelay(int fd) {
  int flag = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

enum class IngressState {
  ReadingHeader,
  ReadingBody,
};

struct PeerConnection {
  UniqueFd fd;
  std::string nodeId;
  identity::Fingerprint fingerprint;
  std::string targetHost;
  std::uint16_t targetPort{0};
  bool isOutbound{false};
  bool connected{false};
  bool handshakeSent{false};
  bool handshakeReceived{false};

  IngressState ingressState{IngressState::ReadingHeader};
  std::vector<std::uint8_t> headerBuf;
  std::uint32_t expectedBodyLen{0};
  std::vector<std::uint8_t> bodyBuf;

  std::deque<std::uint8_t> egressQueue;

  std::chrono::steady_clock::time_point lastAttempt;
};

} // namespace

std::string_view clusterNodeErrorName(ClusterNodeError err) {
  switch (err) {
  case ClusterNodeError::Ok:
    return "Ok";
  case ClusterNodeError::SocketCreationFailed:
    return "SocketCreationFailed";
  case ClusterNodeError::BindFailed:
    return "BindFailed";
  case ClusterNodeError::ListenFailed:
    return "ListenFailed";
  case ClusterNodeError::ConnectionFailed:
    return "ConnectionFailed";
  case ClusterNodeError::PeerNotFound:
    return "PeerNotFound";
  case ClusterNodeError::PeerDisconnected:
    return "PeerDisconnected";
  case ClusterNodeError::FrameTooLarge:
    return "FrameTooLarge";
  case ClusterNodeError::SerializationFailed:
    return "SerializationFailed";
  case ClusterNodeError::BufferOverflow:
    return "BufferOverflow";
  case ClusterNodeError::InvalidAddress:
    return "InvalidAddress";
  case ClusterNodeError::Timeout:
    return "Timeout";
  }
  return "UnknownError";
}

namespace {

identity::Fingerprint defaultFingerprint() {
  return *identity::Fingerprint::fromString(
      "0123456789ABCDEF0123456789ABCDEF01234567");
}

} // namespace

class ClusterNode::Impl {
public:
  Impl(ClusterNode &owner, std::shared_ptr<runtime::CompiledProgram> program,
       Options options)
      : owner_(owner), options_(std::move(options)) {
    options_.hostOptions.author = options_.nodeId;
    owner_.host_ = std::make_unique<runtime::LocalHost>(std::move(*program),
                                                        options_.hostOptions);

    // Register local identity
    if (!options_.identity.isValid()) {
      options_.identity = defaultFingerprint();
    }

    // Install remote sender into AgentExecutor
    owner_.host_->executor().install(
        [this](std::string_view destNode,
               const identity::ActorMessage &message) -> bool {
          return sendToPeer(destNode, message);
        });

    // Wire host sink for launcher emissions and forward rules
    owner_.host_->executor().setHostSink(
        [this](const runtime::Envelope &envelope) {
          handleLocalEmission(envelope);
        });
  }

  ~Impl() { stop(); }

  std::expected<void, ClusterNodeError> start() {
    if (running_) return {};

    listenFd_.reset(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listenFd_) {
      return std::unexpected(ClusterNodeError::SocketCreationFailed);
    }

    int reuse = 1;
    ::setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse));
#ifdef SO_REUSEPORT
    ::setsockopt(listenFd_.get(), SOL_SOCKET, SO_REUSEPORT, &reuse,
                 sizeof(reuse));
#endif

    if (!setNonBlocking(listenFd_.get())) {
      listenFd_.reset();
      return std::unexpected(ClusterNodeError::SocketCreationFailed);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(options_.listenPort);
    if (::inet_pton(AF_INET, options_.listenHost.c_str(), &addr.sin_addr) <=
        0) {
      addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (::bind(listenFd_.get(), reinterpret_cast<sockaddr *>(&addr),
               sizeof(addr)) < 0) {
      listenFd_.reset();
      return std::unexpected(ClusterNodeError::BindFailed);
    }

    socklen_t addrLen = sizeof(addr);
    if (::getsockname(listenFd_.get(), reinterpret_cast<sockaddr *>(&addr),
                      &addrLen) == 0) {
      boundPort_ = ntohs(addr.sin_port);
    } else {
      boundPort_ = options_.listenPort;
    }

    if (::listen(listenFd_.get(), 64) < 0) {
      listenFd_.reset();
      return std::unexpected(ClusterNodeError::ListenFailed);
    }

    running_ = true;

    // Trust configured peers in MeshActorRuntime
    for (const auto &p : options_.peers) {
      identity::Fingerprint fp = p.fingerprint;
      if (!fp.isValid()) {
        fp = defaultFingerprint();
      }
      owner_.host_->runtime().trustNode(p.nodeId, fp);
      preparePeer(p);
    }

    return {};
  }

  void stop() {
    running_ = false;
    listenFd_.reset();
    connections_.clear();
    peersByNodeId_.clear();
  }

  void preparePeer(const PeerEndpoint &p) {
    auto conn                = std::make_shared<PeerConnection>();
    conn->nodeId             = p.nodeId;
    conn->fingerprint        = p.fingerprint;
    conn->targetHost         = p.host;
    conn->targetPort         = p.port;
    conn->isOutbound         = (p.port > 0);
    peersByNodeId_[p.nodeId] = conn;
    connections_.push_back(conn);

    if (conn->isOutbound) {
      attemptConnect(conn);
    }
  }

  void attemptConnect(const std::shared_ptr<PeerConnection> &conn) {
    if (conn->connected || conn->targetPort == 0) return;

    conn->fd.reset(::socket(AF_INET, SOCK_STREAM, 0));
    if (!conn->fd) return;

    setNonBlocking(conn->fd.get());
    setNoDelay(conn->fd.get());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(conn->targetPort);
    if (::inet_pton(AF_INET, conn->targetHost.c_str(), &addr.sin_addr) <= 0) {
      struct hostent *he = ::gethostbyname(conn->targetHost.c_str());
      if (he != nullptr && he->h_addr_list[0] != nullptr) {
        std::memcpy(&addr.sin_addr, he->h_addr_list[0], sizeof(addr.sin_addr));
      } else {
        conn->fd.reset();
        return;
      }
    }

    conn->lastAttempt = std::chrono::steady_clock::now();
    int res = ::connect(conn->fd.get(), reinterpret_cast<sockaddr *>(&addr),
                        sizeof(addr));
    if (res == 0) {
      conn->connected = true;
      sendHandshake(conn);
    } else if (errno == EINPROGRESS) {
      // In-progress non-blocking connect; will finalize in poll
    } else {
      conn->fd.reset();
    }
  }

  void sendHandshake(const std::shared_ptr<PeerConnection> &conn) {
    if (!conn->connected || conn->handshakeSent) return;

    identity::ActorMessage hs;
    hs.type       = identity::MessageType::ActorSend;
    hs.requestId  = "VCLU_HANDSHAKE";
    hs.source     = identity::ActorPid{options_.nodeId, "system", 1, 1};
    hs.target     = identity::ActorPid{conn->nodeId, "system", 1, 1};
    hs.capability = options_.nodeId;

    auto encoded = identity::encodeActorMessage(hs);
    if (encoded) {
      queueFrame(conn, *encoded);
      conn->handshakeSent = true;
    }
  }

  bool sendToPeer(std::string_view destNode,
                  const identity::ActorMessage &message) {
    auto it = peersByNodeId_.find(std::string(destNode));
    if (it == peersByNodeId_.end() || !it->second) {
      return false;
    }
    auto &conn = it->second;
    if (!conn->connected) {
      if (conn->isOutbound && !conn->fd.isValid()) {
        attemptConnect(conn);
      }
    }

    auto encoded = identity::encodeActorMessage(message);
    if (!encoded) return false;

    return queueFrame(conn, *encoded);
  }

  bool queueFrame(const std::shared_ptr<PeerConnection> &conn,
                  std::string_view payload) {
    if (payload.size() > kMaxClusterMessageBytes) return false;
    if (conn->egressQueue.size() + payload.size() + 4 >
        kMaxPeerEgressBufferBytes) {
      return false;
    }

    const std::uint32_t lenNet =
        htonl(static_cast<std::uint32_t>(payload.size()));
    const auto *lenBytes = reinterpret_cast<const std::uint8_t *>(&lenNet);
    conn->egressQueue.insert(conn->egressQueue.end(), lenBytes, lenBytes + 4);
    conn->egressQueue.insert(conn->egressQueue.end(), payload.begin(),
                             payload.end());

    if (conn->connected && conn->fd.isValid()) {
      flushEgress(conn);
    }
    return true;
  }

  void flushEgress(const std::shared_ptr<PeerConnection> &conn) {
    while (!conn->egressQueue.empty() && conn->fd.isValid()) {
      std::vector<std::uint8_t> chunk;
      const std::size_t n =
          std::min<std::size_t>(conn->egressQueue.size(), 65536);
      chunk.assign(conn->egressQueue.begin(), conn->egressQueue.begin() + n);

      ssize_t written =
          ::send(conn->fd.get(), chunk.data(), chunk.size(), MSG_NOSIGNAL);
      if (written > 0) {
        conn->egressQueue.erase(conn->egressQueue.begin(),
                                conn->egressQueue.begin() + written);
      } else if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        conn->connected = false;
        conn->fd.reset();
        break;
      }
    }
  }

  void handleLocalEmission(const runtime::Envelope &envelope) {
    owner_.emissions_.push_back(envelope);
    if (owner_.emissionCb_) {
      owner_.emissionCb_(envelope);
    }

    // Check forward rules
    for (const auto &rule : options_.forwardRules) {
      if (rule.eventType == envelope.type) {
        identity::ActorMessage msg;
        msg.type      = identity::MessageType::ActorSend;
        msg.requestId = options_.nodeId + ":" + envelope.type;
        msg.source    = identity::ActorPid{options_.nodeId, "launcher", 1, 1};
        msg.target =
            identity::ActorPid{rule.targetNode, rule.targetAgent, 1, 1};

        auto envBytes = runtime::encodeEnvelope(envelope);
        if (envBytes) {
          msg.value = *envBytes;
          (void)sendToPeer(rule.targetNode, msg);
        }
      }
    }
  }

  void acceptInbound() {
    while (listenFd_.isValid()) {
      sockaddr_in clientAddr{};
      socklen_t clientLen = sizeof(clientAddr);
      int clientSocket =
          ::accept(listenFd_.get(), reinterpret_cast<sockaddr *>(&clientAddr),
                   &clientLen);
      if (clientSocket < 0) {
        break;
      }

      setNonBlocking(clientSocket);
      setNoDelay(clientSocket);

      auto conn = std::make_shared<PeerConnection>();
      conn->fd.reset(clientSocket);
      conn->connected  = true;
      conn->isOutbound = false;
      connections_.push_back(conn);

      sendHandshake(conn);
    }
  }

  void processIngress(const std::shared_ptr<PeerConnection> &conn) {
    std::uint8_t buf[16384];
    while (conn->fd.isValid()) {
      ssize_t n = ::recv(conn->fd.get(), buf, sizeof(buf), 0);
      if (n > 0) {
        accumulateBytes(conn, std::span<const std::uint8_t>(buf, n));
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        break;
      } else {
        conn->connected = false;
        conn->fd.reset();
        break;
      }
    }
  }

  void accumulateBytes(const std::shared_ptr<PeerConnection> &conn,
                       std::span<const std::uint8_t> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      if (conn->ingressState == IngressState::ReadingHeader) {
        const std::size_t needed = 4 - conn->headerBuf.size();
        const std::size_t avail  = bytes.size() - offset;
        const std::size_t take   = std::min(needed, avail);
        conn->headerBuf.insert(conn->headerBuf.end(), bytes.begin() + offset,
                               bytes.begin() + offset + take);
        offset += take;

        if (conn->headerBuf.size() == 4) {
          std::uint32_t lenNet{};
          std::memcpy(&lenNet, conn->headerBuf.data(), 4);
          conn->expectedBodyLen = ntohl(lenNet);
          conn->headerBuf.clear();

          if (conn->expectedBodyLen == 0 ||
              conn->expectedBodyLen > kMaxClusterMessageBytes) {
            conn->connected = false;
            conn->fd.reset();
            return;
          }
          conn->bodyBuf.clear();
          conn->bodyBuf.reserve(conn->expectedBodyLen);
          conn->ingressState = IngressState::ReadingBody;
        }
      } else {
        const std::size_t needed = conn->expectedBodyLen - conn->bodyBuf.size();
        const std::size_t avail  = bytes.size() - offset;
        const std::size_t take   = std::min(needed, avail);
        conn->bodyBuf.insert(conn->bodyBuf.end(), bytes.begin() + offset,
                             bytes.begin() + offset + take);
        offset += take;

        if (conn->bodyBuf.size() == conn->expectedBodyLen) {
          dispatchFrame(conn, conn->bodyBuf);
          conn->bodyBuf.clear();
          conn->ingressState = IngressState::ReadingHeader;
        }
      }
    }
  }

  void dispatchFrame(const std::shared_ptr<PeerConnection> &conn,
                     const std::vector<std::uint8_t> &frame) {
    auto decoded = identity::decodeActorMessage(frame);
    if (!decoded) return;

    const auto &msg = *decoded;

    // Check if handshake
    if (msg.requestId == "VCLU_HANDSHAKE" && msg.source) {
      conn->nodeId            = msg.source->node;
      conn->handshakeReceived = true;

      auto existing = peersByNodeId_.find(conn->nodeId);
      if (existing != peersByNodeId_.end() && existing->second != conn) {
        if (existing->second->connected) {
          if (options_.nodeId < conn->nodeId) {
            conn->connected = false;
            conn->fd.reset();
            return;
          }
          existing->second->connected = false;
          existing->second->fd.reset();
        }
      }
      peersByNodeId_[conn->nodeId] = conn;

      identity::Fingerprint fp = conn->fingerprint;
      if (!fp.isValid()) {
        fp = defaultFingerprint();
      }
      owner_.host_->runtime().trustNode(conn->nodeId, fp);
      return;
    }

    // Try delivering envelope to local launched actor by name or single actor
    if (msg.target) {
      auto it = launchedActors_.find(msg.target->cursor);
      if (it == launchedActors_.end() && launchedActors_.size() == 1) {
        it = launchedActors_.begin();
      }
      if (it != launchedActors_.end()) {
        auto env = runtime::decodeEnvelope(msg.value);
        if (env) {
          (void)owner_.host_->send(it->second, std::move(*env));
          return;
        }
      }
    }

    // Deliver to local MeshActorRuntime
    const auto now                 = std::chrono::steady_clock::now();
    const auto wallNow             = std::chrono::system_clock::now();
    identity::Fingerprint senderFp = conn->fingerprint;
    if (!senderFp.isValid()) {
      senderFp = defaultFingerprint();
    }

    (void)owner_.host_->runtime().receive(senderFp, msg, now, wallNow);
  }

  std::size_t tick(std::chrono::milliseconds timeout) {
    if (!running_) return 0;

    std::vector<pollfd> pfds;
    if (listenFd_.isValid()) {
      pollfd pfd{};
      pfd.fd     = listenFd_.get();
      pfd.events = POLLIN;
      pfds.push_back(pfd);
    }

    const auto now = std::chrono::steady_clock::now();
    for (auto &conn : connections_) {
      if (conn->isOutbound && !conn->connected) {
        if (!conn->fd.isValid() &&
            now - conn->lastAttempt >= std::chrono::milliseconds(500)) {
          attemptConnect(conn);
        }
      }
      if (conn->fd.isValid()) {
        pollfd pfd{};
        pfd.fd     = conn->fd.get();
        pfd.events = POLLIN;
        if (!conn->connected || !conn->egressQueue.empty()) {
          pfd.events |= POLLOUT;
        }
        pfds.push_back(pfd);
      }
    }

    int rc =
        ::poll(pfds.data(), pfds.size(), static_cast<int>(timeout.count()));
    if (rc > 0) {
      std::size_t idx = 0;
      if (listenFd_.isValid()) {
        if (pfds[idx].revents & POLLIN) {
          acceptInbound();
        }
        ++idx;
      }

      for (auto &conn : connections_) {
        if (!conn->fd.isValid()) continue;
        if (idx >= pfds.size()) break;

        const auto rev = pfds[idx++].revents;
        if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
          conn->connected = false;
          conn->fd.reset();
          continue;
        }

        if (rev & POLLOUT) {
          if (!conn->connected) {
            int err       = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(conn->fd.get(), SOL_SOCKET, SO_ERROR, &err,
                             &len) == 0 &&
                err == 0) {
              conn->connected = true;
              sendHandshake(conn);
            } else {
              conn->fd.reset();
              continue;
            }
          }
          flushEgress(conn);
        }

        if (rev & POLLIN) {
          processIngress(conn);
        }
      }
    }

    // Pumping phase: bounded turns per tick
    std::size_t turns = 0;
    while (turns < kMaxTurnsPerPoll && owner_.host_->pump(1) > 0) {
      ++turns;
    }

    return turns;
  }

  std::size_t run(std::chrono::milliseconds maxDuration,
                  std::function<bool()> shouldExit) {
    const auto deadline    = std::chrono::steady_clock::now() + maxDuration;
    std::size_t totalTurns = 0;

    while (running_ && std::chrono::steady_clock::now() < deadline) {
      if (shouldExit && shouldExit()) {
        break;
      }
      totalTurns += tick(options_.pollTimeout);
    }
    return totalTurns;
  }

  [[nodiscard]] std::expected<identity::ActorPid,
                              zigzag::vortex::mesh::ActorError>
  launch(std::string_view agentType, std::string_view name,
         runtime::WireRecord args) {
    auto res = owner_.host_->launch(agentType, name, std::move(args));
    if (res) {
      launchedActors_[std::string(name)]      = *res;
      launchedActors_[std::string(agentType)] = *res;
    }
    return res;
  }

  [[nodiscard]] bool isRunning() const noexcept { return running_; }
  [[nodiscard]] std::uint16_t boundPort() const noexcept { return boundPort_; }

private:
  ClusterNode &owner_;
  Options options_;
  UniqueFd listenFd_;
  std::uint16_t boundPort_{0};
  bool running_{false};

  std::vector<std::shared_ptr<PeerConnection>> connections_;
  std::map<std::string, std::shared_ptr<PeerConnection>> peersByNodeId_;
  std::map<std::string, identity::ActorPid> launchedActors_;
};

ClusterNode::ClusterNode(std::shared_ptr<runtime::CompiledProgram> program,
                         Options options)
    : options_(std::move(options)) {
  impl_ = std::make_unique<Impl>(*this, std::move(program), options_);
}

ClusterNode::~ClusterNode() = default;

ClusterNode::ClusterNode(ClusterNode &&) noexcept            = default;
ClusterNode &ClusterNode::operator=(ClusterNode &&) noexcept = default;

ClusterNode *ClusterNode::setEmissionCallback(EmissionCallback cb) {
  emissionCb_ = std::move(cb);
  return this;
}

ClusterNode *ClusterNode::addPeer(std::string nodeId, std::string host,
                                  std::uint16_t port,
                                  identity::Fingerprint fingerprint) {
  options_.peers.push_back(PeerEndpoint{std::move(nodeId), std::move(host),
                                        port, std::move(fingerprint)});
  if (impl_ && impl_->isRunning()) {
    impl_->preparePeer(options_.peers.back());
  }
  return this;
}

ClusterNode *ClusterNode::addForwardRule(std::string eventType,
                                         std::string targetNode,
                                         std::string targetAgent) {
  options_.forwardRules.push_back(ForwardRule{
      std::move(eventType), std::move(targetNode), std::move(targetAgent)});
  return this;
}

std::expected<void, ClusterNodeError> ClusterNode::start() {
  return impl_->start();
}

void ClusterNode::stop() { impl_->stop(); }

std::expected<identity::ActorPid, zigzag::vortex::mesh::ActorError>
ClusterNode::launch(std::string_view agentType, std::string_view name,
                    runtime::WireRecord args) {
  return impl_->launch(agentType, name, std::move(args));
}

std::expected<void, zigzag::vortex::mesh::ActorError>
ClusterNode::send(const identity::ActorPid &target, runtime::Envelope event) {
  return host_->send(target, std::move(event));
}

std::size_t ClusterNode::run(std::chrono::milliseconds maxDuration,
                             std::function<bool()> shouldExit) {
  return impl_->run(maxDuration, std::move(shouldExit));
}

std::size_t ClusterNode::tick(std::chrono::milliseconds timeout) {
  return impl_->tick(timeout);
}

bool ClusterNode::isRunning() const noexcept {
  return impl_ && impl_->isRunning();
}

std::uint16_t ClusterNode::boundPort() const noexcept {
  return impl_ ? impl_->boundPort() : 0;
}

ClusterNode::Options
ClusterNode::Options::fromModel(const xanadu::SystemStoreModel &model) {
  Options options;
  options.hostOptions = runtime::LocalHost::Options::fromModel(model);
  return options;
}

ClusterNode::Options
ClusterNode::Options::fromStore(const xanadu::Store &store) {
  Options options;
  options.hostOptions = runtime::LocalHost::Options::fromStore(store);
  return options;
}

ClusterNode::Options ClusterNode::Options::fromManifold(
    const zigzag::Manifold &manifold,
    const std::optional<zigzag::CellRef> homeCell,
    const xanadu::SpanReader *reader) {
  Options options;
  options.hostOptions =
      runtime::LocalHost::Options::fromManifold(manifold, homeCell, reader);
  return options;
}

} // namespace xanadu::vagent::cluster
