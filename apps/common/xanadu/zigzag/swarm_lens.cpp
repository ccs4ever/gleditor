/**
 * @file swarm_lens.cpp
 * @brief SwarmLens: the Merkle identity ledger as a projection
 *        (design/lens-candidates.md section 3.6).
 */

#include "common/xanadu/zigzag/swarm_lens.hpp"

#include "common/xanadu/managed_torrent.hpp"
#include "common/xanadu/merkle_ledger.hpp"

namespace zigzag {

namespace {

const char *stateName(const xanadu::ManagedTorrentState s) {
  switch (s) {
  case xanadu::ManagedTorrentState::Stopped:
    return "stopped";
  case xanadu::ManagedTorrentState::Queued:
    return "queued";
  case xanadu::ManagedTorrentState::Checking:
    return "checking";
  case xanadu::ManagedTorrentState::Downloading:
    return "downloading";
  case xanadu::ManagedTorrentState::Seeding:
    return "seeding";
  case xanadu::ManagedTorrentState::Paused:
    return "paused";
  case xanadu::ManagedTorrentState::Error:
    return "error";
  }
  return "unknown";
}

} // namespace

SwarmLens::SwarmLens(std::string label, const xanadu::MerkleLedger &ledger,
                     const xanadu::SystemTorrentManager *manager)
    : connKey_("swarm:" + std::move(label)), ledger_(&ledger),
      manager_(manager) {}

std::vector<std::pair<std::string, std::optional<GenericDim>>>
SwarmLens::dimensions() const {
  return {{"d.contains", GenericDim::Containment},
          {"d.seq", GenericDim::Sequence},
          {"d.role", std::nullopt},
          {"d.roothash", std::nullopt},
          {"d.hash", std::nullopt},
          {"d.fingerprint", std::nullopt},
          {"d.email", std::nullopt},
          {"d.keyid", std::nullopt},
          {"d.time", std::nullopt},
          {"d.revoked", std::nullopt},
          {"d.name", std::nullopt},
          {"d.state", std::nullopt},
          {"d.peers", std::nullopt},
          {"d.seeds", std::nullopt},
          {"d.progress", std::nullopt},
          {"d.down", std::nullopt},
          {"d.up", std::nullopt},
          {"d.savepath", std::nullopt},
          {"d.keystab", std::nullopt}};
}

void SwarmLens::forEachNode(
    gleditor::cpp26::function_ref<void(const SourceNode &)> emit) const {
  emit({connKey_,
        std::string{"swarm ledger"},
        {{"d.role", "sw.ledger"}, {"d.roothash", ledger_->rootHex()}},
        0});
  for (const auto &e : ledger_->entries()) {
    emit({connKey_ + ":e:" + std::to_string(e.sequence),
          e.identity,
          {{"d.role", "sw.entry"},
           {"d.hash", e.leafHashHex()},
           {"d.fingerprint", e.fingerprint},
           {"d.email", e.email},
           {"d.keyid", e.keyId},
           {"d.time", std::to_string(e.timestamp)},
           {"d.revoked", e.revoked ? "1" : "0"},
           {"d.keystab", "pk"}},
          0});
  }
  // The live half: managed torrents by infohash -- content-addressed
  // identity is pk-strength for free.
  if (manager_ != nullptr) {
    for (const auto &t : manager_->listTorrents()) {
      emit({connKey_ + ":t:" + t.infoHash.hex(),
            t.name,
            {{"d.role", "sw.torrent"},
             {"d.name", t.name},
             {"d.state", stateName(t.state)},
             {"d.peers", std::to_string(t.numPeers)},
             {"d.seeds", std::to_string(t.numSeeds)},
             {"d.progress", std::to_string(t.progress)},
             {"d.down", std::to_string(t.downloadedBytes)},
             {"d.up", std::to_string(t.uploadedBytes)},
             {"d.savepath", t.savePath},
             {"d.keystab", "pk"}},
            0});
    }
  }
}

void SwarmLens::forEachEdge(
    gleditor::cpp26::function_ref<void(const SourceEdge &)> emit) const {
  std::string prev;
  for (const auto &e : ledger_->entries()) {
    const auto key = connKey_ + ":e:" + std::to_string(e.sequence);
    if (prev.empty()) {
      emit({connKey_, key, "d.contains", Posward});
    } else {
      emit({prev, key, "d.seq", Posward});
    }
    prev = key;
  }
  if (manager_ != nullptr) {
    for (const auto &t : manager_->listTorrents()) {
      const auto key = connKey_ + ":t:" + t.infoHash.hex();
      if (prev.empty()) {
        emit({connKey_, key, "d.contains", Posward});
      } else {
        emit({prev, key, "d.seq", Posward});
      }
      prev = key;
    }
  }
}

bool SwarmLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

} // namespace zigzag
