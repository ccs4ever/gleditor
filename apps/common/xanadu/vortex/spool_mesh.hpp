/**
 * @file spool_mesh.hpp
 * @brief Spool-Mesh: multiprocess / network-transparent Vortex distribution
 *        (design/vortex-distribution.md). Slice 0 -- the loopback foundation.
 *
 * Spool-Mesh is a mesh of share-nothing arenas that communicate ONLY by
 * content-addressed, immutable messages. It is the single-machine arena-as-node
 * discipline of the multithreading study restretched toward the network, with
 * one decisive safety property: THERE IS NO REMOTE MUTATION. A node never
 * reaches into another node's arena. It receives an immutable value and merges
 * it locally by minting its own cells. Branch-not-overwrite is the merge rule.
 *
 * Slice 0 proves the message-passing discipline end to end over an in-memory
 * channel, before any network byte:
 *   produce (seal a value in node A's arena)
 *     -> content-address it (sha256) into an immutable MeshMessage
 *       -> hand it over a MeshChannel (the "wire", here a queue)
 *         -> node B receives, deduplicates on content id (effectively-once),
 *            and MERGES LOCALLY (mints its own cell) -- never touching A.
 *
 * The channel is an interface so Slice 1 (a content-addressed Future over the
 * same channel) and Slice 2 (a real BEP10 wire extension) swap the transport
 * without changing the node or the message. The GlobalSpan/globalise/localise
 * path (publication.hpp) is the production carrier for a span that lives in a
 * published scroll; Slice 0 carries the payload bytes directly so the loopback
 * round-trip is testable without standing up a full permascroll and swarm.
 *
 * This is a design-grounded first increment, not the whole distribution layer:
 * no socket, no DHT, no PIDs, no supervision yet (those are Slices 1-2).
 */
#ifndef COMMON_XANADU_VORTEX_SPOOL_MESH_HPP
#define COMMON_XANADU_VORTEX_SPOOL_MESH_HPP

#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/worker_context.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag::vortex::mesh {

/// A node's stable identity on the mesh: an author key, a device key, and a
/// monotonic arena epoch the device bumps on restart (design: node =
/// (authorKey, deviceKey, arenaEpoch)). Slice 0 uses plain strings; the keys
/// become real BEP46/device keys in a later slice. Equality + hashing let a
/// node tell its own messages from a peer's.
struct NodeId {
  std::string authorKey;
  std::string deviceKey;
  std::uint64_t arenaEpoch{0};

  [[nodiscard]] std::string str() const;
  bool operator==(const NodeId &) const = default;
};

/// Refusals at the untrusted message boundary.
enum class MeshError {
  InvalidBencode,
  InvalidFields,
  InvalidIdentity,
  InvalidKind,
  ContentMismatch,
  PayloadOverflow,
};

/// What a message carries. Slice 0 has one payload kind -- an immutable value
/// (a run of text, as a node would seal a cell's content). The variant is
/// named so Slice 1 adds a Future request/result and Slice 2 adds actor
/// control messages without reshaping the envelope.
enum class MeshPayloadKind : std::uint8_t {
  Value, ///< an immutable content value the receiver merges locally
};

/// A content-addressed, immutable message. The @p contentId is the sha256 of
/// the payload, so two independently-produced identical messages collide on id
/// and a receiver can deduplicate (at-least-once + content dedup =
/// effectively-once, the only honest guarantee over a churny transport). A
/// message names its origin node but carries NO arena-local CellRef -- a bare
/// ref is meaningless off its arena, so only the content crosses.
struct MeshMessage {
  NodeId origin;
  MeshPayloadKind kind{MeshPayloadKind::Value};
  std::string payload;   ///< the immutable bytes (a sealed value)
  std::string contentId; ///< sha256Hex(payload); the dedup + merge key

  [[nodiscard]] std::expected<void, MeshError> validate() const;

  /// Build a Value message, computing the content id from the payload.
  [[nodiscard]] static MeshMessage value(NodeId origin, std::string payload);
};

