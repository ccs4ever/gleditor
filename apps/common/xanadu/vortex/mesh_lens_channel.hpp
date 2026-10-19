/**
 * @file mesh_lens_channel.hpp
 * @brief Distributed mesh channel connecting ProjectionSession to Spool-Mesh
 * over BEP 10 actor wire.
 *
 * Implements network-transparent proxy cells with 4-state asynchronous
 * resolution:
 * - ProxyCellState: Unmaterialized, Resolving, Active, Disconnected.
 * - Non-blocking UI access guaranteeing 120 FPS render loops never stall on
 * network lag.
 * - Speculative frontier expansion along dimensional momentum.
 */
#ifndef COMMON_XANADU_VORTEX_MESH_LENS_CHANNEL_HPP
#define COMMON_XANADU_VORTEX_MESH_LENS_CHANNEL_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/xanadu/vortex/spool_mesh.hpp"
#include "common/xanadu/zigzag/arena_manifold.hpp"
#include "common/xanadu/zigzag/dim_vector.hpp"

namespace zigzag::vortex::mesh {

/**
 * @brief 4-state lifecycle of a remote proxy cell.
 */
enum class ProxyCellState : std::uint8_t {
  Unmaterialized, // Local placeholder; content not yet requested
  Resolving,      // Request dispatched over Spool-Mesh; awaiting reply
  Active,         // Resolved; payload merged locally into arena
  Disconnected    // Timeout or partition; UI displays fallback without blocking
};

/**
 * @brief Metadata tracking a foreign proxy cell.
 */
struct ProxyCell {
  std::optional<CellRef> localCell{std::nullopt};
  NodeId remoteNode{};
  std::string remoteCellName{};
  ProxyCellState state{ProxyCellState::Unmaterialized};
  std::string content{};
  std::uint64_t requestTimestampMs{0};
  std::uint32_t retryCount{0};
};

/**
 * @class MeshLensChannel
 * @brief Distributed mesh channel connecting ProjectionSession to Spool-Mesh
 * over BEP 10 actor wire.
 *
 * Implements Milestone M4 Package D1:
 * - 4-state non-blocking proxy cell state machine.
 * - Non-blocking asynchronous resolution ensuring 120 FPS UI never stalls on
 * network delays.
 * - Directional speculative frontier expansion along dimension momentum.
 */
class MeshLensChannel {
public:
  MeshLensChannel(NodeId localNode, std::shared_ptr<MeshChannel> channel);

  /**
   * @brief Mint a local proxy cell standing for a remote cell on @p remoteNode.
   */
  CellRef createProxy(ArenaManifold &arena, const NodeId &remoteNode,
                      std::string_view remoteCellName);

  /**
   * @brief Asynchronously request materialization of @p proxyRef over the
   * channel.
   */
  void requestMaterialization(CellRef proxyRef, std::uint64_t nowMs = 0);

  /**
   * @brief Speculative frontier expansion: prefetches neighbours along @p dim
   * in direction @p dir.
   */
  void frontierExpand(ArenaManifold &arena, CellRef currentProxy, DimRef dim,
                      DimVector dir, std::uint32_t depth = 2,
                      std::uint64_t nowMs = 0);

  /**
   * @brief Poll incoming messages from the mesh channel and update proxy
   * states.
   * @return Number of resolved proxy cells.
   */
  std::size_t pollIncoming(ArenaManifold &arena);

  /**
   * @brief Check for timed out resolving requests and transition them to
   * Disconnected.
   * @return Number of timed out proxies.
   */
  std::size_t checkTimeouts(std::uint64_t nowMs, std::uint64_t timeoutMs = 500);

  /**
   * @brief Simulates serving requests from a peer: inspects channel for
   * requests directed to @p localNode and sends back responses using @p
   * provider.
   */
  std::size_t servePeerRequests(
      const std::function<std::optional<std::string>(std::string_view)>
          &provider);

  [[nodiscard]] std::optional<ProxyCellState> stateOf(CellRef proxyRef) const;
  [[nodiscard]] const ProxyCell *getProxy(CellRef proxyRef) const;
  [[nodiscard]] const NodeId &localNode() const noexcept { return localNode_; }
  [[nodiscard]] std::size_t proxyCount() const noexcept {
    return proxies_.size();
  }

private:
  NodeId localNode_;
  std::shared_ptr<MeshChannel> channel_;
  std::unordered_map<CellRef, ProxyCell> proxies_;
  std::unordered_map<std::string, CellRef>
      pendingRequests_; // "node:cellName" -> CellRef
};

} // namespace zigzag::vortex::mesh

#endif // COMMON_XANADU_VORTEX_MESH_LENS_CHANNEL_HPP
