#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <map>

#include "common/xanadu/provenance.hpp"

namespace xanadu::vagent::runtime {
namespace {

enum class LiteralType : std::uint8_t { Number, Duration, Text, Flag };

enum class Need : std::uint8_t {
  Always,         ///< every arbitrated predicate
  Continuous,     ///< required when continuous, refused when discrete
  Optional,       ///< any predicate
  ContinuousOnly, ///< optional when continuous, refused when discrete
};

struct KeySpec {
  std::string_view name;
  LiteralType type;
  Need need;
};

// The closed table (I3-1 design section 2). Feature names and health
// thresholds join it with the slices that consume them.
constexpr std::array kKeys{
    KeySpec{"model", LiteralType::Text, Need::Always},
    KeySpec{"min_confidence", LiteralType::Number, Need::Always},
    KeySpec{"prior_alpha", LiteralType::Number, Need::Always},
    KeySpec{"prior_beta", LiteralType::Number, Need::Always},
    KeySpec{"clock", LiteralType::Text, Need::Always},
    KeySpec{"max_age", LiteralType::Duration, Need::Always},
    KeySpec{"clock_skew", LiteralType::Duration, Need::Optional},
    // The largest acquisition uncertainty a reading may claim; it widens the
    // age and future windows, so it must be bounded for them to mean
    // anything. Resolves to max_age.
    KeySpec{"max_uncertainty", LiteralType::Duration, Need::Optional},
    // `decay` is the freshness half-life of P19 section 3.2.
    KeySpec{"decay", LiteralType::Duration, Need::Optional},
    KeySpec{"alignment", LiteralType::Duration, Need::Optional},
    KeySpec{"score_tolerance", LiteralType::Number, Need::Optional},
    KeySpec{"allow_untested", LiteralType::Flag, Need::Optional},
    KeySpec{"cluster_tolerance", LiteralType::Number, Need::Continuous},
    KeySpec{"frame", LiteralType::Text, Need::Continuous},
    KeySpec{"units", LiteralType::Text, Need::Continuous},
    KeySpec{"min_variance", LiteralType::Number, Need::Continuous},
    KeySpec{"max_variance", LiteralType::Number, Need::Continuous},
    KeySpec{"max_cluster_diameter", LiteralType::Number, Need::ContinuousOnly},
    // `max_slew` is in policy units per second.
    KeySpec{"max_slew", LiteralType::Number, Need::ContinuousOnly},
    KeySpec{"innovation", LiteralType::Number, Need::ContinuousOnly},
};

std::string_view literalTypeName(LiteralType t) {
  switch (t) {
  case LiteralType::Number:
    return "a number";
  case LiteralType::Duration:
    return "a duration";
  case LiteralType::Text:
    return "a string";
  case LiteralType::Flag:
    return "true or false";
  }
  return "a literal";
}

bool matches(LiteralType want, const ELiteral &v) {
  using K = ELiteral::Kind;
  switch (want) {
  case LiteralType::Number:
    // An integer literal widens to a Float constant only while the double
    // still represents it exactly.
    return v.kind == K::Float ||
           (v.kind == K::Int && v.intValue >= -(std::int64_t{1} << 53) &&
            v.intValue <= (std::int64_t{1} << 53));
  case LiteralType::Duration:
    return v.kind == K::Duration;
  case LiteralType::Text:
    return v.kind == K::Str;
  case LiteralType::Flag:
    return v.kind == K::Bool;
  }
  return false;
}

double number(const ELiteral &v) {
  return v.kind == ELiteral::Kind::Int ? static_cast<double>(v.intValue)
                                       : v.floatValue;
}

class Checker {
public:
  Checker(const ArbitrationClause &clause, bool continuous)
      : clause_(clause), continuous_(continuous) {}

