/**
 * @file swarm_lens.cpp
 * @brief SwarmLens: the Merkle identity ledger as a projection
 *        (design/lens-candidates.md section 3.6).
 */

#include "common/xanadu/zigzag/swarm_lens.hpp"

#include "common/xanadu/merkle_ledger.hpp"

namespace zigzag {

SwarmLens::SwarmLens(std::string label, const xanadu::MerkleLedger &ledger)
    : connKey_("swarm:" + std::move(label)), ledger_(&ledger) {}

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
}

bool SwarmLens::forEachRoot(
    gleditor::cpp26::function_ref<void(const ExternalKey &)> emit) const {
  emit(connKey_);
  return true;
}

} // namespace zigzag
