/**
 * @file net_lens.hpp
 * @brief NetLens: Network Connections and Streams in ZigZag Space.
 *
 * Replaces Plan 9 /net/tcp with multidimensional cells and ranks.
 *
 * Invariants:
 * - Invariant A4: Connection prototypes are instantiated along d.instance,
 *   preserving d.clone strictly for master headcell identity clones.
 * - Endpoints link along d.endpoint.
 * - Connection lifecycle states link along d.state.
 * - Stream ring buffers carry high-throughput payloads bypassing cell storage.
 */
#ifndef ZIGZAG_NET_LENS_HPP
#define ZIGZAG_NET_LENS_HPP

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/zigzag/projection_lens.hpp"

namespace zigzag {

enum class ConnectionState : std::uint8_t {
  Closed,
  Listen,
  SynSent,
  SynReceived,
  Established,
  FinWait,
  CloseWait,
  TimeWait,
};

[[nodiscard]] inline std::string_view
connectionStateName(const ConnectionState s) noexcept {
  switch (s) {
  case ConnectionState::Closed:
    return "CLOSED";
  case ConnectionState::Listen:
    return "LISTEN";
  case ConnectionState::SynSent:
    return "SYN_SENT";
  case ConnectionState::SynReceived:
    return "SYN_RCVD";
  case ConnectionState::Established:
    return "ESTABLISHED";
  case ConnectionState::FinWait:
    return "FIN_WAIT";
  case ConnectionState::CloseWait:
    return "CLOSE_WAIT";
  case ConnectionState::TimeWait:
    return "TIME_WAIT";
  }
  return "UNKNOWN";
}

struct ConnectionSnapshot {
  std::uint32_t id{0};
  std::string localAddr{"0.0.0.0"};
  std::uint16_t localPort{0};
  std::string remoteAddr{"0.0.0.0"};
  std::uint16_t remotePort{0};
  ConnectionState state{ConnectionState::Closed};
  std::uint32_t ringBufferCapacity{65536};
};

/**
 * @class NetStreamRing
 * @brief Zero-allocation ring buffer for high-throughput stream transport.
 */
class NetStreamRing {
public:
  explicit NetStreamRing(std::size_t capacity = 65536);

  std::size_t write(std::span<const std::uint8_t> data);
  std::size_t read(std::span<std::uint8_t> out);

  [[nodiscard]] std::size_t availableRead() const noexcept;
  [[nodiscard]] std::size_t availableWrite() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }

private:
  std::vector<std::uint8_t> buf_;
  std::size_t cap_{0};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

/**
 * @class NetLens
 * @brief ProjectionLens exposing active network sockets as ZigZag cells.
 */
class NetLens : public ProjectionLens {
public:
  NetLens() = default;
  explicit NetLens(std::vector<ConnectionSnapshot> connections);

  [[nodiscard]] std::vector<std::pair<std::string, std::optional<GenericDim>>>
  dimensions() const override;

  void forEachNode(gleditor::cpp26::function_ref<void(const SourceNode &)> emit)
      const override;

  void forEachEdge(gleditor::cpp26::function_ref<void(const SourceEdge &)> emit)
      const override;

  /**
   * @brief Allocate a new connection instance off prototype along d.instance.
   */
  std::uint32_t allocateInstance(std::string localAddr,
                                 std::uint16_t localPort);

  /**
   * @brief Update connection state.
   */
  bool transitionState(std::uint32_t connId, ConnectionState newState);

  [[nodiscard]] NetStreamRing *getStreamRing(std::uint32_t connId);

private:
  std::vector<ConnectionSnapshot> connections_;
  std::vector<std::unique_ptr<NetStreamRing>> rings_;
};

} // namespace zigzag

#endif // ZIGZAG_NET_LENS_HPP
