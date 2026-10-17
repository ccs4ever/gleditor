/**
 * @file spool_mesh.cpp
 * @brief Implementation of the Spool-Mesh distribution core (Slice 0).
 */
#include "common/xanadu/vortex/spool_mesh.hpp"

#include <format>
#include <utility>

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
  // Idempotent: a message already merged returns its existing local cell. This
  // is the content-dedup that turns an at-least-once transport into
  // effectively-once delivery.
  for (const auto &[cid, local] : mergedCells_) {
    if (cid == msg.contentId) return local;
  }
  // No remote mutation: mint a FRESH cell in THIS node's arena holding the
  // immutable payload. The origin node's arena is never touched.
  CellRef local = ctx_->arena.makeCell(std::string_view{msg.payload});
  mergedCells_.emplace_back(msg.contentId, local);
  merged_.insert(msg.contentId);
  return local;
}

bool MeshNode::hasMerged(const std::string &contentId) const {
  return merged_.contains(contentId);
}

} // namespace zigzag::vortex::mesh
