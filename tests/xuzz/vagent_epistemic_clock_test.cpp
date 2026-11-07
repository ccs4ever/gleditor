/**
 * @file vagent_epistemic_clock_test.cpp
 * @brief P19 I3-3b1 part A: the time split of design 4.3. Each row of the
 *        table at its boundary, the counted version 2, the common-cause
 *        invariance of the estimator, the pass-credit bound of 7.1, the
 *        time-class diagnostic and the pinned-sites script.
 */
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "epistemic_invariants.hpp"
#include "epistemic_time.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;
using xanadu::identity::ActorPid;

// max_age 10 s, clock_skew 1 s, max_uncertainty 2 s: every boundary below is
// a multiple of 0.1 s away from its neighbours, so the real clocks the test
// reads between two statements cannot move a reading across one.
constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    clock_skew: 1s, max_uncertainty: 2s, cluster_tolerance: 0.5,
    frame: "enu", units: "m", min_variance: 0.0001, max_variance: 100.0)
predicate seen(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "actor_mapped", max_age: 10s,
    clock_skew: 1s, max_uncertainty: 2s, cluster_tolerance: 0.5,
    frame: "enu", units: "m", min_variance: 0.0001, max_variance: 100.0)
agent A { var adds: Int := 0 }
)vagent";

constexpr std::array<std::string_view, 2> kPredicates{"at", "seen"};
constexpr double kMaxAge = 10.0;
/// Where the device of an actor-mapped source starts its own clock, so that
/// its seconds mean nothing on the host until the mapping is applied.
constexpr double kDeviceBias = 1000.0;

double wallNow() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// A distinct 64-digit lowercase principal per index.
std::string principalOf(unsigned n) {
  char digits[17];
  std::snprintf(digits, sizeof digits, "%016x", 0xabcdef00U + n);
  std::string out;
  while (out.size() < 64) out += digits;
  return out.substr(0, 64);
}

class VagentEpistemicClock : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  std::uint64_t counter{0};
  std::map<std::string, std::uint64_t> sequences;

  void SetUp() override { start({}); }

  void start(AgentSettings settings) {
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.settings = settings;
    host.emplace(std::move(*program), options);
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    sequences.clear();
  }

  /// Bind @p principal on both predicates.
  void bind(const std::string &principal) {
    for (const auto *predicate : {"at", "seen"})
      ASSERT_TRUE(host->executor().bindSource(
          pid, {std::string("s:") + predicate + principal.substr(8, 8),
                principal, 1, predicate, std::chrono::hours(1)}));
  }

  /// A fresh bound source.
  std::string source() {
    const auto principal = principalOf(static_cast<unsigned>(++counter));
    bind(principal);
    return principal;
  }

  double steadyNow() {
    return std::chrono::duration<double>(
               host->executor().hostTime().steady.time_since_epoch())
        .count();
  }

  /// A wall-basis reading signed at acquisition second @p at with signed
  /// uncertainty @p unc, on the identity mapping.
  CheckedObservation reading(const std::string &principal, double at,
                             double unc = 0.0, double x = 1.0) {
    const auto seq = ++sequences[principal];
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = 1;
    o.eventId     = principal.substr(0, 4) + "-" + std::to_string(seq) + "-" +
                std::to_string(++counter);
    o.sequence  = seq;
    o.predicate = "at";
    o.tuple     = {WireValue::text("self"), WireValue::real(x)};
    setTimes(o, at, unc);
    o.clockBasis = "wall";
    o.clockEpoch = 1;
    o.frame      = "enu";
    o.units      = "m";
    o.variance   = 0.25;
    return o;
  }

  /// An actor-mapped reading from a device whose clock reads
  /// `steady + kDeviceBias`, mapped by @p offset. The mapping is right when
  /// the offset is -kDeviceBias.
  CheckedObservation mapped(const std::string &principal, double deviceAt,
                            double offset, double x = 1.0) {
    auto o       = reading(principal, deviceAt, 0.01, x);
    o.predicate  = "seen";
    o.clockBasis = "actor_mapped";
    o.mapping    = ClockMapping{.offset = offset, .version = 1};
    return o;
  }

  std::optional<EpistemicRefusal> refused(const CheckedObservation &o) {
    const auto receipt = host->executor().observe(pid, o);
    host->pump();
    return receipt ? std::nullopt : std::optional{receipt.error()};
  }
  std::optional<IngestOutcome> outcomeOf(const CheckedObservation &o) {
    return host->executor().ingestOutcome(pid, o.principal, o.incarnation,
                                          o.eventId);
  }
  /// Offered and committed.
  ::testing::AssertionResult commits(const CheckedObservation &o) {
    if (const auto code = refused(o))
      return ::testing::AssertionFailure()
             << "refused: " << epistemicRefusalName(*code);
    const auto outcome = outcomeOf(o);
    if (!outcome || outcome->kind != IngestOutcome::Kind::Committed)
      return ::testing::AssertionFailure()
             << "not committed: "
             << (outcome && outcome->code ? *outcome->code : "pending");
    return ::testing::AssertionSuccess();
  }

  EpistemicView view() {
    return EpistemicView(host->executor(), pid, kPredicates);
  }
  EpistemicView::Weights weightsOf(const std::string &principal) {
    const auto all = view().weights();
    const auto it  = all.find(principal);
    return it == all.end() ? EpistemicView::Weights{} : it->second;
  }
  std::optional<SourceHealth> health(const std::string &principal) {
    return host->executor().sourceHealth(pid, principal);
  }
  std::optional<double> timeClass(const std::string &principal) {
    const auto h = health(principal);
    return h ? h->timeClassFraction : std::nullopt;
  }
  double fractionOf(const std::string &principal) {
    return health(principal)->refusalFraction;
  }
};

