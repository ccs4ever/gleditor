#ifndef COMMON_XANADU_VORTEX_MESH_SWARM_HPP
#define COMMON_XANADU_VORTEX_MESH_SWARM_HPP

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "common/xanadu/identity/identity_network_controller.hpp"
#include "common/xanadu/vortex/mesh_jobs.hpp"

namespace zigzag::vortex::mesh {

struct AuthenticatedActorEvent {
  xanadu::identity::Fingerprint identity;
  xanadu::identity::ActorMessage message;
};
struct JobAnnouncement {
  ImmutableJob job;
  CapabilityGrant grant;
};
struct MeshTransportOptions {
  xanadu::SwarmContentSource::Options swarm;
  std::filesystem::path downloads;
  /// Optional literal peer addresses for closed swarms; authentication and
  /// signed capability checks still apply to every computation.
  std::vector<std::pair<std::string, std::uint16_t>> peers;
  std::chrono::milliseconds metadataTimeout{60000};
  std::size_t maximumActorEvents{256};
  std::size_t maximumActorBytes{4 * 1024 * 1024};
};

struct QuicStreamFrame {
  std::uint8_t lane{};
  std::uint64_t streamId{};
  std::uint64_t offset{};
  bool fin{true};
  std::string payload;
};

[[nodiscard]] std::expected<std::string, std::string>
encodeQuicStreamFrame(const QuicStreamFrame &frame);

[[nodiscard]] std::expected<QuicStreamFrame, std::string>
decodeQuicStreamFrame(std::string_view packetBytes);

/// Owns one swarm listener for archival content, discovery and authenticated
/// actor frames, plus a dedicated ZeroMQ/QUIC-framed direct side-channel
/// authenticated by the node's Ed25519 DeviceDelegation keypair.
class MeshSwarmTransport {
public:
  MeshSwarmTransport(
      std::shared_ptr<xanadu::identity::IdentityNetworkController> identity,
      MeshTransportOptions options);
  ~MeshSwarmTransport();
  MeshSwarmTransport(const MeshSwarmTransport &)            = delete;
  MeshSwarmTransport &operator=(const MeshSwarmTransport &) = delete;

  /// Configure an ephemeral-listener peer before handing this transport to its
  /// owner. Configuration is closed once any owner method has been called.
  [[nodiscard]] std::expected<void, std::string>
  addPeerHint(std::string address, std::uint16_t port);
  [[nodiscard]] std::expected<void, std::string>
  addDirectPeer(const xanadu::identity::Fingerprint &peer, std::string address,
                std::uint16_t directPort);
  [[nodiscard]] std::uint16_t directPort() const noexcept {
    return directPort_;
  }
  [[nodiscard]] xanadu::SwarmContentSource &source() noexcept { return swarm_; }
  [[nodiscard]] const xanadu::ContentSource &verifiedSource() const noexcept {
    return sliceSource_;
  }
  [[nodiscard]] bool send(const xanadu::identity::Fingerprint &peer,
                          const xanadu::identity::ActorMessage &message);
  [[nodiscard]] bool sendBundle(const xanadu::identity::Fingerprint &peer,
                                std::string_view bundleBytes);
  [[nodiscard]] std::vector<AuthenticatedActorEvent> takeActorMessages();

  [[nodiscard]] std::expected<void, std::string>
  announce(const ImmutableJob &job, const CapabilityGrant &grant);
  /// Discovery is untrusted. Only FutureScheduler::submit after authenticated
  /// identity and grant verification can authorize any announced computation.
  [[nodiscard]] std::vector<JobAnnouncement> takeAnnouncements();
  [[nodiscard]] std::expected<void, std::string>
  prepare(const ImmutableJob &job, ValueLimits values,
          std::size_t maximumProgramBytes);
  [[nodiscard]] std::expected<void, std::string>
  prepareValue(const xanadu::InfoHash &hash, ValueLimits limits = {});
  [[nodiscard]] std::expected<void, std::string>
  ingestBundle(std::string_view bundleBytes, ValueLimits limits = {});
  [[nodiscard]] std::expected<void, std::string> seed(const ValueSeed &value);
  void poll();

private:
  struct Inbox {
    std::mutex mutex;
    std::deque<std::pair<AuthenticatedActorEvent, std::size_t>> events;
    std::size_t bytes{};
    std::size_t maximumEvents{};
    std::size_t maximumBytes{};
  };
  struct DirectChannelState;
  void requireOwner();
  void joinRendezvous();
  void introducePeers(const xanadu::InfoHash &hash);
  void pollDirectChannel();
  [[nodiscard]] bool sendDirectFrame(const xanadu::identity::Fingerprint &peer,
                                     std::uint8_t lane,
                                     std::string_view payload);
  [[nodiscard]] std::expected<void, std::string>
  materializeSliceToSwarm(const VerifiedTorrentSlice &slice);
  [[nodiscard]] std::expected<void, std::string>
  prepareTorrent(const xanadu::InfoHash &hash, std::uint64_t maximumBytes);

  std::shared_ptr<xanadu::identity::IdentityNetworkController> identity_;
  MeshTransportOptions options_;
  xanadu::SwarmContentSource swarm_;
  VerifiedSliceContentSource sliceSource_;
  std::shared_ptr<Inbox> inbox_;
  std::shared_ptr<DirectChannelState> direct_;
  std::uint16_t directPort_{0};
  std::optional<std::thread::id> owner_;
  bool joined_{};
};

} // namespace zigzag::vortex::mesh
#endif
