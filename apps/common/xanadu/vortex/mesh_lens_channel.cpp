#include "common/xanadu/vortex/mesh_lens_channel.hpp"
#include <expected>
#include <stdexcept>

#include "common/xanadu/bencode.hpp"

namespace zigzag::vortex::mesh {

namespace {

namespace bc = xanadu::bencode;

// Frames are bencode lists, the encoding this mesh's envelope already uses
// (spool_mesh.cpp): byte strings are length-prefixed, so a name or value may
// hold any byte, '|' included, and nothing is split on a delimiter.
constexpr std::string_view kRequestTag  = "LENS_REQ";
constexpr std::string_view kResponseTag = "LENS_RESP";
constexpr std::size_t kRequestFields    = 4; // tag, node, cell name, proxy ref
constexpr std::size_t kResponseFields   = 4; // tag, node, cell name, content

[[nodiscard]] std::string frameHead(const std::string_view tag) {
  return "l" + std::to_string(tag.size()) + ":" + std::string(tag);
}

[[nodiscard]] bool hasTag(const std::string_view payload,
                          const std::string_view tag) {
  return payload.starts_with(frameHead(tag));
}

[[nodiscard]] std::string encodeFrame(bc::List fields) {
  return bc::Value::list(std::move(fields)).encode();
}

[[nodiscard]] std::string pendingKey(const std::string_view node,
                                     const std::string_view cellName) {
  return encodeFrame({bc::Value::string(std::string(node)),
                      bc::Value::string(std::string(cellName))});
}

/// A frame whose tag is ours must parse exactly -- right field count and
/// kinds, nothing trailing -- or it is refused by number, never read short.
[[nodiscard]] std::expected<bc::List, MeshError>
parseFrame(const std::string_view payload, const std::size_t fields,
           const bc::Kind lastFieldKind) {
  try {
    const auto value = bc::decode(payload);
    if (!value.isList()) return std::unexpected(MeshError::InvalidFields);
    const auto &items = value.asList();
    if (items.size() != fields)
      return std::unexpected(MeshError::InvalidFields);
    for (std::size_t i = 0; i + 1 < fields; ++i) {
      if (!items[i].isString()) {
        return std::unexpected(MeshError::InvalidFields);
      }
    }
    if (items.back().kind() != lastFieldKind) {
      return std::unexpected(MeshError::InvalidFields);
    }
    return items;
  } catch (const std::runtime_error &) {
    return std::unexpected(MeshError::InvalidBencode);
  }
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

  pendingRequests_[pendingKey(proxy.remoteNode.str(), proxy.remoteCellName)] =
      proxyRef;

  if (channel_) {
    const std::string payload =
        encodeFrame({bc::Value::string(std::string(kRequestTag)),
                     bc::Value::string(proxy.remoteNode.str()),
                     bc::Value::string(proxy.remoteCellName),
                     bc::Value::integer(static_cast<std::int64_t>(proxyRef))});
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
    if (hasTag(msg.payload, kResponseTag)) {
      const auto frame =
          parseFrame(msg.payload, kResponseFields, bc::Kind::String);
      if (!frame) {
        noteRefusal(frame.error());
        continue;
      }
      const auto key =
          pendingKey((*frame)[1].asString(), (*frame)[2].asString());
      if (const auto reqIt = pendingRequests_.find(key);
          reqIt != pendingRequests_.end()) {
        const auto proxyRef = reqIt->second;
        pendingRequests_.erase(reqIt);
        // Only a proxy still waiting becomes Active: a Disconnected one stays
        // so, whatever arrives late.
        if (const auto pIt = proxies_.find(proxyRef);
            pIt != proxies_.end() &&
            pIt->second.state == ProxyCellState::Resolving) {
          pIt->second.state   = ProxyCellState::Active;
          pIt->second.content = (*frame)[3].asString();
          ++resolved;
        }
        continue;
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
      pendingRequests_.erase(
          pendingKey(proxy.remoteNode.str(), proxy.remoteCellName));
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
    if (hasTag(msg.payload, kRequestTag)) {
      const auto frame =
          parseFrame(msg.payload, kRequestFields, bc::Kind::Integer);
      if (!frame) {
        noteRefusal(frame.error());
        continue;
      }
      const std::string &targetNodeStr = (*frame)[1].asString();
      const std::string &cellName      = (*frame)[2].asString();
      if (targetNodeStr == localNode_.str()) {
        if (auto contentOpt = provider(cellName); contentOpt.has_value()) {
          channel_->send(MeshMessage::value(
              localNode_,
              encodeFrame({bc::Value::string(std::string(kResponseTag)),
                           bc::Value::string(localNode_.str()),
                           bc::Value::string(cellName),
                           bc::Value::string(std::move(*contentOpt))})));
          ++answered;
          continue;
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

void MeshLensChannel::noteRefusal(const MeshError error) noexcept {
  ++refusedFrames_;
  lastRefusal_ = error;
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