/// The warnings `vagent.ingress` logs while a scope lives.
class LogCapture {
public:
  LogCapture()
      : sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(out_)),
        logger_(gleditor::logging::category("vagent.ingress")),
        previous_(logger_->level()) {
    logger_->sinks().push_back(sink_);
    logger_->set_level(spdlog::level::warn);
  }
  ~LogCapture() {
    logger_->sinks().pop_back();
    logger_->set_level(previous_);
  }
  LogCapture(const LogCapture &)            = delete;
  LogCapture &operator=(const LogCapture &) = delete;
  [[nodiscard]] std::string text() {
    logger_->flush();
    return out_.str();
  }
  [[nodiscard]] std::size_t count(std::string_view needle) {
    const auto log = text();
    std::size_t n  = 0;
    for (auto at = log.find(needle); at != std::string::npos;
         at      = log.find(needle, at + needle.size()))
      ++n;
    return n;
  }

private:
  std::ostringstream out_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  spdlog::level::level_enum previous_;
};

// -- Rows of the table, each at its boundary ----------------------------------

TEST_F(VagentEpistemicClock, Row1ANegativeSignedValueIsInvalidAcquisition) {
  const auto now                   = wallNow();
  auto negativeAcquired            = reading(source(), now);
  negativeAcquired.device.acquired = -1;
  EXPECT_EQ(refused(negativeAcquired), EpistemicRefusal::InvalidAcquisition);
  auto negativeSent        = reading(source(), now);
  negativeSent.device.sent = -1;
  EXPECT_EQ(refused(negativeSent), EpistemicRefusal::InvalidAcquisition);
  auto negativeUnc                       = reading(source(), now);
  negativeUnc.device.acquiredUncertainty = -1;
  EXPECT_EQ(refused(negativeUnc), EpistemicRefusal::InvalidAcquisition);
  auto negativeClock                    = reading(source(), now);
  negativeClock.device.clockUncertainty = -1;
  EXPECT_EQ(refused(negativeClock), EpistemicRefusal::InvalidAcquisition);
  // Zero is a time like any other.
  auto zero                    = reading(source(), now);
  zero.device.clockUncertainty = 0;
  EXPECT_TRUE(commits(zero));
}

TEST_F(VagentEpistemicClock, Row2AcquiredAfterItWasSentIsRefusedAtTheEdge) {
  const auto now = wallNow();
  // a == s + u_a is the last instant that passes: exact in nanoseconds.
  auto edge        = reading(source(), now - 1.0, 0.5);
  edge.device.sent = edge.device.acquired - edge.device.acquiredUncertainty;
  EXPECT_TRUE(commits(edge));
  auto over        = reading(source(), now - 1.0, 0.5);
  over.device.sent = over.device.acquired - over.device.acquiredUncertainty - 1;
  EXPECT_EQ(refused(over), EpistemicRefusal::ObservationTimeMismatch);
}

TEST_F(VagentEpistemicClock,
       Row3TheSignedUncertaintyIsBoundedBeforeTheMapping) {
  const auto now               = wallNow();
  auto edge                    = reading(source(), now - 1.0, 1.5);
  edge.device.clockUncertainty = testNanos(0.5);
  EXPECT_TRUE(commits(edge));
  auto over                    = reading(source(), now - 1.0, 1.5);
  over.device.clockUncertainty = testNanos(0.5) + 1;
  EXPECT_EQ(refused(over), EpistemicRefusal::UncertainAcquisition);
}

TEST_F(VagentEpistemicClock, Row4TheDeviceHoldingItIsStaleObservation) {
  const auto now = wallNow();
  // s - a == max_age + u_a passes row 4; the device signs 0.3 s ahead, which
  // the skew allows, so rows 7 and 8 pass too.
  auto edge        = reading(source(), now + 0.3 - kMaxAge - 0.1, 0.1);
  edge.device.sent = testNanos(now + 0.3);
  edge.device.acquired =
      edge.device.sent - testNanos(kMaxAge) - edge.device.acquiredUncertainty;
  EXPECT_TRUE(commits(edge));
  auto over        = reading(source(), now, 0.1);
  over.device.sent = over.device.acquired + testNanos(kMaxAge) +
                     over.device.acquiredUncertainty + 1;
  EXPECT_EQ(refused(over), EpistemicRefusal::StaleObservation);
}

