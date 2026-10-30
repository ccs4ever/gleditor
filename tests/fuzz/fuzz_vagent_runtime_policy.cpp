/**
 * @file fuzz_vagent_runtime_policy.cpp
 * @brief Fuzz the actor dialect's `arbitrated by` clause (P19 I3-1a).
 *
 * The input is the clause after `arbitrated by`, attached to a continuous or
 * a discrete predicate. Parsing and checking must never crash, and any policy
 * the checker accepts must satisfy every domain the design states, with a
 * deterministic canonical hash.
 */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/runtime/checker.hpp"
#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"

namespace {

using namespace xanadu::vagent::runtime;

// Bounds one iteration's work; not a source-language limit.
constexpr std::size_t maxHarnessInputBytes = 4 * 1024;

void require(bool ok) {
  if (!ok) std::abort();
}

void checkDomains(const ArbitrationPolicy &p, bool continuous) {
  require(p.continuous == continuous);
  require(p.model == kBetaReputationModel);
  require(std::isfinite(p.minConfidence) && p.minConfidence >= 0 &&
          p.minConfidence <= 1);
  require(std::isfinite(p.priorAlpha) && p.priorAlpha > 0);
  require(std::isfinite(p.priorBeta) && p.priorBeta > 0);
  require(p.maxAgeMs > 0 && p.clockSkewMs >= 0);
  require(p.maxUncertaintyMs > 0 && p.maxUncertaintyMs <= p.maxAgeMs);
  require(!p.decayMs || *p.decayMs > 0);
  require(!p.alignmentMs || *p.alignmentMs >= 0);
  require(std::isfinite(p.scoreTolerance) && p.scoreTolerance >= 0);
  require(p.priorAlpha <= kMaxPriorCount && p.priorBeta <= kMaxPriorCount);
  require(p.maxAgeMs <= kMaxPolicyDurationMs &&
          p.clockSkewMs <= kMaxPolicyDurationMs);
  if (p.strategy == ArbitrationStrategy::Consensus)
    require(continuous && !p.allowUntested);
  if (!continuous) {
    require(!p.clusterTolerance && !p.frame && !p.units && !p.minVariance &&
            !p.maxVariance && !p.maxClusterDiameter && !p.maxSlew &&
            !p.innovation);
    return;
  }
  require(p.clusterTolerance && std::isfinite(*p.clusterTolerance) &&
          *p.clusterTolerance > 0);
  require(p.frame && !p.frame->empty() && p.units && !p.units->empty());
  require(p.minVariance && p.maxVariance);
  require(*p.minVariance >= kMinAdmissibleVariance &&
          *p.maxVariance <= kMaxAdmissibleVariance &&
          *p.minVariance <= *p.maxVariance);
  require(!p.maxClusterDiameter ||
          (std::isfinite(*p.maxClusterDiameter) &&
           *p.maxClusterDiameter >= *p.clusterTolerance));
  require(!p.maxSlew || (std::isfinite(*p.maxSlew) && *p.maxSlew > 0));
  require(!p.innovation || (std::isfinite(*p.innovation) && *p.innovation > 0));
}

std::optional<ArbitrationPolicy> accepted(const std::string &source) {
  auto unit = parseUnit(source);
  if (!unit) return std::nullopt;
  auto checked = check(*unit);
  if (!checked) return std::nullopt;
  const auto found = checked->predicates.find("p");
  require(found != checked->predicates.end());
  return found->second.policy;
}

} // namespace

// libstdc++ 16's std::ranges::stable_sort allocates its temporary buffer with
// nothrow operator new and releases it with free, which ASan reports inside the
// library whenever the checker sorts several diagnostics. Only that one check
// is relaxed; every other ASan and UBSan check stays on.
extern "C" const char *__asan_default_options() {
  return "alloc_dealloc_mismatch=0";
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size == 0 || size > maxHarnessInputBytes) return 0;
  // The first byte picks the predicate's shape; the rest is the clause.
  const bool continuous = (data[0] & 1U) != 0;
  const std::string_view clause(reinterpret_cast<const char *>(data + 1),
                                size - 1);
  const std::string source =
      std::string(continuous ? "predicate p(key k: Str, x: Float)\n"
                             : "predicate p(key k: Str, open: Bool)\n") +
      "  arbitrated by " + std::string(clause) + "\nagent A {}\n";
  const auto policy = accepted(source);
  if (!policy) return 0;
  checkDomains(*policy, continuous);
  // Checking is deterministic, and the hash identifies the policy.
  const auto again = accepted(source);
  require(again && *again == *policy);
  require(policyHash(*policy) == policyHash(*again));
  require(policyHash(*policy).size() == 64);
  return 0;
}
