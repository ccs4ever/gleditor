#ifndef COMMON_XANADU_VORTEX_MESH_CHECKPOINT_HPP
#define COMMON_XANADU_VORTEX_MESH_CHECKPOINT_HPP

#include "common/xanadu/vortex/mesh_actor.hpp"
#include "common/xanadu/vortex/mesh_swarm.hpp"

namespace zigzag::vortex::mesh {

[[nodiscard]] std::expected<std::string, std::string>
encodeContinuation(const ActorContinuation &continuation);
[[nodiscard]] std::expected<ActorContinuation, std::string>
decodeContinuation(std::string_view bytes, std::size_t maximumRecords = 4096);

struct CheckpointBackend {
  std::function<std::expected<void, std::string>(
      const xanadu::MutableKeys &, const std::string &,
      const xanadu::InfoHash &, std::int64_t)>
      publish;
  std::function<bool(const xanadu::PublicKey &, const std::string &,
                     const xanadu::InfoHash &, std::int64_t)>
      acknowledged;
};
[[nodiscard]] CheckpointBackend
checkpointBackend(xanadu::SwarmContentSource &swarm);

enum class CheckpointState { AwaitingAcknowledgement, Committed };
struct CheckpointStatus {
  xanadu::MutableLink name;
  xanadu::InfoHash hash;
  std::int64_t sequence;
  CheckpointState state;
};

/// Called on the network owner, never the render thread. Publication is pending
/// until the exact signed DHT put is acknowledged; local-cache reads are
/// unused.
class CheckpointPublisher {
public:
  explicit CheckpointPublisher(CheckpointBackend backend);
  [[nodiscard]] std::expected<CheckpointStatus, std::string>
  begin(const PublishedValue &value, const xanadu::InfoHash &seededCarrier,
        const xanadu::MutableKeys &keys, std::string salt,
        std::int64_t previousSequence);
  [[nodiscard]] std::optional<CheckpointStatus> poll();

private:
  CheckpointBackend backend_;
  std::optional<CheckpointStatus> current_;
};

struct ResumedCheckpoint {
  ReceivedValue state;
  ActorContinuation continuation;
  std::int64_t sequence;
};
/// Resolves the signed mutable pointer afresh and verifies all sealed state.
/// Actor reactivation must still pass the new host's capability admission.
[[nodiscard]] std::expected<ResumedCheckpoint, std::string>
resolveCheckpoint(const xanadu::MutableLink &name,
                  MeshSwarmTransport &transport,
                  std::shared_ptr<xanadu::UserPermascroll> reader,
                  std::chrono::milliseconds timeout, ValueLimits limits = {});

} // namespace zigzag::vortex::mesh
#endif