TEST_F(VagentEpistemicClock, Row5NoMappingIsRetryableAndLogged) {
  auto o = reading(source(), wallNow() - 1.0);
  o.mapping.reset();
  EXPECT_EQ(refused(o), EpistemicRefusal::UnknownClockMapping);
  ASSERT_TRUE(outcomeOf(o));
  EXPECT_EQ(outcomeOf(o)->code, "UnknownClockMapping");
  EXPECT_EQ(outcomeOf(o)->outcomeClass, OutcomeClass::Retryable);
}

TEST_F(VagentEpistemicClock, Row5aABrokenMappingNeedsTheHost) {
  const auto now    = wallNow() - 1.0;
  const auto broken = [&](auto &&change) {
    auto o = reading(source(), now);
    change(*o.mapping);
    return refused(o);
  };
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(broken([&](ClockMapping &m) { m.offset = kNaN; }),
            EpistemicRefusal::InvalidClockMapping);
  EXPECT_EQ(broken([](ClockMapping &m) { m.offset = 2e12; }),
            EpistemicRefusal::InvalidClockMapping);
  EXPECT_EQ(broken([](ClockMapping &m) { m.uncertainty = -1.0; }),
            EpistemicRefusal::InvalidClockMapping);
  EXPECT_EQ(broken([&](ClockMapping &m) { m.uncertainty = kNaN; }),
            EpistemicRefusal::InvalidClockMapping);
  EXPECT_EQ(broken([](ClockMapping &m) { m.driftPpm = -1.0; }),
            EpistemicRefusal::InvalidClockMapping);
  EXPECT_EQ(broken([](ClockMapping &m) {
              m.driftPpm = std::numeric_limits<double>::infinity();
            }),
            EpistemicRefusal::InvalidClockMapping);
  auto wrongBasis       = reading(source(), now);
  wrongBasis.clockBasis = "actor_mapped";
  EXPECT_EQ(refused(wrongBasis), EpistemicRefusal::InvalidClockMapping);
  ASSERT_TRUE(outcomeOf(wrongBasis));
  EXPECT_EQ(outcomeOf(wrongBasis)->outcomeClass, OutcomeClass::NeedsHostAction);
}

TEST_F(VagentEpistemicClock, Row6OnlyTheMappingMakesItTooUncertain) {
  const auto now            = wallNow() - 1.0;
  auto edge                 = reading(source(), now, 1.5);
  edge.mapping->uncertainty = 0.5;
  EXPECT_TRUE(commits(edge));
  auto over                 = reading(source(), now, 1.5);
  over.mapping->uncertainty = 0.6;
  EXPECT_EQ(refused(over), EpistemicRefusal::UncertainClockMapping);
  EXPECT_EQ(outcomeOf(over)->outcomeClass, OutcomeClass::Retryable);
  // Drift widens it with the distance from the anchor: 1e5 ppm over 5 s.
  auto drifting              = reading(source(), now, 1.5);
  drifting.mapping->driftPpm = 1e5;
  drifting.mapping->anchor   = now - 10.0;
  EXPECT_EQ(refused(drifting), EpistemicRefusal::UncertainClockMapping);
  drifting.mapping->anchor = now;
  drifting.eventId += "b";
  drifting.sequence += 1;
  EXPECT_TRUE(commits(drifting));
  // A device that signed too much uncertainty is row 3, not row 6.
  auto signedOver = reading(source(), now, 2.5);
  EXPECT_EQ(refused(signedOver), EpistemicRefusal::UncertainAcquisition);
}

TEST_F(VagentEpistemicClock, Row7ASendTimeAheadOfTheHostByMoreThanTheBounds) {
  const auto now = wallNow();
  // Sent 0.8 s ahead with a 1 s skew: inside. The device's clock
  // uncertainty widens the bound; the acquisition's does not.
  auto inside = reading(source(), now + 0.8, 0.0);
  EXPECT_TRUE(commits(inside));
  auto outside = reading(source(), now + 1.5, 0.0);
  EXPECT_EQ(refused(outside), EpistemicRefusal::ClockMappingMismatch);
  EXPECT_EQ(outcomeOf(outside)->outcomeClass, OutcomeClass::Retryable);
  auto excused                    = reading(source(), now + 1.5, 0.0);
  excused.device.clockUncertainty = testNanos(1.0);
  EXPECT_TRUE(commits(excused));
  auto acquisitionOnly = reading(source(), now + 1.5, 1.0);
  EXPECT_EQ(refused(acquisitionOnly), EpistemicRefusal::ClockMappingMismatch);
}

