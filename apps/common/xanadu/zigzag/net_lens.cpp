/**
 * @file net_lens.cpp
 * @brief Implementation of NetLens and NetStreamRing.
 */
#include "common/xanadu/zigzag/net_lens.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace zigzag {

NetStreamRing::NetStreamRing(const std::size_t capacity)
    : buf_(capacity), cap_(capacity) {}

std::size_t NetStreamRing::availableRead() const noexcept { return size_; }

std::size_t NetStreamRing::availableWrite() const noexcept {
  return cap_ - size_;
}

std::size_t NetStreamRing::write(const std::span<const std::uint8_t> data) {
  const std::size_t toWrite = std::min(data.size(), availableWrite());
  if (toWrite == 0) {
    return 0;
  }

  const std::size_t firstPart = std::min(toWrite, cap_ - head_);
  std::memcpy(buf_.data() + head_, data.data(), firstPart);

  const std::size_t secondPart = toWrite - firstPart;
  if (secondPart > 0) {
    std::memcpy(buf_.data(), data.data() + firstPart, secondPart);
  }

  head_ = (head_ + toWrite) % cap_;
  size_ += toWrite;
  return toWrite;
}

std::size_t NetStreamRing::read(const std::span<std::uint8_t> out) {
  const std::size_t toRead = std::min(out.size(), availableRead());
  if (toRead == 0) {
    return 0;
  }

  const std::size_t firstPart = std::min(toRead, cap_ - tail_);
  std::memcpy(out.data(), buf_.data() + tail_, firstPart);

  const std::size_t secondPart = toRead - firstPart;
  if (secondPart > 0) {
    std::memcpy(out.data() + firstPart, buf_.data(), secondPart);
  }

  tail_ = (tail_ + toRead) % cap_;
  size_ -= toRead;
  return toRead;
}

NetLens::NetLens(std::vector<ConnectionSnapshot> connections)
    : connections_(std::move(connections)) {
  for (const auto &c : connections_) {
    rings_.push_back(std::make_unique<NetStreamRing>(c.ringBufferCapacity));
  }
}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
NetLens::dimensions() const {
  return {
      {"d.instance", GenericDim::Sequence}, // Prototype -> Conn1 -> Conn2 ...
      {"d.endpoint", std::nullopt}, // Conn -> localAddr:port -> remoteAddr:port
      {"d.state", std::nullopt},    // Conn -> State
      {"d.stream", std::nullopt},   // Conn -> RingBuffer handle
  };
}

void NetLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  // Prototype node
  emit(SourceNode{
      .key        = "net:tcp_proto",
      .content    = "tcp_proto",
      .attributes = {{"d.role", "proto"}},
      .order      = 0,
  });

  for (const auto &c : connections_) {
    const std::string key      = std::format("net:conn_{}", c.id);
    const std::string localStr = std::format("{}:{}", c.localAddr, c.localPort);
    const std::string remoteStr =
        std::format("{}:{}", c.remoteAddr, c.remotePort);

    emit(SourceNode{
        .key     = key,
        .content = localStr + " -> " + remoteStr,
        .attributes =
            {
                {"d.state", std::string(connectionStateName(c.state))},
                {"d.local", localStr},
                {"d.remote", remoteStr},
            },
        .order = c.id,
    });
  }
}

void NetLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  if (connections_.empty()) {
    return;
  }

  // Prototype links to first instance along d.instance
  std::string prevKey = "net:tcp_proto";
  for (const auto &c : connections_) {
    const std::string currKey = std::format("net:conn_{}", c.id);
    emit(SourceEdge{
        .from      = prevKey,
        .to        = currKey,
        .dimension = "d.instance",
        .dir       = DimVector::POS,
    });
    prevKey = currKey;
  }
}

std::uint32_t NetLens::allocateInstance(std::string localAddr,
                                        const std::uint16_t localPort) {
  const auto newId = static_cast<std::uint32_t>(connections_.size() + 1);
  connections_.push_back(ConnectionSnapshot{
      .id                 = newId,
      .localAddr          = std::move(localAddr),
      .localPort          = localPort,
      .remoteAddr         = "0.0.0.0",
      .remotePort         = 0,
      .state              = ConnectionState::Listen,
      .ringBufferCapacity = 65536,
  });
  rings_.push_back(std::make_unique<NetStreamRing>(65536));
  return newId;
}

bool NetLens::transitionState(const std::uint32_t connId,
                              const ConnectionState newState) {
  const auto it = std::find_if(
      connections_.begin(), connections_.end(),
      [connId](const ConnectionSnapshot &c) { return c.id == connId; });
  if (it == connections_.end()) {
    return false;
  }
  it->state = newState;
  return true;
}

NetStreamRing *NetLens::getStreamRing(const std::uint32_t connId) {
  for (std::size_t i = 0; i < connections_.size(); ++i) {
    if (connections_[i].id == connId && i < rings_.size()) {
      return rings_[i].get();
    }
  }
  return nullptr;
}

} // namespace zigzag
