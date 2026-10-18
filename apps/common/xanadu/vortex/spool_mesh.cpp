/**
 * @file spool_mesh.cpp
 * @brief Implementation of the Spool-Mesh distribution core (Slice 0).
 */
#include "common/xanadu/vortex/spool_mesh.hpp"

#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_layout.hpp"

#include "common/xanadu/provenance.hpp"

namespace zigzag::vortex::mesh {

std::string NodeId::str() const {
  return std::format("{}:{}:{}", authorKey, deviceKey, arenaEpoch);
}

MeshMessage MeshMessage::value(NodeId origin, std::string payload) {
  MeshMessage m;
  m.origin    = std::move(origin);
  m.kind      = MeshPayloadKind::Value;
  m.contentId = xanadu::sha256Hex(payload); // content address = dedup/merge key
  m.payload   = std::move(payload);
  return m;
}

std::expected<void, MeshError> MeshMessage::validate() const {
  if (origin.authorKey.empty() || origin.deviceKey.empty() ||
      origin.arenaEpoch >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    return std::unexpected(MeshError::InvalidIdentity);
  if (kind != MeshPayloadKind::Value)
    return std::unexpected(MeshError::InvalidKind);
  if (payload.size() > xanadu::identity::kMaxPayloadBytes)
    return std::unexpected(MeshError::PayloadOverflow);
  if (contentId != xanadu::sha256Hex(payload))
    return std::unexpected(MeshError::ContentMismatch);
  return {};
}

std::expected<std::string, MeshError>
encodeMessage(const MeshMessage &message) {
  if (auto valid = message.validate(); !valid)
    return std::unexpected(valid.error());
  using V = xanadu::bencode::Value;
  auto bytes =
      V::dict({
                  {"author", V::string(message.origin.authorKey)},
                  {"device", V::string(message.origin.deviceKey)},
                  {"epoch", V::integer(static_cast<std::int64_t>(
                                message.origin.arenaEpoch))},
                  {"hash", V::string(message.contentId)},
                  {"kind", V::integer(static_cast<std::int64_t>(message.kind))},
                  {"payload", V::string(message.payload)},
                  {"version", V::integer(1)},
              })
          .encode();
  if (bytes.size() > xanadu::identity::kMaxPayloadBytes)
    return std::unexpected(MeshError::PayloadOverflow);
  return bytes;
}

std::expected<MeshMessage, MeshError> decodeMessage(std::string_view bytes) {
  if (bytes.size() > xanadu::identity::kMaxPayloadBytes)
    return std::unexpected(MeshError::PayloadOverflow);
  try {
    const auto value   = xanadu::bencode::decode(bytes);
    const auto author  = value.find("author");
    const auto device  = value.find("device");
    const auto epoch   = value.find("epoch");
    const auto hash    = value.find("hash");
    const auto kind    = value.find("kind");
    const auto payload = value.find("payload");
    const auto version = value.find("version");
    if (!value.isDict() || value.asDict().size() != 7 || !author ||
        !author->isString() || !device || !device->isString() || !epoch ||
        !epoch->isInteger() || epoch->asInteger() < 0 || !hash ||
        !hash->isString() || !kind || !kind->isInteger() || !payload ||
        !payload->isString() || !version || !version->isInteger() ||
        version->asInteger() != 1)
      return std::unexpected(MeshError::InvalidFields);
    if (kind->asInteger() != static_cast<std::int64_t>(MeshPayloadKind::Value))
      return std::unexpected(MeshError::InvalidKind);
    MeshMessage message{
        .origin    = {author->asString(), device->asString(),
                      static_cast<std::uint64_t>(epoch->asInteger())},
        .kind      = MeshPayloadKind::Value,
        .payload   = payload->asString(),
        .contentId = hash->asString()};
    if (auto valid = message.validate(); !valid)
      return std::unexpected(valid.error());
    return message;
  } catch (const std::runtime_error &) {
    return std::unexpected(MeshError::InvalidBencode);
  }
}

void LoopbackChannel::send(const MeshMessage &msg) { queue_.push_back(msg); }

std::optional<MeshMessage> LoopbackChannel::receive() {
  if (queue_.empty()) return std::nullopt;
  MeshMessage front = std::move(queue_.front());
  queue_.pop_front();
  return front;
}

MeshNode::MeshNode(NodeId id, const Manifold *base)
    : id_(std::move(id)), ctx_(std::make_unique<WorkerContext>(base)) {}

MeshMessage MeshNode::seal(CellRef cell) const {
  // Read the cell's content as bytes (what a node would publish). The message
  // carries the bytes, never the CellRef -- a ref is meaningless off this
  // arena.
  std::string text = ctx_->arena.textOf(cell);
  return MeshMessage::value(id_, std::move(text));
}

CellRef MeshNode::merge(const MeshMessage &msg) {
  auto result = tryMerge(msg);
  if (!result) {
    throw std::invalid_argument(std::format("Spool-Mesh: invalid message ({})",
                                            static_cast<int>(result.error())));
  }
  return *result;
}

std::expected<CellRef, MeshError> MeshNode::tryMerge(const MeshMessage &msg) {
  // Validate even redeliveries: an attacker must not smuggle altered content
  // behind an id already present in the receiver's cache.
  if (auto valid = encodeMessage(msg); !valid)
    return std::unexpected(valid.error());
  if (auto it = merged_.find(msg.contentId); it != merged_.end())
    return it->second;
  const auto local = ctx_->arena.makeCell(std::string_view{msg.payload});
  merged_.emplace(msg.contentId, local);
  return local;
}

bool MeshNode::hasMerged(const std::string &contentId) const {
  return merged_.contains(contentId);
}

} // namespace zigzag::vortex::mesh