TEST_F(VagentEpistemicClock, Row8ThePathMakingItOldIsLateDelivery) {
  const auto now = wallNow();
  // r - M(a) == max_age is inside max_age + U by the 0.1 s of U.
  auto inside = reading(source(), now - kMaxAge, 0.1);
  EXPECT_TRUE(commits(inside));
  auto outside = reading(source(), now - kMaxAge - 0.5, 0.1);
  EXPECT_EQ(refused(outside), EpistemicRefusal::LateDelivery);
  EXPECT_EQ(outcomeOf(outside)->outcomeClass, OutcomeClass::Terminal);
}

TEST_F(VagentEpistemicClock,
       Row9AnExpiredMessageWidensByTheMappingUncertainty) {
  const auto now      = wallNow();
  auto live           = reading(source(), now - 0.5, 0.0);
  live.device.expires = testNanos(now + 5.0);
  EXPECT_TRUE(commits(live));
  auto expired           = reading(source(), now - 0.5, 0.0);
  expired.device.expires = testNanos(now - 0.5);
  EXPECT_EQ(refused(expired), EpistemicRefusal::MessageExpired);
  EXPECT_EQ(outcomeOf(expired)->outcomeClass, OutcomeClass::Terminal);
  // The mapping's uncertainty at the expiry keeps it alive.
  auto widened                 = reading(source(), now - 0.5, 0.0);
  widened.device.expires       = testNanos(now - 0.5);
  widened.mapping->uncertainty = 1.0;
  EXPECT_TRUE(commits(widened));
}

TEST_F(VagentEpistemicClock, SignedChecksRunWithoutOverflow) {
  const auto now                  = wallNow();
  auto huge                       = reading(source(), now, 0.0);
  huge.device.acquiredUncertainty = std::numeric_limits<std::int64_t>::max();
  huge.device.clockUncertainty    = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(refused(huge), EpistemicRefusal::UncertainAcquisition);
  auto wide            = reading(source(), now, 0.0);
  wide.device.sent     = std::numeric_limits<std::int64_t>::max();
  wide.device.acquired = 0;
  EXPECT_EQ(refused(wide), EpistemicRefusal::StaleObservation);
}

TEST_F(VagentEpistemicClock,
       AnUnsynchronisedWallClockStillRefusesBeforeTheMapping) {
  host->executor().setWallClockSynchronised(false);
  auto o = reading(source(), wallNow() - 1.0);
  o.mapping.reset();
  EXPECT_EQ(refused(o), EpistemicRefusal::ClockUnsynchronised);
}

TEST_F(VagentEpistemicClock, TheStoredRecordKeepsTheSignedAndTheMappedTimes) {
  const auto principal      = source();
  const auto now            = wallNow() - 1.0;
  auto o                    = reading(principal, now, 0.25);
  o.device.clockUncertainty = testNanos(0.05);
  o.mapping->offset         = 2.0;
  o.mapping->uncertainty    = 0.5;
  o.mapping->version        = 9;
  o.clockEpoch              = 3;
  // 2 s ahead of the device clock, and the same ahead on the host: row 7 and
  // 8 read the mapped values.
  o.device.acquired = testNanos(now - 2.0);
  o.device.sent     = o.device.acquired;
  ASSERT_TRUE(commits(o));
  const auto &ctx = *host->executor().context(pid);
  auto &arena     = const_cast<zigzag::ArenaManifold &>(ctx.arena);
  const auto dims = registerDimensions(arena);
  std::optional<zigzag::CellRef> record;
  for (const auto profile :
       zigzag::rankAfter(arena, *host->executor().agentCell(pid), dims.trust))
    if (arena.textOf(profile) == principal)
      for (const auto r : zigzag::rankAfter(arena, profile, dims.epistemic))
        record = r;
  // The current record hangs on the support, not the profile's history.
  for (const auto pred :
       zigzag::rankAfter(arena, *host->executor().agentCell(pid), dims.belief))
    for (const auto clause :
         zigzag::rankAfter(arena, pred, ctx.core.dims().clause))
      for (const auto support : zigzag::rankAfter(arena, clause, dims.source))
        if (arena.textOf(support) == principal)
          for (const auto r : zigzag::rankAfter(arena, support, dims.epistemic))
            record = r;
  ASSERT_TRUE(record);
  const auto field = [&](std::string_view name) {
    return fieldValue(arena, valueDims(ctx.core), *record, name);
  };
  const auto real = [&](std::string_view name) {
    const auto cell = field(name);
    return cell ? arena.asDouble(*cell) : std::nullopt;
  };
  const auto integer = [&](std::string_view name) {
    const auto cell = field(name);
    return cell ? arena.asInt64(*cell) : std::nullopt;
  };
  EXPECT_NEAR(*real(kObsAcquiredAt), now, 1e-6);
  EXPECT_NEAR(*real(kObsAcquiredUncertainty), 0.25 + 0.05 + 0.5, 1e-9);
  EXPECT_EQ(integer(kObsClockMappingVersion), 9);
  EXPECT_EQ(integer(kObsAcquiredDevice), o.device.acquired);
  EXPECT_EQ(integer(kObsSentDevice), o.device.sent);
  EXPECT_EQ(integer(kObsDeviceUncertainty), testNanos(0.05));
}

