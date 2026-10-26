#include "common/xanadu/vortex/mesh_lens_channel.hpp"
#include <sstream>

namespace zigzag::vortex::mesh {

namespace {

std::vector<std::string> split(std::string_view s, char delim) {
  std::vector<std::string> elems;
  std::stringstream ss{std::string(s)};
  std::string item;
  while (std::getline(ss, item, delim)) {
    elems.push_back(item);
  }
  return elems;
}

} // namespace

MeshLensChannel::MeshLensChannel(NodeId localNode,
                                 std::shared_ptr<MeshChannel> channel)
    : localNode_(std::move(localNode)), channel_(std::move(channel)) {}

CellRef MeshLensChannel::createProxy(ArenaManifold &arena,
                                     const NodeId &remoteNode,
                                     std::string_view remoteCellName) {
  const auto cell = arena.makeCell();
  ProxyCell proxy{};
  proxy.localCell      = cell;
  proxy.remoteNode     = remoteNode;
  proxy.remoteCellName = std::string(remoteCellName);
  proxy.state          = ProxyCellState::Unmaterialized;
  proxies_[cell]       = std::move(proxy);
  return cell;
}

void MeshLensChannel::requestMaterialization(CellRef proxyRef,
                                             std::uint64_t nowMs) {
  auto it = proxies_.find(proxyRef);
  if (it == proxies_.end()) {
    return;
  }

  auto &proxy = it->second;
  if (proxy.state == ProxyCellState::Active) {
    return;
  }

  proxy.state              = ProxyCellState::Resolving;
  proxy.requestTimestampMs = nowMs;

  const std::string key = proxy.remoteNode.str() + ":" + proxy.remoteCellName;
  pendingRequests_[key] = proxyRef;

  if (channel_) {
    const std::string payload = "LENS_REQ|" + proxy.remoteNode.str() + "|" +
                                proxy.remoteCellName + "|" +
                                std::to_string(proxyRef);
    channel_->send(MeshMessage::value(localNode_, payload));
  }
}

void MeshLensChannel::frontierExpand(ArenaManifold &arena, CellRef currentProxy,
                                     DimRef dim, DimVector dir,
                                     std::uint32_t depth, std::uint64_t nowMs) {
  CellRef curr = currentProxy;
  for (std::uint32_t d = 0; d < depth; ++d) {
    const auto nbr = arena.linked(curr, dim, dir);
    if (!nbr.has_value() || *nbr == curr) {
      break;
    }
    const auto it = proxies_.find(*nbr);
    if (it != proxies_.end() &&
        it->second.state == ProxyCellState::Unmaterialized) {
      requestMaterialization(*nbr, nowMs);
    }
    curr = *nbr;
  }
}

std::size_t MeshLensChannel::pollIncoming(ArenaManifold &) {
  if (!channel_) {
    return 0;
  }

  std::size_t resolved = 0;
  std::vector<MeshMessage> unhandled;

  while (auto msgOpt = channel_->receive()) {
    const auto &msg = *msgOpt;
    if (msg.payload.starts_with("LENS_RESP|")) {
      const auto parts = split(msg.payload, '|');
      if (parts.size() >= 4) {
        const std::string &originStr = parts[1];
        const std::string &cellName  = parts[2];
        const std::string &content   = parts[3];

        const std::string key = originStr + ":" + cellName;
        auto reqIt            = pendingRequests_.find(key);
        if (reqIt != pendingRequests_.end()) {
          const auto proxyRef = reqIt->second;
          auto pIt            = proxies_.find(proxyRef);
          if (pIt != proxies_.end()) {
            pIt->second.state   = ProxyCellState::Active;
            pIt->second.content = content;
            ++resolved;
          }
          pendingRequests_.erase(reqIt);
          continue;
        }
      }
    }
    unhandled.push_back(msg);
  }

  // Restore unhandled messages for other listeners
  for (const auto &m : unhandled) {
    channel_->send(m);
  }

  return resolved;
}

std::size_t MeshLensChannel::checkTimeouts(std::uint64_t nowMs,
                                           std::uint64_t timeoutMs) {
  std::size_t timedOut = 0;
  for (auto &[cell, proxy] : proxies_) {
    if (proxy.state == ProxyCellState::Resolving &&
        nowMs >= proxy.requestTimestampMs &&
        (nowMs - proxy.requestTimestampMs) >= timeoutMs) {
      proxy.state   = ProxyCellState::Disconnected;
      proxy.content = "[Disconnected: " + proxy.remoteCellName + "]";
      ++timedOut;
    }
  }
  return timedOut;
}

std::size_t MeshLensChannel::servePeerRequests(
    gleditor::cpp26::function_ref<std::optional<std::string>(std::string_view)>
        provider) {
  if (!channel_) {
    return 0;
  }

  std::size_t answered = 0;
  std::vector<MeshMessage> unhandled;

  while (auto msgOpt = channel_->receive()) {
    const auto &msg = *msgOpt;
    if (msg.payload.starts_with("LENS_REQ|")) {
      const auto parts = split(msg.payload, '|');
      if (parts.size() >= 4) {
        const std::string &targetNodeStr = parts[1];
        const std::string &cellName      = parts[2];

        if (targetNodeStr == localNode_.str()) {
          auto contentOpt = provider(cellName);
          if (contentOpt.has_value()) {
            const std::string respPayload = "LENS_RESP|" + localNode_.str() +
                                            "|" + cellName + "|" + *contentOpt;
            channel_->send(MeshMessage::value(localNode_, respPayload));
            ++answered;
            continue;
          }
        }
      }
    }
    unhandled.push_back(msg);
  }

  for (const auto &m : unhandled) {
    channel_->send(m);
  }

  return answered;
}

std::optional<ProxyCellState> MeshLensChannel::stateOf(CellRef proxyRef) const {
  auto it = proxies_.find(proxyRef);
  if (it != proxies_.end()) {
    return it->second.state;
  }
  return std::nullopt;
}

const ProxyCell *MeshLensChannel::getProxy(CellRef proxyRef) const {
  auto it = proxies_.find(proxyRef);
  if (it != proxies_.end()) {
    return &it->second;
  }
  return nullptr;
}

} // namespace zigzag::vortex::mesh