  std::expected<ArbitrationPolicy, std::vector<PolicyIssue>> run() {
    policy_.continuous = continuous_;
    strategy();
    collect();
    required();
    if (!issues_.empty()) return std::unexpected(std::move(issues_));
    assign();
    domains();
    if (!issues_.empty()) return std::unexpected(std::move(issues_));
    return policy_;
  }

private:
  void issue(std::string message, SourceLocation loc,
             PolicyIssueCode code = PolicyIssueCode::InvalidEpistemicPolicy) {
    issues_.push_back({code, std::move(message), loc});
  }

  void strategy() {
    const auto &s = clause_.strategy;
    if (s == "truest") {
      policy_.strategy = ArbitrationStrategy::Truest;
    } else if (s == "ranked") {
      policy_.strategy = ArbitrationStrategy::Ranked;
    } else if (s == "consensus") {
      policy_.strategy = ArbitrationStrategy::Consensus;
      // Consensus fuses measurements of one continuous state (P19 5.3).
      if (!continuous_)
        issue("`consensus` needs a continuous predicate (a non-key Float "
              "parameter)",
              clause_.loc);
    } else {
      issue(std::format("unknown arbitration strategy `{}`; expected "
                        "`truest`, `ranked` or `consensus`",
                        s),
            clause_.loc);
    }
  }

  void collect() {
    for (const auto &arg : clause_.args) {
      const auto spec = std::ranges::find(kKeys, arg.key, &KeySpec::name);
      if (spec == kKeys.end()) {
        issue(std::format("unknown policy key `{}`", arg.key), arg.loc);
        continue;
      }
      if (!values_.emplace(spec->name, &arg).second) {
        issue(std::format("policy key `{}` repeats", arg.key), arg.loc);
        continue;
      }
      if (!matches(spec->type, arg.value))
        issue(std::format("policy key `{}` takes {}", arg.key,
                          literalTypeName(spec->type)),
              arg.loc);
      const bool continuousKey =
          spec->need == Need::Continuous || spec->need == Need::ContinuousOnly;
      if (continuousKey && !continuous_)
        issue(std::format("policy key `{}` applies only to a continuous "
                          "predicate",
                          arg.key),
              arg.loc);
    }
  }

  void required() {
    for (const auto &spec : kKeys) {
      const bool needed = spec.need == Need::Always ||
                          (spec.need == Need::Continuous && continuous_);
      if (!needed || values_.contains(spec.name)) continue;
      issue(std::format("policy is missing required key `{}`", spec.name),
            clause_.loc,
            spec.name == "cluster_tolerance"
                ? PolicyIssueCode::MissingClusterTolerance
                : PolicyIssueCode::InvalidEpistemicPolicy);
    }
  }

  const PolicyArg *arg(std::string_view key) const {
    const auto found = values_.find(key);
    return found == values_.end() ? nullptr : found->second;
  }
  std::optional<double> num(std::string_view key) const {
    const auto *a = arg(key);
    return a ? std::optional{number(a->value)} : std::nullopt;
  }
  std::optional<std::int64_t> ms(std::string_view key) const {
    const auto *a = arg(key);
    return a ? std::optional{a->value.durationMs} : std::nullopt;
  }
  std::optional<std::string> text(std::string_view key) const {
    const auto *a = arg(key);
    return a ? std::optional{a->value.text} : std::nullopt;
  }

  void assign() {
    policy_.model            = *text("model");
    policy_.minConfidence    = *num("min_confidence");
    policy_.priorAlpha       = *num("prior_alpha");
    policy_.priorBeta        = *num("prior_beta");
    policy_.maxAgeMs         = *ms("max_age");
    policy_.clockSkewMs      = ms("clock_skew").value_or(0);
    policy_.maxUncertaintyMs = ms("max_uncertainty").value_or(policy_.maxAgeMs);
    policy_.decayMs          = ms("decay");
    policy_.alignmentMs      = ms("alignment");
    policy_.scoreTolerance =
        num("score_tolerance").value_or(kDefaultScoreTolerance);
    if (const auto *flag = arg("allow_untested"))
      policy_.allowUntested = flag->value.boolValue;
    policy_.clusterTolerance   = num("cluster_tolerance");
    policy_.frame              = text("frame");
    policy_.units              = text("units");
    policy_.minVariance        = num("min_variance");
    policy_.maxVariance        = num("max_variance");
    policy_.maxClusterDiameter = num("max_cluster_diameter");
    policy_.maxSlew            = num("max_slew");
    policy_.innovation         = num("innovation");
  }