// -- What counts
// -----------------------------------------------------------------

TEST_F(VagentEpistemicClock, ARelayedCopyCountsExactlyTheVersion2Codes) {
  const auto now = wallNow();
  struct Case {
    const char *code;
    bool counted;
    std::function<void(CheckedObservation &)> break_;
  };
  const std::vector<Case> cases{
      {"ObservationFrameMismatch", true, [](auto &o) { o.frame = "ecef"; }},
      {"MissingVariance", true, [](auto &o) { o.variance.reset(); }},
      {"InvalidVariance", true, [](auto &o) { o.variance = 1e9; }},
      {"InvalidValue", true,
       [](auto &o) {
         o.tuple = {WireValue::text("self"),
                    WireValue::real(std::numeric_limits<double>::quiet_NaN())};
       }},
      {"InvalidAcquisition", true, [](auto &o) { o.device.acquired = -1; }},
      {"ObservationTimeMismatch", true,
       [](auto &o) { o.device.sent = o.device.acquired - 1; }},
      {"UncertainAcquisition", true,
       [](auto &o) { o.device.acquiredUncertainty = testNanos(3.0); }},
      {"ClockEpochRegression", true, [](auto &o) { o.clockEpoch = 4; }},
      {"StaleObservation", false,
       [](auto &o) { o.device.sent = o.device.acquired + testNanos(11.0); }},
      {"UnknownClockMapping", false, [](auto &o) { o.mapping.reset(); }},
      {"InvalidClockMapping", false,
       [](auto &o) { o.mapping->offset = std::nan(""); }},
      {"UncertainClockMapping", false,
       [](auto &o) { o.mapping->uncertainty = 5.0; }},
      {"ClockMappingMismatch", false,
       [](auto &o) { o.device.sent = o.device.acquired + testNanos(5.0); }},
      {"LateDelivery", false,
       [](auto &o) {
         o.device.acquired = testNanos(wallNow() - 30.0);
         o.device.sent     = o.device.acquired;
       }},
      {"MessageExpired", false,
       [](auto &o) { o.device.expires = testNanos(wallNow() - 5.0); }},
  };
  for (const bool relayed : {false, true})
    for (const auto &c : cases) {
      const auto principal = source();
      // Warm the stream at epoch 5 so the epoch regression has a floor.
      auto warm       = reading(principal, now - 1.0);
      warm.clockEpoch = 5;
      ASSERT_TRUE(commits(warm)) << c.code;
      const auto before = weightsOf(principal);
      auto o            = reading(principal, now - 1.0);
      o.clockEpoch      = 5;
      c.break_(o);
      if (relayed) o.relayedBy = principalOf(9999);
      refused(o);
      const auto outcome = outcomeOf(o);
      ASSERT_TRUE(outcome) << c.code;
      EXPECT_EQ(outcome->code, c.code)
          << (relayed ? "relayed " : "direct ") << c.code;
      const auto after = weightsOf(principal);
      if (c.counted) {
        EXPECT_GT(after.r, before.r)
            << (relayed ? "relayed " : "direct ") << c.code;
      } else {
        EXPECT_EQ(std::bit_cast<std::uint64_t>(after.r),
                  std::bit_cast<std::uint64_t>(before.r))
            << (relayed ? "relayed " : "direct ") << c.code;
        EXPECT_EQ(std::bit_cast<std::uint64_t>(after.n),
                  std::bit_cast<std::uint64_t>(before.n))
            << c.code;
      }
    }
  EXPECT_EQ(kHealthRefusalCodesVersion, 2);
}