/// A bounded, versioned envelope. The content id addresses the value, while
/// origin remains untrusted until the transport authenticates its device key.
[[nodiscard]] std::expected<std::string, MeshError>
encodeMessage(const MeshMessage &message);
[[nodiscard]] std::expected<MeshMessage, MeshError>
decodeMessage(std::string_view bytes);

/// The transport seam. Slice 0's implementation is an in-memory loopback queue;
/// Slice 2 replaces it with a BEP10 wire extension. A channel moves immutable
/// messages between nodes and never exposes a node's arena to another.
class MeshChannel {
public:
  virtual ~MeshChannel() = default;

  /// Enqueue @p msg for delivery to the peer. Non-blocking; the transport may
  /// deliver more than once (the receiver dedups), never corrupt or reorder a
  /// single message's bytes.
  virtual void send(const MeshMessage &msg) = 0;

  /// Take the next undelivered message, or nullopt when the queue is empty.
  [[nodiscard]] virtual std::optional<MeshMessage> receive() = 0;

  /// Messages still waiting to be received.
  [[nodiscard]] virtual std::size_t pending() const = 0;
};

/// An in-memory loopback channel: Slice 0's "wire". A single queue shared by
/// two endpoints via a shared_ptr; send() pushes, receive() pops. Deliberately
/// trivial -- it exists to prove the node/message discipline, not the network.
class LoopbackChannel final : public MeshChannel {
public:
  void send(const MeshMessage &msg) override;
  [[nodiscard]] std::optional<MeshMessage> receive() override;
  [[nodiscard]] std::size_t pending() const override { return queue_.size(); }

  /// Re-enqueue a message that was already delivered, to exercise the
  /// receiver's content dedup (models an at-least-once transport redelivery).
  void redeliver(const MeshMessage &msg) { queue_.push_back(msg); }

private:
  std::deque<MeshMessage> queue_;
};

/// One Spool-Mesh node: a WorkerContext (its own arena/core/vm/stdlib -- a
/// share-nothing node) plus its identity and the set of content ids it has
/// already merged (for idempotent dedup). A node seals values from its arena
/// into messages and merges received messages into its arena by minting new
/// cells -- never by accepting a foreign ref.
class MeshNode {
public:
  explicit MeshNode(NodeId id, const Manifold *base = nullptr);

  [[nodiscard]] const NodeId &id() const noexcept { return id_; }
  [[nodiscard]] WorkerContext &context() noexcept { return *ctx_; }

  /// Seal @p cell's text content into an immutable, content-addressed message
  /// ready to send. Reads the arena; mutates nothing. The resulting message is
  /// safe to hand any peer because it carries bytes, not a ref.
  [[nodiscard]] MeshMessage seal(CellRef cell) const;

  /// Merge @p msg into THIS node's arena by minting a fresh cell holding the
  /// message's payload, and return that new local CellRef. This is the
  /// no-remote-mutation invariant in code: the receiver creates its own cell;
  /// the sender's arena is never touched. A message whose content id was
  /// already merged is a no-op and returns the existing local cell (dedup).
  /// Invalid envelopes throw std::invalid_argument before any arena mutation.
  CellRef merge(const MeshMessage &msg);
  [[nodiscard]] std::expected<CellRef, MeshError>
  tryMerge(const MeshMessage &msg);

  /// Whether this node has already merged a message with @p contentId.
  [[nodiscard]] bool hasMerged(const std::string &contentId) const;

  /// How many distinct messages this node has merged.
  [[nodiscard]] std::size_t mergedCount() const noexcept {
    return merged_.size();
  }

private:
  NodeId id_;
  std::unique_ptr<WorkerContext> ctx_;
  // content id -> the local cell minted when it was first merged (dedup map).
  std::unordered_map<std::string, CellRef> merged_;
};

} // namespace zigzag::vortex::mesh

#endif // COMMON_XANADU_VORTEX_SPOOL_MESH_HPP