  void need(bool ok, std::string_view key, std::string_view rule) {
    if (!ok)
      issue(std::format("policy key `{}` {}", key, rule),
            arg(key) ? arg(key)->loc : clause_.loc);
  }

  void positive(std::string_view key, std::optional<double> v) {
    if (v) need(std::isfinite(*v) && *v > 0, key, "must be finite and > 0");
  }
  void positiveMs(std::string_view key, std::optional<std::int64_t> v) {
    if (v)
      need(*v > 0 && *v <= kMaxPolicyDurationMs, key,
           "must be a positive duration of at most about 31 years");
  }
  void nonEmpty(std::string_view key, const std::optional<std::string> &v) {
    if (v) need(!v->empty(), key, "must not be empty");
  }

  void domains() {
    need(policy_.model == kBetaReputationModel, "model",
         std::format("must be \"{}\", the only model this release admits",
                     kBetaReputationModel));
    need(std::isfinite(policy_.minConfidence) && policy_.minConfidence >= 0 &&
             policy_.minConfidence <= 1,
         "min_confidence", "must lie in [0, 1]");
    positive("prior_alpha", policy_.priorAlpha);
    positive("prior_beta", policy_.priorBeta);
    need(policy_.priorAlpha <= kMaxPriorCount, "prior_alpha",
         "must be at most 1e12");
    need(policy_.priorBeta <= kMaxPriorCount, "prior_beta",
         "must be at most 1e12");
    if (const auto clock = *text("clock"); clock == "wall") {
      policy_.clock = ObservationClock::Wall;
    } else if (clock == "actor_mapped") {
      policy_.clock = ObservationClock::ActorMapped;
    } else {
      need(false, "clock", "must be \"wall\" or \"actor_mapped\"");
    }
    positiveMs("max_age", policy_.maxAgeMs);
    need(policy_.clockSkewMs >= 0 &&
             policy_.clockSkewMs <= kMaxPolicyDurationMs,
         "clock_skew", "must be a non-negative duration of at most 31 years");
    // Beyond max_age a reading could be any age and still pass.
    need(policy_.maxUncertaintyMs > 0 &&
             policy_.maxUncertaintyMs <= policy_.maxAgeMs,
         "max_uncertainty", "must be a positive duration of at most max_age");
    positiveMs("decay", policy_.decayMs);
    // P19 section 8.1 asks only for a nonnegative alignment tolerance.
    if (policy_.alignmentMs)
      need(*policy_.alignmentMs >= 0 &&
               *policy_.alignmentMs <= kMaxPolicyDurationMs,
           "alignment", "must be a non-negative duration of at most 31 years");
    need(std::isfinite(policy_.scoreTolerance) && policy_.scoreTolerance >= 0,
         "score_tolerance", "must be finite and >= 0");
    need(
        !(policy_.allowUntested &&
          policy_.strategy == ArbitrationStrategy::Consensus),
        "allow_untested",
        "cannot be true for consensus, which fuses accepted observations only");
    positive("cluster_tolerance", policy_.clusterTolerance);
    nonEmpty("frame", policy_.frame);
    nonEmpty("units", policy_.units);
    positive("max_slew", policy_.maxSlew);
    positive("innovation", policy_.innovation);
    if (policy_.maxClusterDiameter && policy_.clusterTolerance)
      need(std::isfinite(*policy_.maxClusterDiameter) &&
               *policy_.maxClusterDiameter >= *policy_.clusterTolerance,
           "max_cluster_diameter", "must be finite and >= cluster_tolerance");
    const auto within = [](double v) {
      return std::isfinite(v) && v >= kMinAdmissibleVariance &&
             v <= kMaxAdmissibleVariance;
    };
    if (policy_.minVariance)
      need(within(*policy_.minVariance), "min_variance",
           "must lie in [1e-150, 1e150]");
    if (policy_.maxVariance)
      need(within(*policy_.maxVariance), "max_variance",
           "must lie in [1e-150, 1e150]");
    if (policy_.minVariance && policy_.maxVariance)
      need(*policy_.minVariance <= *policy_.maxVariance, "max_variance",
           "must be >= min_variance");
  }

