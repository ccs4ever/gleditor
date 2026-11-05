#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"

#include <format>

namespace xanadu::vagent::runtime {

namespace {

std::optional<std::string> cut(std::optional<std::string> text) {
  if (!text || text->size() <= kRefusalTextLimit) return text;
  auto end = kRefusalTextLimit;
  // Never end inside a UTF-8 sequence: a continuation byte is 10xxxxxx.
  while (end != 0 &&
         (static_cast<unsigned char>((*text)[end]) & 0xC0U) == 0x80U)
    --end;
  text->resize(end);
  return text;
}

} // namespace

TransferRefusal TransferRefusal::make(TransferCode code, TransferRoute route,
                                      zigzag::vortex::mesh::ActorError actor,
                                      RefusalDetail detail) {
  TransferRefusal refusal;
  refusal.code   = code;
  refusal.route  = route;
  refusal.actor  = actor;
  detail.subject = cut(std::move(detail.subject));
  detail.saved   = cut(std::move(detail.saved));
  detail.current = cut(std::move(detail.current));
  refusal.detail = std::move(detail);
  return refusal;
}

std::string_view toString(const TransferCode code) noexcept {
  switch (code) {
  case TransferCode::Runtime:
    return "Runtime";
  case TransferCode::NotQuiescent:
    return "NotQuiescent";
  case TransferCode::ActiveIntention:
    return "ActiveIntention";
  case TransferCode::PolicyChanged:
    return "PolicyChanged";
  case TransferCode::PredicateIncompatible:
    return "PredicateIncompatible";
  case TransferCode::PredicateRemoved:
    return "PredicateRemoved";
  case TransferCode::PeakCopyBudget:
    return "PeakCopyBudget";
  case TransferCode::WorkBudget:
    return "WorkBudget";
  case TransferCode::ProgramReferenceReused:
    return "ProgramReferenceReused";
  case TransferCode::RouteCarriesNoEpistemicState:
    return "RouteCarriesNoEpistemicState";
  case TransferCode::UnknownShapeVersion:
    return "UnknownShapeVersion";
  case TransferCode::PolicyEncodingChanged:
    return "PolicyEncodingChanged";
  }
  return "Unknown";
}

std::string_view toString(const TransferRoute route) noexcept {
  switch (route) {
  case TransferRoute::Resume:
    return "Resume";
  case TransferRoute::UpgradeSchema:
    return "UpgradeSchema";
  case TransferRoute::UpgradeReconciler:
    return "UpgradeReconciler";
  }
  return "Unknown";
}

std::string toString(const TransferRefusal &refusal) {
  std::string out = std::format("{}(route={}", toString(refusal.code),
                                toString(refusal.route));
  const auto &d   = refusal.detail;
  if (d.subject) out += std::format(", subject={}", *d.subject);
  if (d.saved) out += std::format(", saved={}", *d.saved);
  if (d.current) out += std::format(", current={}", *d.current);
  if (d.savedCount) out += std::format(", savedCount={}", *d.savedCount);
  if (d.currentCount) out += std::format(", currentCount={}", *d.currentCount);
  if (d.limit) out += std::format(", limit={}", *d.limit);
  out += ')';
  return out;
}

} // namespace xanadu::vagent::runtime