TEST_F(VagentEpistemicClock, HostFaultsOfEitherKindMoveNoEstimatorWeight) {
  // The `common_cause` test of design 4.3, with the mappings handed to
  // `observe` directly: eight sources whose mapping shifts across the change
  // (A3 fires in the arena) and eight whose mapping breaks (row 5a).
  std::vector<std::string> shifted, broken;
  for (int i = 0; i < 8; ++i) {
    shifted.push_back(source());
    broken.push_back(source());
  }
  const auto steady = steadyNow();
  for (const auto &principal : shifted) {
    const double a1 = steady + kDeviceBias - 0.5;
    ASSERT_TRUE(commits(mapped(principal, a1, -kDeviceBias)));
    // The re-pair is off by 3 s: the mapped acquisition goes back in time by
    // more than both uncertainties and the skew.
    auto next  = mapped(principal, a1 + 0.2, -kDeviceBias - 3.0);
    next.tuple = {WireValue::text("self"), WireValue::real(2.0)};
    refused(next);
    ASSERT_TRUE(outcomeOf(next));
    EXPECT_EQ(outcomeOf(next)->code, "AcquisitionOrderMismatch");
  }
  for (const auto &principal : broken) {
    const double a1 = steady + kDeviceBias - 0.5;
    ASSERT_TRUE(commits(mapped(principal, a1, -kDeviceBias)));
    auto next  = mapped(principal, a1 + 0.2, std::nan(""));
    next.tuple = {WireValue::text("self"), WireValue::real(2.0)};
    refused(next);
    ASSERT_TRUE(outcomeOf(next));
    EXPECT_EQ(outcomeOf(next)->code, "InvalidClockMapping");
  }
  const auto all    = view().weights();
  const auto bits   = [](double v) { return std::bit_cast<std::uint64_t>(v); };
  const auto &first = all.at(shifted.front());
  for (const auto &group : {shifted, broken})
    for (const auto &principal : group) {
      const auto &w = all.at(principal);
      EXPECT_EQ(bits(w.r), bits(first.r)) << principal;
      EXPECT_EQ(bits(w.n), bits(first.n)) << principal;
      const auto h = health(principal);
      ASSERT_TRUE(h);
      EXPECT_EQ(h->state, HealthState::Nominal) << principal;
      EXPECT_EQ(bits(h->refusalFraction),
                bits(health(shifted.front())->refusalFraction))
          << principal;
    }
  EXPECT_EQ(first.r, 0.0);
  // A counted refusal, by contrast, moves the weights: the comparison above
  // can fail.
  const auto control = source();
  ASSERT_TRUE(commits(reading(control, wallNow() - 1.0)));
  auto frame  = reading(control, wallNow() - 1.0);
  frame.frame = "ecef";
  refused(frame);
  EXPECT_GT(weightsOf(control).r, 0.0);
}

TEST_F(VagentEpistemicClock,
       AFlushOfBufferedReadingsAfterAnOutageMovesNoWeight) {
  const auto principal = source();
  const auto now       = wallNow();
  ASSERT_TRUE(commits(reading(principal, now - 1.0)));
  const auto before = weightsOf(principal);
  // The device signs when the link returns: each reading is signed long
  // after it was acquired (row 4).
  for (int i = 0; i < 600; ++i) {
    auto o        = reading(principal, now - 100.0 - 0.01 * i);
    o.device.sent = testNanos(now - 0.05);
    EXPECT_EQ(refused(o), EpistemicRefusal::StaleObservation) << i;
  }
  const auto after = weightsOf(principal);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(after.r),
            std::bit_cast<std::uint64_t>(before.r));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(after.n),
            std::bit_cast<std::uint64_t>(before.n));
  EXPECT_EQ(health(principal)->state, HealthState::Nominal);
  // The host sees it, as a diagnostic only.
  ASSERT_TRUE(timeClass(principal));
  EXPECT_GT(*timeClass(principal), 0.9);
}

// -- The pass-credit bound
// ------------------------------------------------------

TEST_F(VagentEpistemicClock, ASlowStreamIsCreditedUpToTheVerifiedSequence) {
  constexpr std::uint64_t kStride = 1080;
  for (const bool verified : {false, true}) {
    const auto principal = source();
    const auto now       = wallNow();
    auto first           = reading(principal, now - 1.0);
    ASSERT_TRUE(commits(first));
    // Offered without a pump, so the per-source queue of eight stays full.
    for (int i = 0; i < 8; ++i)
      ASSERT_TRUE(host->executor().observe(pid, reading(principal, now - 1.0)))
          << "queued " << i;
    auto dropped     = reading(principal, now - 1.0);
    dropped.sequence = first.sequence + kStride;
    if (verified) dropped.verifiedSequenceBound = dropped.sequence;
    const auto receipt = host->executor().observe(pid, dropped);
    EXPECT_FALSE(receipt);
    host->pump();
    // First reading, eight queued ones, and a credit only when the verified
    // bound covers the stride: past mark + 1,024 it is not credited.
    const auto w = weightsOf(principal);
    const auto b = healthDecay(32);
    double n     = 0.0;
    for (int i = 0; i < 9 + (verified ? 1 : 0); ++i) n = b * n + 1.0;
    EXPECT_NEAR(w.n, n, 1e-9) << (verified ? "verified" : "unverified");
  }
}

// -- The time-class diagnostic
// --------------------------------------------------