  const ArbitrationClause &clause_;
  bool continuous_;
  ArbitrationPolicy policy_;
  std::map<std::string_view, const PolicyArg *, std::less<>> values_;
  std::vector<PolicyIssue> issues_;
};

} // namespace

std::expected<ArbitrationPolicy, std::vector<PolicyIssue>>
checkArbitrationPolicy(const ArbitrationClause &clause, bool continuous) {
  return Checker(clause, continuous).run();
}

std::string_view strategyName(ArbitrationStrategy s) noexcept {
  switch (s) {
  case ArbitrationStrategy::Truest:
    return "truest";
  case ArbitrationStrategy::Ranked:
    return "ranked";
  case ArbitrationStrategy::Consensus:
    return "consensus";
  }
  return "unknown";
}

std::string_view clockName(ObservationClock c) noexcept {
  switch (c) {
  case ObservationClock::Wall:
    return "wall";
  case ObservationClock::ActorMapped:
    return "actor_mapped";
  }
  return "unknown";
}

std::string canonicalPolicy(const ArbitrationPolicy &p) {
  // Length-prefixed fields in a fixed order; doubles by their exact bits so
  // the encoding never depends on decimal formatting.
  std::string out("vagent-policy-2");
  out.push_back('\0');
  const auto put = [&](std::string_view tag, std::string_view value) {
    out += std::format("{}:{}:{};", tag, value.size(), value);
  };
  // +0.0 and -0.0 compare equal, so both encode as +0.0.
  const auto real = [&](std::string_view tag, std::optional<double> v) {
    put(tag, v ? std::format("{:016x}",
                             std::bit_cast<std::uint64_t>(*v == 0.0 ? 0.0 : *v))
               : std::string("-"));
  };
  const auto dur = [&](std::string_view tag, std::optional<std::int64_t> v) {
    put(tag, v ? std::to_string(*v) : std::string("-"));
  };
  const auto str = [&](std::string_view tag,
                       const std::optional<std::string> &v) {
    put(tag, v ? "=" + *v : std::string("-"));
  };
  put("strategy", strategyName(p.strategy));
  put("model", p.model);
  real("min_confidence", p.minConfidence);
  real("prior_alpha", p.priorAlpha);
  real("prior_beta", p.priorBeta);
  put("clock", clockName(p.clock));
  dur("max_age", p.maxAgeMs);
  dur("clock_skew", p.clockSkewMs);
  dur("max_uncertainty", p.maxUncertaintyMs);
  dur("decay", p.decayMs);
  dur("alignment", p.alignmentMs);
  real("score_tolerance", p.scoreTolerance);
  put("allow_untested", p.allowUntested ? "1" : "0");
  put("continuous", p.continuous ? "1" : "0");
  real("cluster_tolerance", p.clusterTolerance);
  str("frame", p.frame);
  str("units", p.units);
  real("min_variance", p.minVariance);
  real("max_variance", p.maxVariance);
  real("max_cluster_diameter", p.maxClusterDiameter);
  real("max_slew", p.maxSlew);
  real("innovation", p.innovation);
  return out;
}

std::string policyHash(const ArbitrationPolicy &policy) {
  return xanadu::sha256Hex(canonicalPolicy(policy));
}

} // namespace xanadu::vagent::runtime
