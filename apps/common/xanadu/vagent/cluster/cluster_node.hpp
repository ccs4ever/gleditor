/**
 * @file cluster_node.hpp
 * @brief Concurrent cluster actor node with non-blocking TCP internode routing.
 */
#ifndef COMMON_XANADU_VAGENT_CLUSTER_CLUSTER_NODE_HPP
#define COMMON_XANADU_VAGENT_CLUSTER_CLUSTER_NODE_HPP

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/identity/identity_layout.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"

namespace xanadu::vagent::cluster {

enum class ClusterNodeError {
  Ok = 0,
  SocketCreationFailed,
  BindFailed,
  ListenFailed,
  ConnectionFailed,
  PeerNotFound,
  PeerDisconnected,
  FrameTooLarge,
  SerializationFailed,
  BufferOverflow,
  InvalidAddress,
  Timeout,
};

[[nodiscard]] std::string_view clusterNodeErrorName(ClusterNodeError err);

inline constexpr std::uint32_t kMaxClusterMessageBytes =
    64 * 1024 * 1024; // 64 MiB
inline constexpr std::size_t kMaxPeerEgressBufferBytes =
    16 * 1024 * 1024; // 16 MiB
inline constexpr std::size_t kMaxTurnsPerPoll = 16;

struct PeerEndpoint {
  std::string nodeId;
  std::string host;
  std::uint16_t port{0};
  identity::Fingerprint fingerprint{};
};

struct ForwardRule {
  std::string eventType;
  std::string targetNode;
  std::string targetAgent;
};

class ClusterNode {
public:
  struct Options {
    std::string nodeId{"local"};
    std::string listenHost{"0.0.0.0"};
    std::uint16_t listenPort{0};
    identity::Fingerprint identity{};
    std::vector<PeerEndpoint> peers;
    std::vector<ForwardRule> forwardRules;
    std::chrono::milliseconds connectTimeout{10000};
    std::chrono::milliseconds pollTimeout{25};
    runtime::LocalHost::Options hostOptions{};

    [[nodiscard]] static Options fromStore(const xanadu::Store &store);
    [[nodiscard]] static Options
    fromManifold(const zigzag::Manifold &manifold,
                 std::optional<zigzag::CellRef> homeCell = std::nullopt,
                 const xanadu::SpanReader *reader        = nullptr);
    [[nodiscard]] static Options
    fromModel(const xanadu::SystemStoreModel &model);
  };

  using EmissionCallback = std::function<void(const runtime::Envelope &)>;

  ClusterNode(std::shared_ptr<runtime::CompiledProgram> program,
              Options options);
  ~ClusterNode();

  ClusterNode(const ClusterNode &)            = delete;
  ClusterNode &operator=(const ClusterNode &) = delete;
  ClusterNode(ClusterNode &&) noexcept;
  ClusterNode &operator=(ClusterNode &&) noexcept;

  ClusterNode *setEmissionCallback(EmissionCallback cb);
  ClusterNode *addPeer(std::string nodeId, std::string host, std::uint16_t port,
                       identity::Fingerprint fingerprint = {});
  ClusterNode *addForwardRule(std::string eventType, std::string targetNode,
                              std::string targetAgent);

  [[nodiscard]] std::expected<void, ClusterNodeError> start();
  void stop();

  [[nodiscard]] std::expected<identity::ActorPid,
                              zigzag::vortex::mesh::ActorError>
  launch(std::string_view agentType, std::string_view name,
         runtime::WireRecord args = {});

  [[nodiscard]] std::expected<void, zigzag::vortex::mesh::ActorError>
  send(const identity::ActorPid &target, runtime::Envelope event);

  /// Runs the non-blocking event loop until maxDuration expires, stop() is
  /// called, or shouldExit predicate returns true.
  std::size_t run(std::chrono::milliseconds maxDuration,
                  std::function<bool()> shouldExit = nullptr);

  /// Performs a single non-blocking event poll tick and bounded pump.
  std::size_t
  tick(std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  [[nodiscard]] bool isRunning() const noexcept;
  [[nodiscard]] std::uint16_t boundPort() const noexcept;
  [[nodiscard]] const Options &options() const noexcept { return options_; }
  [[nodiscard]] runtime::LocalHost &host() noexcept { return *host_; }
  [[nodiscard]] const std::vector<runtime::Envelope> &
  emissions() const noexcept {
    return emissions_;
  }

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  Options options_;
  std::unique_ptr<runtime::LocalHost> host_;
  std::vector<runtime::Envelope> emissions_;
  EmissionCallback emissionCb_;
};

} // namespace xanadu::vagent::cluster

#endif // COMMON_XANADU_VAGENT_CLUSTER_CLUSTER_NODE_HPP