TEST_F(VagentEpistemicClock, TheTimeClassFractionRisesOnRows4And7And8Only) {
  LogCapture log;
  const auto now = wallNow();
  for (const auto kind : {4, 7, 8}) {
    const auto principal = source();
    ASSERT_TRUE(commits(reading(principal, now - 1.0)));
    EXPECT_EQ(timeClass(principal), 0.0) << kind;
    for (int i = 0; i < 12; ++i) {
      auto o = reading(principal, now - 1.0);
      if (kind == 4) o.device.sent = o.device.acquired + testNanos(11.0);
      if (kind == 7) {
        o.device.acquired = testNanos(now + 5.0);
        o.device.sent     = o.device.acquired;
      }
      if (kind == 8) {
        o.device.acquired = testNanos(now - 30.0);
        o.device.sent     = o.device.acquired;
      }
      EXPECT_TRUE(refused(o)) << kind;
    }
    ASSERT_TRUE(timeClass(principal));
    EXPECT_GT(*timeClass(principal), 0.5) << kind;
    EXPECT_EQ(health(principal)->state, HealthState::Nominal) << kind;
    EXPECT_EQ(health(principal)->refusalFraction, 0.0) << kind;
  }
  EXPECT_EQ(log.count("time class fraction"), 3U);
  EXPECT_EQ(log.count("shared device time source"), 0U);
}

TEST_F(VagentEpistemicClock, AReplayedSequenceFeedsTheFractionOnce) {
  const auto principal = source();
  const auto now       = wallNow();
  ASSERT_TRUE(commits(reading(principal, now - 1.0)));
  auto late = reading(principal, now - 30.0);
  for (int i = 0; i < 20; ++i) {
    late.eventId = "again-" + std::to_string(i);
    EXPECT_EQ(refused(late), EpistemicRefusal::LateDelivery);
  }
  const auto b = healthDecay(32);
  // One commit's pass, then the one refusal the ring let through.
  const double n = b * 1.0 + 1.0;
  EXPECT_NEAR(*timeClass(principal), 1.0 / n, 1e-9);
}

TEST_F(VagentEpistemicClock, RowsThatSayNothingAboutTheDeviceFeedNothing) {
  const auto principal = source();
  const auto now       = wallNow();
  // Build a fraction first, so a feed would show.
  ASSERT_TRUE(commits(reading(principal, now - 1.0)));
  for (int i = 0; i < 10; ++i) {
    auto late = reading(principal, now - 30.0);
    refused(late);
  }
  const auto before = timeClass(principal);
  ASSERT_TRUE(before);
  EXPECT_GT(*before, 0.0);
  auto none = reading(principal, now - 1.0);
  none.mapping.reset();
  EXPECT_EQ(refused(none), EpistemicRefusal::UnknownClockMapping);
  auto broken            = reading(principal, now - 1.0);
  broken.mapping->offset = std::nan("");
  EXPECT_EQ(refused(broken), EpistemicRefusal::InvalidClockMapping);
  auto uncertain                 = reading(principal, now - 1.0, 1.5);
  uncertain.mapping->uncertainty = 1.0;
  EXPECT_EQ(refused(uncertain), EpistemicRefusal::UncertainClockMapping);
  auto expired           = reading(principal, now - 1.0);
  expired.device.expires = testNanos(now - 5.0);
  EXPECT_EQ(refused(expired), EpistemicRefusal::MessageExpired);
  auto stale     = reading(principal, now - 1.0);
  stale.sequence = 1;
  stale.eventId += "-old";
  (void)host->executor().observe(pid, stale);
  host->pump();
  EXPECT_EQ(std::bit_cast<std::uint64_t>(*timeClass(principal)),
            std::bit_cast<std::uint64_t>(*before));
}

TEST_F(VagentEpistemicClock,
       TheFirstPairingAfterARebootDoesNotMoveTheFraction) {
  const auto principal = source();
  const auto now       = wallNow();
  auto unpaired        = reading(principal, now - 1.0);
  unpaired.mapping.reset();
  EXPECT_EQ(refused(unpaired), EpistemicRefusal::UnknownClockMapping);
  EXPECT_FALSE(timeClass(principal));
  auto paired = reading(principal, now - 1.0);
  ASSERT_TRUE(commits(paired));
  EXPECT_EQ(timeClass(principal), 0.0);
  EXPECT_EQ(weightsOf(principal).r, 0.0);
}

TEST_F(VagentEpistemicClock, ASourceAmongAgreeingPeersIsNotSingledOut) {
  AgentSettings settings;
  settings.epistemic.commonCauseSources = 1;
  start(settings);
  LogCapture log;
  const auto now   = wallNow();
  const auto flood = [&](const std::string &principal) {
    ASSERT_TRUE(commits(reading(principal, now - 1.0)));
    for (int i = 0; i < 12; ++i) refused(reading(principal, now - 30.0));
  };
  flood(source());
  // One source is already above the fraction: the second is no outlier.
  flood(source());
  EXPECT_EQ(log.count("time class fraction"), 1U);
}

TEST_F(VagentEpistemicClock,
       SeveralSourcesSigningBadTimesWarnOnceOfASharedClock) {
  AgentSettings settings;
  settings.epistemic.commonCauseSources = 3;
  start(settings);
  LogCapture log;
  const auto now = wallNow();
  std::vector<std::string> sources;
  for (int i = 0; i < 4; ++i) sources.push_back(source());
  for (int i = 0; i < 4; ++i) {
    auto o = reading(sources[static_cast<std::size_t>(i)], now - 1.0);
    // Row 2 for some, row 3 for others: both are counted signed faults.
    if (i % 2 == 0) {
      o.device.sent = o.device.acquired - 1;
    } else {
      o.device.acquiredUncertainty = testNanos(3.0);
    }
    EXPECT_TRUE(refused(o));
    if (i < 2) {
      EXPECT_EQ(log.count("shared device time source"), 0U) << i;
    }
  }
  EXPECT_EQ(log.count("shared device time source"), 1U);
  const auto text = log.text();
  // Never what a device said: no tuple, no key.
  EXPECT_EQ(text.find("self"), std::string::npos);
}

// -- Settings ----------------------------------------------------------------

TEST(VagentEpistemicClockSettings, TheFourSettingsShipWithTheirDefaults) {
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  ASSERT_TRUE(model.isValid()) << model.validationError();
  EXPECT_EQ(AgentSettings::fromModel(model).epistemic, EpistemicLimits{});
  const EpistemicLimits defaults;
  EXPECT_EQ(defaults.timeClassHalfLife, 32);
  EXPECT_EQ(defaults.timeSuspectFraction, 0.25);
  EXPECT_EQ(defaults.commonCauseSources, 3);
  EXPECT_EQ(defaults.commonCauseWindowMs, 10'000);
  const auto broken = [](auto &&change) {
    EpistemicLimits l;
    change(l);
    return l.validate();
  };
  EXPECT_FALSE(broken([](auto &l) { l.timeClassHalfLife = 7; }));
  EXPECT_FALSE(broken([](auto &l) { l.timeClassHalfLife = 4097; }));
  EXPECT_FALSE(broken([](auto &l) { l.timeSuspectFraction = 0.0; }));
  EXPECT_FALSE(broken([](auto &l) { l.timeSuspectFraction = 1.01; }));
  EXPECT_FALSE(broken([](auto &l) { l.commonCauseSources = 0; }));
  EXPECT_FALSE(broken([](auto &l) { l.commonCauseSources = 129; }));
  EXPECT_FALSE(broken([](auto &l) { l.commonCauseWindowMs = 99; }));
  EXPECT_FALSE(broken([](auto &l) { l.commonCauseWindowMs = 3'600'001; }));
  EXPECT_TRUE(broken([](auto &l) { l.timeClassHalfLife = 8; }));
  EXPECT_TRUE(broken([](auto &l) { l.commonCauseSources = 128; }));
}

// -- The pinned sites
// ----------------------------------------------------------

namespace fs = std::filesystem;

int runScript(const fs::path &root, const fs::path &errors) {
  const auto command = "python3 -B tools/check-counted-sites.py --root '" +
                       root.string() + "' 2> '" + errors.string() + "'";
  return std::system(command.c_str());
}

TEST(VagentEpistemicClockSites, APlantedCountedSiteFailsTheScript) {
  if (std::system("python3 --version > /dev/null 2>&1") != 0)
    GTEST_SKIP() << "python3 is not available";
  const fs::path script = "tools/check-counted-sites.py";
  if (!fs::exists(script)) GTEST_SKIP() << "run from the repository root";
  const auto tree = fs::temp_directory_path() /
                    ("counted-sites-" + std::to_string(::getpid()));
  const fs::path runtime = "apps/common/xanadu/vagent/runtime";
  fs::create_directories(tree / runtime);
  for (const auto &entry : fs::directory_iterator(runtime))
    if (entry.is_regular_file())
      fs::copy_file(entry.path(), tree / runtime / entry.path().filename(),
                    fs::copy_options::overwrite_existing);
  const auto errors = tree / "errors.txt";
  EXPECT_EQ(runScript(tree, errors), 0) << "the tree as it is passes";
  {
    std::ofstream planted(tree / runtime / "agent_executor.cpp", std::ios::app);
    planted << "\nvoid planted() { refuse(EpistemicRefusal::"
               "ObservationTimeMismatch); }\n";
  }
  EXPECT_NE(runScript(tree, errors), 0);
  std::ifstream in(errors);
  std::stringstream text;
  text << in.rdbuf();
  EXPECT_NE(text.str().find("agent_executor.cpp"), std::string::npos)
      << text.str();
  EXPECT_NE(text.str().find("ObservationTimeMismatch"), std::string::npos);
  fs::remove_all(tree);
}

} // namespace
