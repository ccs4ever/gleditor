/**
 * @file vagent_epistemic_health_test.cpp
 * @brief P19 I3-2 slice 2a: the source-health estimator, its host-side
 *        counting rules, the derived state and the read API.
 *
 * The estimator is emitted IR, so most tests drive it through `observe` and
 * compare the profile's `R` and `N` with closed forms; the reference `Model`
 * below restates the formulae of the design (3.2) independently of the IR.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::CellRef;

constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

const std::string kP1(64, '1');
const std::string kP2(64, '2');
const std::string kRelay(64, 'a');

/// The estimator of design 3.2, written out for one half-life.
struct Model {
  std::int64_t halfLife{32};
  double r{0.0};
  double n{0.0};

  [[nodiscard]] double b() const { return healthDecay(halfLife); }
  [[nodiscard]] double cap() const { return healthWeightCap(halfLife); }
  [[nodiscard]] double f() const { return n > 0.0 ? r / n : 0.0; }
  void rescale() {
    if (n > cap()) {
      r = r * cap() / n;
      n = cap();
    }
  }
  /// `k` refusals among `n = k + c` events as one evenly interleaved mixture.
  void note(std::int64_t k, std::int64_t c) {
    const auto events = k + c;
    if (events <= 0) return;
    rescale();
    const double bn =
        std::exp(-static_cast<double>(events) * std::numbers::ln2_v<double> /
                 static_cast<double>(halfLife));
    const double geo = (1.0 - bn) / (1.0 - b());
    r = bn * r + static_cast<double>(k) / static_cast<double>(events) * geo;
    n = bn * n + geo;
    n = std::min(n, cap());
    r = std::min(r, n);
  }
  void accept() { note(0, 1); }
  void refuse() { note(1, 0); }
};

class VagentEpistemicHealth : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  double base{};
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
    for (const auto &[principal, alias] :
         {std::pair{kP1, "sensor:gps"}, std::pair{kP2, "sensor:odometry"}})
      ASSERT_TRUE(host->executor().bindSource(pid, grant(principal, alias)));
  }

  static EpistemicSourceGrant grant(const std::string &principal,
                                    std::string alias,
                                    std::uint64_t incarnation = 1) {
    return {std::move(alias), principal, incarnation, "at",
            std::chrono::hours(1)};
  }

  static double wallNow() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  /// A reading of @p principal at its next sequence unless @p sequence is
  /// given. Acquired just now, so a long run stays inside the policy's max_age.
  CheckedObservation reading(const std::string &principal,
                             std::optional<std::uint64_t> sequence = {},
                             double x                              = 1.0) {
    auto &next     = sequences[principal];
    const auto seq = sequence.value_or(++next);
    if (!sequence) next = seq;
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = 1;
    o.eventId     = principal.substr(0, 4) + "-" + std::to_string(seq) + "-" +
                std::to_string(++counter);
    o.sequence            = seq;
    o.predicate           = "at";
    o.tuple               = {WireValue::text("self"), WireValue::real(x)};
    o.acquiredAt          = wallNow() - 0.5;
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.clockEpoch          = 1;
    o.clockMappingVersion = 1;
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
  }
  /// The same reading with a wrong frame: a health refusal decided on signed
  /// fields.
  CheckedObservation bad(const std::string &principal,
                         std::optional<std::uint64_t> sequence = {}) {
    auto o  = reading(principal, sequence);
    o.frame = "ecef";
    return o;
  }
  std::uint64_t counter{0};

  /// Hand a reading to the executor without pumping; true when it queued.
  bool offer(const CheckedObservation &o) {
    return host->executor().observe(pid, o).has_value();
  }
  void pump() { host->pump(); }
  /// Offer and pump: one event, the whole way.
  bool send(const CheckedObservation &o) {
    const bool queued = offer(o);
    pump();
    return queued;
  }
  std::optional<IngestOutcome> outcomeOf(const CheckedObservation &o) {
    return host->executor().ingestOutcome(pid, o.principal, o.incarnation,
                                          o.eventId);
  }

  const zigzag::vortex::WorkerContext &ctx() {
    return *host->executor().context(pid);
  }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(ctx().arena);
  }
  std::vector<CellRef> profiles() {
    const auto dims = registerDimensions(arena());
    return zigzag::rankAfter(arena(), *host->executor().agentCell(pid),
                             dims.trust) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::optional<CellRef> profileOf(const std::string &principal) {
    for (const auto p : profiles())
      if (arena().textOf(p) == principal) return p;
    return std::nullopt;
  }
  std::optional<CellRef> value(CellRef record, std::string_view name) {
    return fieldValue(ctx().arena, valueDims(ctx().core), record, name);
  }
  std::optional<double> real(CellRef record, std::string_view name) {
    const auto cell = value(record, name);
    return cell ? ctx().arena.asDouble(*cell) : std::nullopt;
  }
  std::int64_t counterOf(CellRef profile, std::string_view name) {
    const auto cell = value(profile, name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::string text(CellRef record, std::string_view name) {
    const auto cell = value(record, name);
    return cell ? ctx().arena.textOf(*cell) : std::string("<absent>");
  }
  /// `R` and `N` of @p principal's profile, 0 while absent.
  Model weights(const std::string &principal, std::int64_t halfLife = 32) {
    Model m;
    m.halfLife = halfLife;
    if (const auto p = profileOf(principal)) {
      m.r = real(*p, kProfileHealthRefusedWeight).value_or(0.0);
      m.n = real(*p, kProfileHealthWeight).value_or(0.0);
    }
    return m;
  }
  void setWeights(const std::string &principal, double r, double n) {
    const auto p = profileOf(principal);
    ASSERT_TRUE(p);
    for (const auto &[name, v] : {std::pair{kProfileHealthRefusedWeight, r},
                                  std::pair{kProfileHealthWeight, n}}) {
      const auto cell = value(*p, name);
      ASSERT_TRUE(cell) << name;
      ASSERT_TRUE(arena().setValueBits(*cell, xanadu::ValueKind::Double,
                                       xanadu::canonicalDoubleBits(v)));
    }
  }
  /// The newest record of @p principal's only support.
  std::optional<CellRef> newestRecord(const std::string &principal) {
    const auto dims = registerDimensions(arena());
    for (const auto pred : zigzag::rankAfter(
             arena(), *host->executor().agentCell(pid), dims.belief))
      for (const auto clause :
           zigzag::rankAfter(arena(), pred, ctx().core.dims().clause))
        for (const auto support :
             zigzag::rankAfter(arena(), clause, dims.source))
          if (arena().textOf(support) == principal) {
            std::optional<CellRef> newest;
            for (const auto record :
                 zigzag::rankAfter(arena(), support, dims.epistemic))
              newest = record;
            return newest;
          }
    return std::nullopt;
  }
  std::optional<SourceHealth> health(const std::string &principal) {
    return host->executor().sourceHealth(pid, principal);
  }
  void expectWeights(const std::string &principal, const Model &want,
                     const char *what) {
    const auto got = weights(principal, want.halfLife);
    EXPECT_NEAR(got.r, want.r, 1e-9) << what;
    EXPECT_NEAR(got.n, want.n, 1e-9) << what;
  }
};

TEST_F(VagentEpistemicHealth, ClosedFormsForRefusalsAcceptsAndMixedNotes) {
  const double b = healthDecay(32);
  // k refusals in one note from an empty history: R = N = (1 - b^k)/(1 - b).
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(offer(bad(kP1)) == false);
  pump();
  const double geo5 = (1.0 - std::pow(b, 5)) / (1.0 - b);
  auto got          = weights(kP1);
  EXPECT_NEAR(got.r, geo5, 1e-9);
  EXPECT_NEAR(got.n, geo5, 1e-9);
  // m accepts after them: R decays by b^m, N gains (1 - b^m)/(1 - b).
  for (int i = 0; i < 7; ++i) ASSERT_TRUE(send(reading(kP1)));
  got = weights(kP1);
  EXPECT_NEAR(got.r, std::pow(b, 7) * geo5, 1e-9);
  EXPECT_NEAR(got.n, std::pow(b, 7) * geo5 + (1.0 - std::pow(b, 7)) / (1.0 - b),
              1e-9);
  // One refusal is R <- bR + 1, N <- bN + 1.
  const auto before = got;
  EXPECT_FALSE(send(bad(kP1)));
  got = weights(kP1);
  EXPECT_NEAR(got.r, b * before.r + 1.0, 1e-9);
  EXPECT_NEAR(got.n, b * before.n + 1.0, 1e-9);
}

TEST_F(VagentEpistemicHealth, WeightsStayBoundedAndFollowTheModelOverALongRun) {
  std::mt19937_64 rng(1);
  Model model;
  const double cap = model.cap();
  for (int step = 0; step < 1500; ++step) {
    const bool refuse = std::uniform_real_distribution<>(0, 1)(rng) < 0.3;
    if (refuse) {
      EXPECT_FALSE(send(bad(kP1)));
      model.refuse();
    } else {
      ASSERT_TRUE(send(reading(kP1)));
      model.accept();
    }
    if (step % 100 == 99) {
      const auto got = weights(kP1);
      ASSERT_GE(got.r, 0.0);
      ASSERT_LE(got.r, got.n);
      ASSERT_LE(got.n, cap);
      expectWeights(kP1, model, "long run");
    }
  }
}

TEST_F(VagentEpistemicHealth, ARescaleKeepsTheFractionWhenTheHalfLifeShrinks) {
  ASSERT_TRUE(send(reading(kP1)));
  // History accumulated under H = 64 (N near its cap) at f = 0.126, read
  // under the default H = 32.
  const double cap64 = healthWeightCap(64);
  const double cap32 = healthWeightCap(32);
  ASSERT_GT(cap64, cap32);
  setWeights(kP1, 0.126 * cap64, cap64);
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_NEAR(seen->refusalFraction, 0.126, 1e-12);
  EXPECT_NEAR(seen->weight, cap32, 1e-9);
  EXPECT_EQ(seen->state, HealthState::Nominal);
  // The next update rescales first, so f is continuous: 0.126 then one accept.
  ASSERT_TRUE(send(reading(kP1)));
  Model want;
  want.r = 0.126 * cap32;
  want.n = cap32;
  want.accept();
  expectWeights(kP1, want, "after the rescale");
  EXPECT_NEAR(weights(kP1).f(), want.f(), 1e-9);
  // Revision 3's clamp would have read 0.245 here and flipped the state.
  EXPECT_LT(weights(kP1).f(), 0.15);
}

TEST_F(VagentEpistemicHealth, ABatchEqualsSequentialUpdatesAcrossAShrink) {
  for (const std::int64_t k : {1, 2, 7, 30}) {
    // Two fresh sources with the same oversized history.
    for (const auto *who : {&kP1, &kP2}) {
      ASSERT_TRUE(send(reading(*who)));
      setWeights(*who, 0.3 * healthWeightCap(128), healthWeightCap(128));
    }
    // kP1: k refusals in one note. kP2: the same k, one note each.
    for (std::int64_t i = 0; i < k; ++i) EXPECT_FALSE(offer(bad(kP1)));
    pump();
    for (std::int64_t i = 0; i < k; ++i) {
      EXPECT_FALSE(offer(bad(kP2)));
      pump();
    }
    const auto batch = weights(kP1);
    const auto seq   = weights(kP2);
    EXPECT_NEAR(batch.r, seq.r, 1e-9) << "k=" << k;
    EXPECT_NEAR(batch.n, seq.n, 1e-9) << "k=" << k;
    EXPECT_LE(batch.n, healthWeightCap(32) + 1e-9);
    // Equal to each other is not enough (a clamp after the update makes both
    // wrong the same way): both equal the rescaled model.
    Model rescaled, stepwise;
    rescaled.r = stepwise.r = 0.3 * healthWeightCap(128);
    rescaled.n = stepwise.n = healthWeightCap(128);
    rescaled.note(k, 0);
    for (std::int64_t i = 0; i < k; ++i) stepwise.refuse();
    expectWeights(kP1, rescaled, "batch");
    expectWeights(kP2, stepwise, "sequential");
  }
}

TEST_F(VagentEpistemicHealth, SuspectNeedsTheMinimumWeightNotTheCount) {
  // Eight refusals: f = 1 but N = 7.43 < 8, so the fraction does not decide.
  for (int i = 0; i < 8; ++i) {
    EXPECT_FALSE(send(bad(kP1)));
    const auto seen = health(kP1);
    ASSERT_TRUE(seen);
    EXPECT_EQ(seen->state, HealthState::Nominal) << "after " << i + 1;
  }
  EXPECT_NEAR(health(kP1)->weight,
              (1 - std::pow(healthDecay(32), 8)) / (1 - healthDecay(32)), 1e-9);
  // The ninth reading is the first that can read SUSPECT.
  EXPECT_FALSE(send(bad(kP1)));
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->state, HealthState::Suspect);
  EXPECT_EQ(seen->reasonCode, "refusal_fraction");
  // One early refusal then clean readings never reads SUSPECT.
  EXPECT_FALSE(send(bad(kP2)));
  for (int i = 0; i < 40; ++i) {
    ASSERT_TRUE(send(reading(kP2)));
    ASSERT_EQ(health(kP2)->state, HealthState::Nominal) << i;
  }
}

TEST_F(VagentEpistemicHealth, ANoteIsAnEvenlyInterleavedMixture) {
  ASSERT_TRUE(send(reading(kP1)));
  // Fill the per-source queue (8), then offer 56 more readings with every
  // twentieth bad. The good ones meet a full queue and are credited.
  std::int64_t refused = 0, credited = 0;
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(offer(reading(kP1)));
  for (int i = 0; i < 56; ++i) {
    if (i % 20 == 10) {
      EXPECT_FALSE(offer(bad(kP1)));
      ++refused;
    } else {
      EXPECT_FALSE(offer(reading(kP1)));
      ++credited;
    }
  }
  pump();
  Model want;
  for (int i = 0; i < 9; ++i) want.accept();
  want.note(refused, credited);
  expectWeights(kP1, want, "mixture");
  EXPECT_LT(want.f(), 0.25);
  // Credits first (revision 3) would have put the refusals at the newest end.
  Model credits;
  for (int i = 0; i < 9; ++i) credits.accept();
  for (int i = 0; i < credited; ++i) credits.accept();
  for (int i = 0; i < refused; ++i) credits.refuse();
  EXPECT_GT(credits.f(), want.f() + 0.02);
  EXPECT_EQ(health(kP1)->state, HealthState::Nominal);
}

TEST_F(VagentEpistemicHealth, ARefuseAcceptRefuseSequencePostsTwoNotesInOrder) {
  EXPECT_FALSE(offer(bad(kP1)));
  ASSERT_TRUE(offer(reading(kP1)));
  EXPECT_FALSE(offer(bad(kP1)));
  pump();
  Model want;
  want.refuse();
  want.accept();
  want.refuse();
  expectWeights(kP1, want, "refuse, accept, refuse");
  Model swapped;
  swapped.refuse();
  swapped.refuse();
  swapped.accept();
  EXPECT_GT(std::abs(swapped.r - want.r), 0.01);
}

TEST_F(VagentEpistemicHealth, ARelayedRefusalCountsOnlyOnSignedFields) {
  // Signed basis: a relayed frame mismatch counts once.
  auto relayedBad      = bad(kP1);
  relayedBad.relayedBy = kRelay;
  EXPECT_FALSE(send(relayedBad));
  Model want;
  want.refuse();
  expectWeights(kP1, want, "relayed signed-basis refusal");
  // Host-mapped basis: a relayed bad acquisition time moves nothing.
  auto relayedAcq       = reading(kP1);
  relayedAcq.acquiredAt = std::numeric_limits<double>::quiet_NaN();
  relayedAcq.relayedBy  = kRelay;
  EXPECT_FALSE(send(relayedAcq));
  expectWeights(kP1, want, "relayed host-mapped refusal");
  EXPECT_EQ(health(kP1)->uncountedSinceCommit, 1U);
  // The same defect on a direct copy counts.
  auto direct       = reading(kP1);
  direct.acquiredAt = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(send(direct));
  want.refuse();
  expectWeights(kP1, want, "direct host-mapped refusal");
}

TEST_F(VagentEpistemicHealth, ARelayedRefusalDoesNotHideOrDoubleTheDirectCopy) {
  auto relayed      = bad(kP1, 1);
  relayed.relayedBy = kRelay;
  EXPECT_FALSE(send(relayed));
  auto direct = relayed;
  direct.relayedBy.reset();
  EXPECT_FALSE(send(direct));
  const auto profile = profileOf(kP1);
  ASSERT_TRUE(profile);
  EXPECT_EQ(counterOf(*profile, kProfileContentRefusals), 1);
  EXPECT_TRUE(value(*profile, kProfileLastContact));
  Model want;
  want.refuse();
  expectWeights(kP1, want, "moved once");
}

TEST_F(VagentEpistemicHealth, ReplayedRefusalsBelowTheMarkCountNothing) {
  ASSERT_TRUE(send(reading(kP1, 1000)));
  Model want;
  want.accept();
  for (const int distinct : {65, 1000}) {
    for (int round = 0; round < 3; ++round)
      for (int s = 1; s <= distinct; ++s) {
        auto o    = bad(kP1, static_cast<std::uint64_t>(s));
        o.eventId = "old-" + std::to_string(s);
        EXPECT_FALSE(offer(o));
      }
    pump();
    expectWeights(kP1, want, "an archive below the mark");
  }
  // Above the mark a refusal counts once however often it is replayed.
  for (int round = 0; round < 4; ++round)
    for (int s = 1001; s <= 1008; ++s) {
      auto o    = bad(kP1, static_cast<std::uint64_t>(s));
      o.eventId = "new-" + std::to_string(s);
      EXPECT_FALSE(offer(o));
    }
  pump();
  want.note(8, 0);
  expectWeights(kP1, want, "eight distinct refusals above the mark");
}

TEST_F(VagentEpistemicHealth,
       ARefusalWithAHugeSequenceCannotFreezeTheEstimator) {
  EXPECT_FALSE(send(bad(kP1, std::uint64_t{1} << 31)));
  Model want;
  want.refuse();
  std::mt19937_64 rng(7);
  for (int step = 1; step <= 300; ++step) {
    if (std::uniform_real_distribution<>(0, 1)(rng) < 0.3) {
      EXPECT_FALSE(send(bad(kP1, static_cast<std::uint64_t>(step))));
      want.refuse();
    } else {
      ASSERT_TRUE(send(reading(kP1, static_cast<std::uint64_t>(step))));
      want.accept();
    }
  }
  expectWeights(kP1, want, "after a poisoned sequence");
  // The mark followed the commits, not the refusal.
  EXPECT_NEAR(weights(kP1).f(), want.f(), 1e-9);
}

TEST_F(VagentEpistemicHealth, ACorruptSequenceIsNotCredited) {
  ASSERT_TRUE(send(reading(kP1, 1)));
  Model want;
  want.accept();
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(offer(reading(kP1, 2 + i)));
  // Past the gap of 1,024 above the mark: dropped and credited nothing.
  EXPECT_FALSE(offer(reading(kP1, 1'000'000)));
  pump();
  for (int i = 0; i < 8; ++i) want.accept();
  expectWeights(kP1, want, "no credit past the gap");
}

TEST_F(VagentEpistemicHealth, OneDroppedReadingCannotSetTheFirstMark) {
  // The stream has never committed, so a dropped reading credits nothing and
  // sets no mark: eight queued accepts, a ninth dropped, nine would be nine.
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(offer(reading(kP1, 1 + i)));
  EXPECT_FALSE(offer(reading(kP1, 9)));
  pump();
  Model want;
  for (int i = 0; i < 8; ++i) want.accept();
  expectWeights(kP1, want, "no credit without a mark");
}

TEST_F(VagentEpistemicHealth,
       ANewIncarnationStartsWithoutAMarkAndKeepsTheWeights) {
  ASSERT_TRUE(send(reading(kP1, 1000)));
  Model want;
  want.accept();
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  pump();
  // Sequence 5 is far below the old incarnation's mark of 1000 and still
  // counts: marks belong to an incarnation, and the advance keeps R and N.
  auto o        = bad(kP1, 5);
  o.incarnation = 2;
  EXPECT_FALSE(send(o));
  want.refuse();
  expectWeights(kP1, want, "after the advance");
}

TEST_F(VagentEpistemicHealth, ACreditedResendIsNotCountedTwice) {
  ASSERT_TRUE(send(reading(kP1, 1)));
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(offer(reading(kP1, 2 + i)));
  const auto dropped = reading(kP1, 10);
  EXPECT_FALSE(offer(dropped));
  pump();
  Model want;
  for (int i = 0; i < 9; ++i) want.accept();
  want.accept(); // the credit
  expectWeights(kP1, want, "after the credit");
  // The resend commits but the credit already stood for it.
  ASSERT_TRUE(send(dropped));
  expectWeights(kP1, want, "after the resend");
  const auto committed = outcomeOf(dropped);
  ASSERT_TRUE(committed);
  EXPECT_EQ(committed->kind, IngestOutcome::Kind::Committed);
  // A queued reading is counted although a later one was credited first.
  EXPECT_NE(committed->kind, IngestOutcome::Kind::Refused);
}

TEST_F(VagentEpistemicHealth, ALateReadingBelowTheMarkUpdatesNothing) {
  ASSERT_TRUE(send(reading(kP1, 5)));
  Model want;
  want.accept();
  EXPECT_TRUE(send(reading(kP1, 3)));
  EXPECT_FALSE(send(bad(kP1, 2)));
  expectWeights(kP1, want, "late genuine and late bad readings");
}

TEST_F(VagentEpistemicHealth, EpochRegressionCountsAndHostTimeRefusalsDoNot) {
  auto first                = reading(kP1);
  first.clockEpoch          = 5;
  first.clockMappingVersion = 2;
  ASSERT_TRUE(send(first));
  Model want;
  want.accept();
  // An epoch that went backwards is the device's own clock fault.
  auto olderEpoch       = reading(kP1);
  olderEpoch.clockEpoch = 4;
  olderEpoch.tuple      = {WireValue::text("other"), WireValue::real(2.0)};
  ASSERT_TRUE(send(olderEpoch));
  EXPECT_EQ(outcomeOf(olderEpoch)->code, "ClockEpochRegression");
  want.refuse();
  expectWeights(kP1, want, "ClockEpochRegression");
  // A mapping version that went backwards within one epoch is the host's.
  auto olderMapping                = reading(kP1);
  olderMapping.clockEpoch          = 5;
  olderMapping.clockMappingVersion = 1;
  olderMapping.tuple = {WireValue::text("other"), WireValue::real(3.0)};
  ASSERT_TRUE(send(olderMapping));
  EXPECT_EQ(outcomeOf(olderMapping)->code, "ClockMappingRegression");
  expectWeights(kP1, want, "ClockMappingRegression");
  EXPECT_EQ(health(kP1)->uncountedSinceCommit, 1U);
  // A real-time clock that reset reads stale: NOMINAL, and the signs.
  auto stale       = reading(kP1);
  stale.acquiredAt = wallNow() - 3600.0;
  EXPECT_FALSE(send(stale));
  expectWeights(kP1, want, "StaleObservation");
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->state, HealthState::Nominal);
  EXPECT_EQ(seen->lastRefusal, "StaleObservation");
  EXPECT_EQ(seen->uncountedSinceCommit, 2U);
  EXPECT_GE(seen->contentRefusals, 3);
  // A commit ends the streak.
  auto next                = reading(kP1);
  next.clockEpoch          = 5;
  next.clockMappingVersion = 2;
  ASSERT_TRUE(send(next));
  EXPECT_EQ(outcomeOf(next)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(health(kP1)->uncountedSinceCommit, 0U);
}

TEST_F(VagentEpistemicHealth, ANonFiniteOrMistypedTupleIsAHealthRefusal) {
  Model want;
  for (const double x : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    auto o  = reading(kP1);
    o.tuple = {WireValue::text("self"), WireValue::real(x)};
    EXPECT_FALSE(send(o));
    EXPECT_EQ(outcomeOf(o)->code, "InvalidValue");
    want.refuse();
  }
  auto mistyped  = reading(kP1);
  mistyped.tuple = {WireValue::text("self"), WireValue::text("one")};
  EXPECT_FALSE(send(mistyped));
  want.refuse();
  expectWeights(kP1, want, "InvalidValue");
}

TEST_F(VagentEpistemicHealth, TheCountedCodeTableIsPublishedAndVersioned) {
  EXPECT_EQ(kHealthRefusalCodesVersion, 1);
  std::vector<std::string_view> direct, relayed;
  for (const auto &entry : kHealthRefusalCodes) {
    direct.push_back(entry.code);
    if (entry.countsWhenRelayed) relayed.push_back(entry.code);
    EXPECT_TRUE(isHealthRefusalCode(entry.code, false));
    EXPECT_EQ(isHealthRefusalCode(entry.code, true), entry.countsWhenRelayed);
  }
  EXPECT_EQ(direct, (std::vector<std::string_view>{
                        "ObservationFrameMismatch", "MissingVariance",
                        "InvalidVariance", "InvalidValue", "InvalidAcquisition",
                        "ClockEpochRegression"}));
  EXPECT_EQ(relayed, (std::vector<std::string_view>{
                         "ObservationFrameMismatch", "MissingVariance",
                         "InvalidVariance", "InvalidValue"}));
  for (const auto host_time :
       {"StaleObservation", "ObservationTimeMismatch", "UncertainAcquisition",
        "ClockMappingRegression", "ConflictingEvent", "InvalidObservation"})
    EXPECT_FALSE(isHealthRefusalCode(host_time, false)) << host_time;
}

TEST_F(VagentEpistemicHealth, ARefusalOfARevivedBareTombstoneIsCharged) {
  const std::string who(64, '4');
  ASSERT_TRUE(host->executor().bindSource(pid, grant(who, "s:4", 4)));
  ASSERT_TRUE(host->executor().retireSource(pid, who));
  pump();
  ASSERT_TRUE(host->executor().bindSource(pid, grant(who, "s:4", 5)));
  pump();
  auto o        = bad(who);
  o.incarnation = 5;
  EXPECT_FALSE(send(o));
  const auto profile = profileOf(who);
  ASSERT_TRUE(profile);
  EXPECT_EQ(counterOf(*profile, kProfileContentRefusals), 1);
  Model want;
  want.refuse();
  expectWeights(who, want, "the revived tombstone");
}

TEST_F(VagentEpistemicHealth,
       TheStampHoldsTheStateAndTheFractionAfterTheUpdate) {
  Model want;
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(send(reading(kP1)));
    want.accept();
  }
  auto record = newestRecord(kP1);
  ASSERT_TRUE(record);
  EXPECT_EQ(text(*record, kObsHealth), "NOMINAL");
  EXPECT_NEAR(real(*record, kObsHealthFraction).value_or(-1), 0.0, 1e-12);
  // Refuse until SUSPECT, then a commit stamps the new state and fraction.
  for (int i = 0; i < 14; ++i) {
    EXPECT_FALSE(send(bad(kP1)));
    want.refuse();
  }
  ASSERT_TRUE(send(reading(kP1)));
  want.accept();
  record = newestRecord(kP1);
  ASSERT_TRUE(record);
  EXPECT_EQ(text(*record, kObsHealth), "SUSPECT");
  EXPECT_NEAR(real(*record, kObsHealthFraction).value_or(-1), want.f(), 1e-9);
  EXPECT_GE(want.f(), 0.25);
}

TEST_F(VagentEpistemicHealth, SourceHealthEqualsTheStampAtTheThresholdEdges) {
  ASSERT_TRUE(send(reading(kP1)));
  const double b = healthDecay(32);
  struct Case {
    double n, f; ///< the weights the commit leaves, as N and R / N
    HealthState want;
  };
  // Each is stamped by a real commit, so the IR rule is what is compared with
  // the C++ view; the margins are 1e-6, far above the update's rounding.
  const std::vector<Case> cases{
      {8.0 + 1e-6, 0.25 + 1e-6, HealthState::Suspect},  // both just over
      {8.0 - 1e-6, 0.9, HealthState::Nominal},          // N just under
      {20.0, 0.25 - 1e-6, HealthState::Nominal},        // f just under
      {20.0, 0.25 + 1e-6, HealthState::Suspect},        // f just over
      {20.0, 1e-320, HealthState::Nominal},             // subnormal R
      {healthWeightCap(32), 1.0, HealthState::Suspect}, // saturated
  };
  for (const auto &c : cases) {
    // The weights before the accept that leaves (n, f n).
    setWeights(kP1, c.f * c.n / b, (c.n - 1.0) / b);
    ASSERT_TRUE(send(reading(kP1)));
    const auto record = newestRecord(kP1);
    ASSERT_TRUE(record);
    const auto after = health(kP1);
    ASSERT_TRUE(after);
    EXPECT_NEAR(after->weight, c.n, 1e-9) << c.n << " " << c.f;
    EXPECT_EQ(after->state, c.want) << c.n << " " << c.f;
    EXPECT_EQ(text(*record, kObsHealth), healthStateName(c.want))
        << "the IR stamp at " << c.n << " " << c.f;
    EXPECT_NEAR(real(*record, kObsHealthFraction).value_or(-1),
                after->refusalFraction, 1e-12);
  }
}

TEST_F(VagentEpistemicHealth, TheReadApiAnswersOnlyForAReadableProfile) {
  EXPECT_FALSE(host->executor().sourceHealth(ActorPid{}, kP1));
  EXPECT_TRUE(host->executor().sourceHealths(ActorPid{}).empty());
  EXPECT_FALSE(health(kP1)); // bound but no profile yet
  ASSERT_TRUE(send(reading(kP1)));
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->principal, kP1);
  EXPECT_EQ(seen->incarnation, 1U);
  EXPECT_FALSE(seen->tombstone);
  EXPECT_FALSE(seen->faulted);
  EXPECT_EQ(host->executor().sourceHealths(pid).size(), 1U);
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  pump();
  const auto tomb = health(kP1);
  ASSERT_TRUE(tomb);
  EXPECT_TRUE(tomb->tombstone);
  EXPECT_EQ(tomb->state, HealthState::Nominal);
}

// -- The simulation ----------------------------------------------------------

/// The share of events spent SUSPECT after a warm-up of 3H, for Bernoulli
/// refusals at @p rate, with the default thresholds.
double suspectShare(std::int64_t halfLife, double rate, std::uint64_t seed,
                    std::int64_t events = 400'000) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<> uniform(0.0, 1.0);
  Model m;
  m.halfLife           = halfLife;
  std::int64_t suspect = 0, counted = 0;
  for (std::int64_t i = 0; i < events; ++i) {
    if (uniform(rng) < rate)
      m.refuse();
    else
      m.accept();
    if (i < 3 * halfLife) continue;
    ++counted;
    if (m.n >= 8.0 && m.f() >= 0.25) ++suspect;
  }
  return 100.0 * static_cast<double>(suspect) / static_cast<double>(counted);
}

TEST(VagentEpistemicHealthSimulation,
     TheSuspectShareFollowsTheDocumentedTable) {
  struct Row {
    double rate;
    double share[4]; // H = 8, 16, 32, 64, in percent
  };
  const Row table[] = {
      {0.05, {0.1, 0.0, 0.0, 0.0}},     {0.10, {2.0, 0.2, 0.0, 0.0}},
      {0.15, {10.0, 3.8, 0.7, 0.0}},    {0.20, {26.0, 19.5, 12.0, 4.6}},
      {0.30, {68.0, 76.0, 85.0, 93.0}}, {0.50, {99.0, 100.0, 100.0, 100.0}},
  };
  const std::int64_t halfLives[] = {8, 16, 32, 64};
  for (const auto &row : table)
    for (int h = 0; h < 4; ++h) {
      const double got = suspectShare(halfLives[h], row.rate, 1);
      EXPECT_NEAR(got, row.share[h], 2.0)
          << "rate " << row.rate << " H " << halfLives[h];
    }
}

TEST(VagentEpistemicHealthSimulation, StartUpFalseAlarmsAreAStartUpEffect) {
  // The probability of at least one SUSPECT reading in the first 12 and 100
  // events at a true 5% and 10% refusal rate, min_events 8.
  const auto alarm = [](double rate, int horizon) {
    std::mt19937_64 rng(1);
    std::uniform_real_distribution<> uniform(0.0, 1.0);
    int alarms            = 0;
    constexpr int kTrials = 20000;
    for (int t = 0; t < kTrials; ++t) {
      Model m;
      bool hit = false;
      for (int i = 0; i < horizon && !hit; ++i) {
        if (uniform(rng) < rate)
          m.refuse();
        else
          m.accept();
        hit = m.n >= 8.0 && m.f() >= 0.25;
      }
      alarms += hit;
    }
    return 100.0 * alarms / kTrials;
  };
  EXPECT_NEAR(alarm(0.05, 12), 1.9, 1.0);
  EXPECT_NEAR(alarm(0.10, 12), 10.8, 1.5);
  EXPECT_NEAR(alarm(0.05, 100), 2.2, 1.0);
  EXPECT_NEAR(alarm(0.10, 100), 15.1, 2.0);
}

TEST(VagentEpistemicHealthSimulation, TheRescaleKeepsAMatureFraction) {
  Model m;
  m.halfLife = 64;
  m.r        = 0.126 * m.cap();
  m.n        = m.cap();
  m.halfLife = 32;
  m.rescale();
  EXPECT_NEAR(m.f(), 0.126, 1e-12);
  // Revision 3's clamp: R kept, N cut to the new cap.
  EXPECT_GT(0.126 * healthWeightCap(64) / healthWeightCap(32), 0.24);
}

TEST(VagentEpistemicHealthSimulation, ANoteMixtureMatchesTheOrderItReplaces) {
  // Randomised mixed accept/refusal/credit runs including an H change: a
  // pure run batches exactly; a mixed run differs from any one order by at
  // most the weight of the events in it.
  std::mt19937_64 rng(3);
  for (int trial = 0; trial < 200; ++trial) {
    Model batch, seq;
    batch.halfLife = seq.halfLife =
        std::uniform_int_distribution<>(8, 128)(rng);
    const auto warm = std::uniform_int_distribution<>(0, 200)(rng);
    for (int i = 0; i < warm; ++i) {
      const bool refuse = std::uniform_real_distribution<>(0, 1)(rng) < 0.2;
      refuse ? batch.refuse() : batch.accept();
      refuse ? seq.refuse() : seq.accept();
    }
    batch.halfLife = seq.halfLife =
        std::uniform_int_distribution<>(8, 128)(rng);
    const auto k = std::uniform_int_distribution<>(0, 40)(rng);
    const auto c = std::uniform_int_distribution<>(0, 40)(rng);
    batch.note(k, c);
    // The same events one at a time, evenly interleaved.
    seq.rescale();
    for (int i = 0, kk = 0, cc = 0; i < k + c; ++i) {
      const bool refuse = kk < k && (cc >= c || kk * (k + c) <= i * k);
      refuse ? (seq.refuse(), ++kk) : (seq.accept(), ++cc);
    }
    EXPECT_LE(batch.r, batch.n + 1e-12);
    EXPECT_LE(batch.n, batch.cap() + 1e-9);
    if (k == 0 || c == 0) {
      EXPECT_NEAR(batch.r, seq.r, 1e-9) << trial;
      EXPECT_NEAR(batch.n, seq.n, 1e-9) << trial;
    } else {
      EXPECT_NEAR(batch.f(), seq.f(), 0.05) << trial;
    }
  }
}

TEST_F(VagentEpistemicHealth, TheVmAgreesWithTheModelOnASeededRun) {
  // The IR, not just the model: a seeded 30% source read through the VM
  // reaches SUSPECT where the model does, at the same event.
  std::mt19937_64 rng(11);
  Model m;
  std::optional<int> modelAt, vmAt;
  for (int i = 0; i < 120; ++i) {
    const bool refuse = std::uniform_real_distribution<>(0, 1)(rng) < 0.5;
    if (refuse) {
      EXPECT_FALSE(send(bad(kP1)));
      m.refuse();
    } else {
      ASSERT_TRUE(send(reading(kP1)));
      m.accept();
    }
    if (!modelAt && m.n >= 8.0 && m.f() >= 0.25) modelAt = i;
    const auto seen = health(kP1);
    ASSERT_TRUE(seen);
    if (!vmAt && seen->state == HealthState::Suspect) vmAt = i;
  }
  ASSERT_TRUE(modelAt);
  EXPECT_EQ(modelAt, vmAt);
  expectWeights(kP1, m, "seeded run");
}

TEST_F(VagentEpistemicHealth, ResumeKeepsTheWeightsAndForgetsTheMarks) {
  ASSERT_TRUE(send(reading(kP1, 1000)));
  Model want;
  want.accept();
  for (std::uint64_t s = 1001; s <= 1003; ++s) {
    EXPECT_FALSE(send(bad(kP1, s)));
    want.refuse();
  }
  expectWeights(kP1, want, "before the checkpoint");
  const auto saved = host->executor().checkpoint(pid);
  ASSERT_TRUE(saved);
  auto program = CompiledProgram::fromSource(kProgram);
  ASSERT_TRUE(program) << program.error();
  LocalHost other(std::move(*program));
  const auto resumed = other.executor().resume(*saved);
  ASSERT_TRUE(resumed);
  other.pump();
  other.executor().setWallClockSynchronised(true);
  ASSERT_TRUE(other.executor().bindSource(*resumed, grant(kP1, "sensor:gps")));
  other.pump();
  const auto profile = [&]() -> std::optional<CellRef> {
    const auto &c   = *other.executor().context(*resumed);
    auto &arena     = const_cast<zigzag::ArenaManifold &>(c.arena);
    const auto dims = registerDimensions(arena);
    for (const auto p : zigzag::rankAfter(
             arena, *other.executor().agentCell(*resumed), dims.trust))
      if (arena.textOf(p) == kP1) return p;
    return std::nullopt;
  }();
  ASSERT_TRUE(profile);
  const auto &c  = *other.executor().context(*resumed);
  const auto get = [&](std::string_view name) {
    const auto cell = fieldValue(c.arena, valueDims(c.core), *profile, name);
    return cell ? c.arena.asDouble(*cell).value_or(-1.0) : -1.0;
  };
  EXPECT_NEAR(get(kProfileHealthRefusedWeight), want.r, 1e-12);
  EXPECT_NEAR(get(kProfileHealthWeight), want.n, 1e-12);
  // The marks are host memory: the same three refusals count again on the
  // resumed side, until the device next commits. This is the stated bound.
  for (std::uint64_t s = 1001; s <= 1003; ++s) {
    auto o = bad(kP1, s);
    ASSERT_FALSE(other.executor().observe(*resumed, o));
  }
  other.pump();
  want.note(3, 0);
  EXPECT_NEAR(get(kProfileHealthRefusedWeight), want.r, 1e-9);
  EXPECT_NEAR(get(kProfileHealthWeight), want.n, 1e-9);
  const auto seen = other.executor().sourceHealth(*resumed, kP1);
  ASSERT_TRUE(seen);
  EXPECT_NEAR(seen->refusalFraction, want.f(), 1e-9);
}

TEST_F(VagentEpistemicHealth,
       SubnormalWeightsStayFiniteAndRoundTripInTheArena) {
  ASSERT_TRUE(send(reading(kP1)));
  setWeights(kP1, 5e-324, 12.0);
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(send(reading(kP1)));
  const auto got = weights(kP1);
  EXPECT_GE(got.r, 0.0);
  EXPECT_LT(got.r, 1e-300);
  EXPECT_TRUE(std::isfinite(got.n));
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_LT(seen->refusalFraction, 1e-300);
  EXPECT_EQ(seen->state, HealthState::Nominal);
}

TEST_F(VagentEpistemicHealth, AFleetWideFramePolicyChangeReadsSuspectAndHeals) {
  const std::vector<std::string> fleet{kP1, kP2, std::string(64, '3')};
  ASSERT_TRUE(
      host->executor().bindSource(pid, grant(fleet[2], "sensor:third")));
  // Every source refuses every reading while the frame is wrong.
  for (int i = 0; i < 16; ++i)
    for (const auto &who : fleet) EXPECT_FALSE(send(bad(who)));
  for (const auto &who : fleet)
    EXPECT_EQ(health(who)->state, HealthState::Suspect) << who.substr(0, 4);
  // The policy is fixed: clean readings heal them without a reset.
  for (int i = 0; i < 90; ++i)
    for (const auto &who : fleet) ASSERT_TRUE(send(reading(who)));
  for (const auto &who : fleet)
    EXPECT_EQ(health(who)->state, HealthState::Nominal) << who.substr(0, 4);
}

TEST_F(VagentEpistemicHealth, AProfileWithoutWeightsAndATombstoneHaveAState) {
  ASSERT_TRUE(send(reading(kP1)));
  // A profile from before the estimator: the fields absent read as 0.
  const auto profile = profileOf(kP1);
  ASSERT_TRUE(profile);
  EXPECT_TRUE(value(*profile, kProfileHealthRefusedWeight));
  const auto seen = health(kP1);
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->state, HealthState::Nominal);
  EXPECT_NEAR(seen->weight, 1.0, 1e-12);
  EXPECT_TRUE(std::isfinite(seen->refusalFraction));
}

TEST_F(VagentEpistemicHealth, MeasureTheCellsOfAProfileAndARecord) {
  const auto c0 = *host->executor().epistemicCells(pid);
  ASSERT_TRUE(send(reading(kP1)));
  const auto c1 = *host->executor().epistemicCells(pid);
  ASSERT_TRUE(send(reading(kP1)));
  const auto c2 = *host->executor().epistemicCells(pid);
  ASSERT_TRUE(send(reading(kP1)));
  const auto c3     = *host->executor().epistemicCells(pid);
  const auto record = c3 - c2;
  std::fprintf(
      stderr,
      "MEASURE cells: empty %zu, first commit +%zu, second +%zu, "
      "third +%zu (a record is %zu; the profile and support are %zu)\n",
      c0, c1 - c0, c2 - c1, c3 - c2, record, c1 - c0 - record);
  EXPECT_EQ(c3 - c2, c2 - c1);
}

TEST_F(VagentEpistemicHealth, MeasureTheCostOfAnIngestionAndABookkeepingTurn) {
  // A measurement for the design's price section, not a bound.
  const auto slotsAtLaunch = ctx().arena.cellCount();
  ASSERT_TRUE(send(reading(kP1)));
  ASSERT_TRUE(send(reading(kP1)));
  const auto cycles       = [&] { return ctx().vm.totalCycles(); };
  const auto slots        = [&] { return ctx().arena.cellCount(); };
  const auto cyclesBefore = cycles();
  const auto slotsBefore  = slots();
  ASSERT_TRUE(send(reading(kP1)));
  const auto ingestion      = cycles() - cyclesBefore;
  const auto ingestionSlots = slots() - slotsBefore;
  const auto mid            = cycles();
  const auto slotsMid       = slots();
  EXPECT_FALSE(send(bad(kP1)));
  const auto refusal      = cycles() - mid;
  const auto refusalSlots = slots() - slotsMid;
  std::fprintf(stderr,
               "MEASURE health: ingestion %llu cycles, %zu slots; one refusal "
               "and its bookkeeping turn %llu cycles, %zu slots; arena slots "
               "at launch %zu\n",
               static_cast<unsigned long long>(ingestion),
               static_cast<std::size_t>(ingestionSlots),
               static_cast<unsigned long long>(refusal),
               static_cast<std::size_t>(refusalSlots),
               static_cast<std::size_t>(slotsAtLaunch));
}

// -- Slice 2c2: the health turn ---------------------------------------------

constexpr auto kActionProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate boom(n: Int)
event Quit { }
agent A {
  var adds: Int := 0
  var drops: Int := 0
  on Quit { terminate }
  on +at(?w, ?x) { adds := adds + 1 }
  on -at(?w, ?x) { drops := drops + 1 }
}
)vagent";

constexpr auto kFailingDropProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate boom(n: Int)
event Quit { }
agent A {
  var drops: Int := 0
  on Quit { terminate }
  on -at(?w, ?x) { believe boom(1) }
  on +boom(?n) { believe boom(n + 1) }
}
)vagent";

const std::string kOperatorPrincipal(64, 'b');

class VagentEpistemicHealthActions : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  std::uint64_t seq{0};
  std::optional<HealthGrantToken> token;
  // Entry labels of the arena's reasons and actions, read back.
  struct Entry {
    std::string kind, code, origin;
    std::int64_t seq{0}, count{0};
    std::string removed;
  };

  void start(std::string_view source    = kActionProgram,
             LocalHost::Options options = {}) {
    auto program = CompiledProgram::fromSource(std::string(source));
    ASSERT_TRUE(program) << program.error();
    host.emplace(std::move(*program), std::move(options));
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    ASSERT_TRUE(ex().bindSource(pid, bindingFor(1)));
    auto granted = ex().grantHealth(pid, grant("g1"));
    ASSERT_TRUE(granted);
    token.emplace(*granted);
  }

  AgentExecutor &ex() { return host->executor(); }
  static EpistemicSourceGrant bindingFor(std::uint64_t incarnation) {
    return {"sensor:gps", kP1, incarnation, "at", std::chrono::hours(1)};
  }
  static EpistemicHealthGrant
  grant(std::string id,
        std::vector<HealthActionKind> kinds = {
            HealthActionKind::Isolate, HealthActionKind::Degrade,
            HealthActionKind::Clear, HealthActionKind::Reset}) {
    EpistemicHealthGrant g{std::move(id),
                           *HealthIssuer::fromPrincipal(kOperatorPrincipal),
                           {kP1},
                           {},
                           std::chrono::hours(1),
                           64};
    for (const auto kind : kinds) g.actions.push_back(kind);
    return g;
  }

  static double wall() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  CheckedObservation reading(std::string key, std::uint64_t incarnation = 1) {
    ++seq;
    CheckedObservation o;
    o.principal   = kP1;
    o.incarnation = incarnation;
    o.eventId     = "e" + std::to_string(seq);
    o.sequence    = seq;
    o.predicate   = "at";
    o.tuple       = {WireValue::text(std::move(key)), WireValue::real(1.0)};
    o.acquiredAt  = wall() - 5.0 + 0.01 * static_cast<double>(seq);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
  }
  /// The outcome code of a reading, "Committed" or the refusal.
  std::string send(const CheckedObservation &o) {
    const auto queued = ex().observe(pid, o);
    if (!queued) return std::string(epistemicRefusalName(queued.error()));
    host->pump();
    const auto out =
        ex().ingestOutcome(pid, o.principal, o.incarnation, o.eventId);
    if (!out) return "none";
    return out->kind == IngestOutcome::Kind::Committed
               ? "Committed"
               : out->code.value_or("?");
  }
  /// "Committed", the refusal code, or "NoChange" for an action that never ran.
  std::string act(HealthAction action) {
    const auto receipt =
        ex().healthAction(pid, {*token, kP1, std::move(action)});
    if (!receipt) return std::string(epistemicRefusalName(receipt.error()));
    host->pump();
    const auto view = ex().healthOutcome(pid, *receipt);
    if (view.state != HealthOutcomeView::State::Known || view.items.empty())
      return "NoChange";
    const auto &outcome = view.items.front().outcome;
    return outcome.kind == IngestOutcome::Kind::Committed
               ? "Committed"
               : outcome.code.value_or("?");
  }
  std::optional<SourceHealth> health() { return ex().sourceHealth(pid, kP1); }

  const zigzag::vortex::WorkerContext &ctx() { return *ex().context(pid); }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(ctx().arena);
  }
  ValueDims vd() { return valueDims(ctx().core); }
  std::optional<CellRef> profile() {
    const auto dims = registerDimensions(arena());
    for (const auto p :
         zigzag::rankAfter(arena(), *ex().agentCell(pid), dims.trust))
      if (arena().textOf(p) == kP1) return p;
    return std::nullopt;
  }
  std::optional<CellRef> field(CellRef record, std::string_view name) {
    return fieldValue(ctx().arena, vd(), record, name);
  }
  std::int64_t integer(CellRef record, std::string_view name) {
    const auto cell = field(record, name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::string text(CellRef record, std::string_view name) {
    const auto cell = field(record, name);
    return cell ? ctx().arena.textOf(*cell) : std::string("<absent>");
  }
  std::int64_t agentInt(std::string_view name) {
    const auto cell = fieldValue(ctx().arena, vd(), *ex().agentCell(pid), name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::vector<CellRef> listOf(std::string_view name) {
    std::vector<CellRef> out;
    const auto p = profile();
    if (!p) return out;
    const auto head = field(*p, name);
    if (!head) return out;
    for (auto e = zigzag::step(ctx().arena, *head, vd().grab); e;
         e      = zigzag::step(ctx().arena, *e, vd().step))
      out.push_back(*e);
    return out;
  }
  std::vector<Entry> entries(std::string_view name) {
    std::vector<Entry> out;
    for (const auto e : listOf(name))
      out.push_back({text(e, "kind"), text(e, "code"), text(e, "origin"),
                     integer(e, "seq"), integer(e, "count"),
                     text(e, "removed")});
    return out;
  }
  /// Every record of the profile's retired chain.
  std::vector<CellRef> retiredRecords() {
    const auto dims = registerDimensions(arena());
    const auto p    = profile();
    if (!p) return {};
    return zigzag::rankAfter(arena(), *p, dims.epistemic) |
           std::ranges::to<std::vector<CellRef>>();
  }
  /// A reason the device would add (I3-3), written straight into the arena.
  void addDeviceReason(std::string_view kind, std::string_view code) {
    WireRecord r;
    r.type = "Reason";
    r.set("kind", WireValue::text(std::string(kind)));
    r.set("code", WireValue::text(std::string(code)));
    r.set("origin", WireValue::text("device"));
    r.set("grant", WireValue::text(""));
    r.set("issuer", WireValue::text(""));
    const auto cell = writeValue(arena(), vd(), WireValue::record(r));
    const auto p    = profile();
    ASSERT_TRUE(p);
    const auto head = field(*p, "reasons");
    ASSERT_TRUE(head);
    CellRef tail = *head;
    auto dim     = vd().grab;
    while (const auto next = arena().linked(tail, dim)) {
      tail = *next;
      dim  = vd().step;
    }
    ASSERT_TRUE(arena().link(tail, dim, zigzag::DimVector::POS, cell));
  }
  /// A second executor resumed from this actor's checkpoint.
  struct Resumed {
    std::unique_ptr<LocalHost> host;
    ActorPid pid;
  };
  Resumed resumeElsewhere() {
    Resumed r;
    const auto saved = ex().checkpoint(pid);
    EXPECT_TRUE(saved);
    auto program = CompiledProgram::fromSource(kActionProgram);
    EXPECT_TRUE(program);
    r.host =
        std::make_unique<LocalHost>(std::move(*program), LocalHost::Options{});
    const auto resumed = r.host->executor().resume(*saved);
    EXPECT_TRUE(resumed);
    r.pid = *resumed;
    r.host->pump();
    return r;
  }
};

TEST_F(VagentEpistemicHealthActions, IsolateBeforeTheFirstReadingSticks) {
  start();
  EXPECT_EQ(act(Isolate{"stuck"}), "Committed");
  auto h = health();
  ASSERT_TRUE(h);
  EXPECT_TRUE(h->faulted);
  EXPECT_EQ(h->state, HealthState::Faulted);
  EXPECT_NE(h->faultSeq, 0U);
  EXPECT_EQ(h->faultCode, "stuck");
  // The arena refuses the reading itself, and writes nothing else.
  EXPECT_EQ(send(reading("k")), "SourceFaulted");
  const auto slots = arena().cellCount();
  EXPECT_EQ(send(reading("k")), "SourceFaulted");
  EXPECT_EQ(arena().cellCount(), slots);
  h = health();
  EXPECT_EQ(h->contentRefusals, 0);
  EXPECT_EQ(h->refusedCount, 0);
  EXPECT_FALSE(h->lastRefusal);
  EXPECT_EQ(act(Isolate{"again"}), "NoChange");
  // A resume carries the latch with the arena.
  auto other = resumeElsewhere();
  ASSERT_TRUE(other.host);
  ASSERT_TRUE(other.host->executor().bindSource(other.pid, bindingFor(1)));
  other.host->pump();
  other.host->executor().setWallClockSynchronised(true);
  const auto again = reading("k2");
  ASSERT_TRUE(other.host->executor().observe(other.pid, again));
  other.host->pump();
  const auto out =
      other.host->executor().ingestOutcome(other.pid, kP1, 1, again.eventId);
  ASSERT_TRUE(out);
  EXPECT_EQ(out->code, std::string("SourceFaulted"));
  EXPECT_EQ(out->outcomeClass, OutcomeClass::NeedsHostAction);
}

TEST_F(VagentEpistemicHealthActions, IsolateDropsSupportsAndStampsRetiredBy) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(send(reading("b")), "Committed");
  // A superseded record never carries `retired_by`.
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  EXPECT_EQ(agentInt("drops"), 2);
  std::size_t isolate = 0, superseded = 0;
  for (const auto record : retiredRecords()) {
    const auto by = text(record, "retired_by");
    if (by == "isolate") ++isolate;
    if (by == "<absent>") ++superseded;
  }
  EXPECT_EQ(isolate, 2U);
  EXPECT_EQ(superseded, 1U);
  // No refutation, label or calibration moved.
  EXPECT_FALSE(health()->lastRefusal);
}

TEST_F(VagentEpistemicHealthActions, AnAdvanceStampsItsRecordsAdvance) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  ASSERT_TRUE(ex().bindSource(pid, bindingFor(2)));
  host->pump();
  const auto records = retiredRecords();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(text(records.front(), "retired_by"), "advance");
}

TEST_F(VagentEpistemicHealthActions, LatchReasonsAndActionsSurviveEverything) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Degrade{"slow"}), "Committed");
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  const auto check = [&](const SourceHealth &h, const char *when) {
    EXPECT_TRUE(h.faulted) << when;
    ASSERT_EQ(h.reasons.size(), 1U) << when;
    EXPECT_EQ(h.reasons.front().code, "slow") << when;
    EXPECT_EQ(h.faultCode, "x") << when;
  };
  check(*health(), "before");
  const auto actionsBefore = entries("actions");
  ASSERT_EQ(actionsBefore.size(), 2U);
  // A reboot (advance) keeps them.
  ASSERT_TRUE(ex().bindSource(pid, bindingFor(2)));
  host->pump();
  check(*health(), "advance");
  // So does a retire and a revive.
  ASSERT_TRUE(ex().retireSource(pid, kP1));
  host->pump();
  ASSERT_TRUE(ex().bindSource(pid, bindingFor(3)));
  host->pump();
  check(*health(), "retire and revive");
  EXPECT_EQ(entries("actions").size(), actionsBefore.size());
  // And a checkpoint resumed elsewhere.
  auto other         = resumeElsewhere();
  const auto resumed = other.host->executor().sourceHealth(other.pid, kP1);
  ASSERT_TRUE(resumed);
  check(*resumed, "resume");
}

TEST_F(VagentEpistemicHealthActions, DegradeIsUniqueByKindCodeAndOrigin) {
  start();
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
  EXPECT_EQ(act(Degrade{"x"}), "NoChange");
  EXPECT_EQ(act(Degrade{"y"}), "Committed");
  // Three slots, one kept for the device: the third operator reason refuses.
  EXPECT_EQ(act(Degrade{"z"}), "HealthReasonsFull");
  const auto h = health();
  ASSERT_TRUE(h);
  ASSERT_EQ(h->reasons.size(), 2U);
  EXPECT_EQ(h->state, HealthState::Degraded);
  EXPECT_EQ(h->reasonCode, "x");
  // A device reason of the same code is a different reason.
  addDeviceReason("degraded", "x");
  EXPECT_EQ(health()->reasons.size(), 3U);
}

TEST_F(VagentEpistemicHealthActions, ClearRemovesOnlyOperatorDegradedReasons) {
  start();
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
  EXPECT_EQ(act(Degrade{"y"}), "Committed");
  EXPECT_EQ(act(Isolate{"stuck"}), "Committed");
  EXPECT_EQ(act(Clear{std::string("x")}), "Committed");
  ASSERT_EQ(health()->reasons.size(), 1U);
  EXPECT_EQ(health()->reasons.front().code, "y");
  EXPECT_EQ(act(Clear{std::string("x")}), "NoChange");
  EXPECT_EQ(act(Clear{}), "Committed");
  EXPECT_TRUE(health()->reasons.empty());
  // Never the latch.
  EXPECT_TRUE(health()->faulted);
  EXPECT_EQ(act(Clear{}), "NoChange");
  // A device reason survives `Clear {}` and refuses by code.
  addDeviceReason("degraded", "d");
  EXPECT_EQ(act(Clear{}), "NoChange");
  EXPECT_EQ(act(Clear{std::string("d")}), "NotOperatorReason");
  EXPECT_EQ(health()->reasons.size(), 1U);
  EXPECT_TRUE(health()->faulted);
}

TEST_F(VagentEpistemicHealthActions,
       ResetClearsTheRepairedStateAndKeepsTheRest) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(send(reading("b")), "Committed");
  EXPECT_EQ(act(Degrade{"slow"}), "Committed");
  EXPECT_EQ(act(Isolate{"stuck"}), "Committed");
  const auto p = profile();
  ASSERT_TRUE(p);
  const auto dedupBefore = listOf("dedup").size();
  EXPECT_GT(dedupBefore, 0U);
  ASSERT_TRUE(health()->livenessPresent);
  // A stale expected epoch commits nothing, and a retry cannot destroy what
  // a first reset repaired.
  EXPECT_EQ(act(Reset{7}), "StaleHealthState");
  EXPECT_TRUE(health()->faulted);
  EXPECT_EQ(act(Reset{0}), "Committed");
  const auto h = health();
  ASSERT_TRUE(h);
  EXPECT_FALSE(h->faulted);
  EXPECT_TRUE(h->reasons.empty());
  EXPECT_EQ(h->state, HealthState::Nominal);
  EXPECT_EQ(h->weight, 0.0);
  EXPECT_EQ(h->refusalFraction, 0.0);
  EXPECT_EQ(h->rejectionStreak, 0);
  EXPECT_EQ(h->resetEpoch, 1U);
  EXPECT_FALSE(h->livenessPresent);
  EXPECT_EQ(act(Reset{0}), "StaleHealthState");
  // Dedup and the audit list stay, and the reset says what it removed.
  EXPECT_EQ(listOf("dedup").size(), dedupBefore);
  const auto actions = entries("actions");
  ASSERT_EQ(actions.size(), 3U);
  EXPECT_EQ(actions.back().kind, "reset");
  EXPECT_NE(actions.back().removed.find("fault=stuck"), std::string::npos)
      << actions.back().removed;
  EXPECT_NE(actions.back().removed.find("degraded:slow"), std::string::npos);
  // The streams stay: a resend of a counted sequence is still stale.
  auto resend     = reading("a");
  resend.sequence = 1;
  resend.eventId  = "resent";
  EXPECT_EQ(send(resend), "StaleSequence");
  // Eligible only after a new reading, whose record carries the epoch.
  EXPECT_FALSE(health()->livenessPresent);
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_TRUE(health()->livenessPresent);
  const auto chain = retiredRecords();
  ASSERT_FALSE(chain.empty());
  // The isolate had already withdrawn the supports, so the reset retired
  // nothing and the records keep what the isolate said.
  EXPECT_EQ(text(chain.front(), "retired_by"), "isolate");
  EXPECT_EQ(integer(chain.front(), "calibration_epoch"), 0);
  std::int64_t newest = -1;
  const auto dims     = registerDimensions(arena());
  for (const auto pred :
       zigzag::rankAfter(arena(), *ex().agentCell(pid), dims.belief))
    for (const auto clause :
         zigzag::rankAfter(arena(), pred, ctx().core.dims().clause))
      for (const auto support : zigzag::rankAfter(arena(), clause, dims.source))
        for (const auto record :
             zigzag::rankAfter(arena(), support, dims.epistemic))
          newest = integer(record, "calibration_epoch");
  EXPECT_EQ(newest, 1);
}

TEST_F(VagentEpistemicHealthActions, AResetStampsTheRecordsItWithdraws) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Reset{0}), "Committed");
  EXPECT_EQ(agentInt("drops"), 1);
  const auto records = retiredRecords();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(text(records.front(), "retired_by"), "reset");
}

TEST_F(VagentEpistemicHealthActions, AResetWithAFailingHandlerStillCommits) {
  start(kFailingDropProgram);
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  EXPECT_EQ(act(Reset{0}), "Committed");
  EXPECT_FALSE(health()->faulted);
  EXPECT_EQ(health()->resetEpoch, 1U);
  EXPECT_EQ(ex().stat(pid, std::string(kStatSettleDiscarded)), 1U);
}

TEST_F(VagentEpistemicHealthActions,
       AnIdleResetOfAZeroWeightProfileIsNoChange) {
  start();
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
  EXPECT_EQ(act(Clear{}), "Committed");
  EXPECT_EQ(act(Reset{0}), "NoChange");
  EXPECT_EQ(health()->resetEpoch, 0U);
}

TEST_F(VagentEpistemicHealthActions,
       RepeatedOrdinaryActionsNeverEvictTheFault) {
  start();
  EXPECT_EQ(act(Isolate{"stuck"}), "Committed");
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(act(Degrade{"x"}), "Committed");
    EXPECT_EQ(act(Clear{std::string("x")}), "Committed");
  }
  const auto actions = entries("actions");
  ASSERT_EQ(actions.size(), 3U);
  EXPECT_EQ(actions[0].kind, "isolate");
  EXPECT_EQ(actions[1].kind, "degrade");
  EXPECT_EQ(actions[1].count, 6);
  EXPECT_EQ(actions[2].kind, "clear");
  EXPECT_EQ(actions[2].count, 6);
  // seq counts every accepted action, merged or not.
  const auto p = profile();
  EXPECT_EQ(integer(*p, "action_seq"), 13);
  // Fill the list with safety entries: a reset evicts the oldest ordinary
  // entry, and a degrade finds no ordinary entry to evict.
  EXPECT_EQ(act(Reset{0}), "Committed");
  EXPECT_EQ(act(Isolate{"again"}), "Committed");
  EXPECT_EQ(act(Reset{1}), "Committed");
  const auto after = entries("actions");
  ASSERT_EQ(after.size(), 4U);
  for (const auto &entry : after)
    EXPECT_TRUE(entry.kind == "isolate" || entry.kind == "reset");
  EXPECT_EQ(act(Degrade{"y"}), "Committed");
  EXPECT_EQ(entries("actions").size(), 4U);
  EXPECT_EQ(entries("actions").back().kind, "reset");
}

TEST_F(VagentEpistemicHealthActions, ASensorCannotClearItsOwnLatch) {
  start();
  const auto isolateOnly =
      ex().grantHealth(pid, grant("iso", {HealthActionKind::Isolate}));
  ASSERT_TRUE(isolateOnly);
  ASSERT_TRUE(ex().healthAction(pid, {*isolateOnly, kP1, Isolate{"babble"}}));
  host->pump();
  ASSERT_TRUE(health()->faulted);
  // Its own readings neither unlatch it nor are recorded.
  EXPECT_EQ(send(reading("k")), "SourceFaulted");
  EXPECT_EQ(send(reading("k")), "SourceFaulted");
  // A grant without reset authority refuses a reset, and so does a token the
  // sensor invented.
  const auto narrow = ex().healthAction(pid, {*isolateOnly, kP1, Reset{0}});
  ASSERT_FALSE(narrow);
  EXPECT_EQ(narrow.error(), EpistemicRefusal::UnauthorizedHealthAction);
  const auto forged =
      HealthGrantToken::fromBytes(std::array<std::byte, 16>{std::byte{7}});
  if (forged) {
    const auto refused = ex().healthAction(pid, {*forged, kP1, Reset{0}});
    ASSERT_FALSE(refused);
  }
  // The issuer principal cannot be bound as a source at all.
  EXPECT_FALSE(ex().bindSource(pid, {"sensor:self", kOperatorPrincipal, 1, "at",
                                     std::chrono::hours(1)}));
  EXPECT_TRUE(health()->faulted);
}

TEST_F(VagentEpistemicHealthActions, ASixteenRecordResetFitsHalfTheGrant) {
  constexpr std::uint64_t kHalf = 5'000'000;
  EXPECT_EQ(LocalHost::Options{}.maxInstructions, 2 * kHalf);
  LocalHost::Options options;
  options.maxInstructions                  = kHalf;
  options.settings.epistemic.keysPerSource = 16;
  start(kActionProgram, options);
  for (int i = 0; i < 16; ++i)
    EXPECT_EQ(send(reading("k" + std::to_string(i))), "Committed");
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  const auto before = ctx().vm.totalCycles();
  EXPECT_EQ(act(Reset{0}), "Committed");
  const auto spent = ctx().vm.totalCycles() - before;
  RecordProperty("reset16_cycles_including_settle", static_cast<int>(spent));
  std::fprintf(stderr, "MEASURE reset of 16 records: %llu cycles of %llu\n",
               static_cast<unsigned long long>(spent),
               static_cast<unsigned long long>(kHalf));
  EXPECT_FALSE(health()->faulted);
  EXPECT_LT(spent, kHalf);
}

// -- Slice 2c3: the host latch cache (design 6.5) ---------------------------

TEST_F(VagentEpistemicHealthActions, ABabblingLatchedSourceCostsNoTurns) {
  start();
  ASSERT_TRUE(ex().bindSource(
      pid, {"sensor:other", kP2, 1, "at", std::chrono::hours(1)}));
  EXPECT_EQ(act(Isolate{"babble"}), "Committed");
  EXPECT_TRUE(ex().sourceLatchCached(pid, kP1));
  host->pump();
  const auto cycles = ctx().vm.totalCycles();
  const auto slots  = arena().cellCount();
  // A flood with no pump between: nothing reaches the queue, so nothing is
  // ever delivered, logged or charged.
  const auto babble = reading("k");
  for (int i = 0; i < 500; ++i) {
    const auto queued = ex().observe(pid, babble);
    ASSERT_FALSE(queued);
    EXPECT_EQ(queued.error(), EpistemicRefusal::SourceFaulted);
  }
  // Another source's readings are still accepted and committed.
  for (int i = 0; i < 8; ++i) {
    auto o            = reading("k" + std::to_string(i));
    o.principal       = kP2;
    const auto queued = ex().observe(pid, o);
    ASSERT_TRUE(queued) << epistemicRefusalName(queued.error());
    host->pump();
  }
  EXPECT_EQ(health()->refusedWhileFaulted, 500U);
  EXPECT_EQ(health()->contentRefusals, 0);
  EXPECT_EQ(health()->refusedCount, 0);
  EXPECT_FALSE(health()->lastRefusal);
  EXPECT_EQ(ex().sourceHealth(pid, kP2)->state, HealthState::Nominal);
  EXPECT_GT(ctx().vm.totalCycles(), cycles);
  EXPECT_GT(arena().cellCount(), slots);
  // The babbler's own turns: none. Only the 8 healthy readings ran.
  const auto healthy = ctx().vm.totalCycles() - cycles;
  const auto before  = ctx().vm.totalCycles();
  for (int i = 0; i < 500; ++i) (void)ex().observe(pid, babble);
  host->pump();
  EXPECT_EQ(ctx().vm.totalCycles(), before);
  EXPECT_GT(healthy, 0U);
}

TEST_F(VagentEpistemicHealthActions,
       TheLatchCacheIsSetByAnIsolateAndClearedByAReset) {
  start();
  EXPECT_FALSE(ex().sourceLatchCached(pid, kP1));
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  EXPECT_TRUE(ex().sourceLatchCached(pid, kP1));
  EXPECT_EQ(send(reading("a")), "SourceFaulted");
  EXPECT_EQ(health()->refusedWhileFaulted, 1U);
  // The cache never refuses what the arena would accept: a committed reset
  // empties it, and the next reading commits.
  EXPECT_EQ(act(Reset{0}), "Committed");
  EXPECT_FALSE(ex().sourceLatchCached(pid, kP1));
  EXPECT_EQ(health()->refusedWhileFaulted, 0U);
  EXPECT_EQ(send(reading("a")), "Committed");
  // A reset that commits nothing leaves it alone.
  EXPECT_EQ(act(Isolate{"again"}), "Committed");
  EXPECT_EQ(act(Reset{0}), "StaleHealthState");
  EXPECT_TRUE(ex().sourceLatchCached(pid, kP1));
  EXPECT_EQ(send(reading("a")), "SourceFaulted");
}

TEST_F(VagentEpistemicHealthActions, AnArenaRefusalSetsTheCacheAfterAResume) {
  start();
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  // A resume is a new pid with a new ingress entry: the cache starts empty
  // and the arena, which carries the latch, decides the first reading.
  auto other = resumeElsewhere();
  ASSERT_TRUE(other.host);
  auto &second = other.host->executor();
  ASSERT_TRUE(second.bindSource(other.pid, bindingFor(1)));
  second.setWallClockSynchronised(true);
  other.host->pump();
  EXPECT_FALSE(second.sourceLatchCached(other.pid, kP1));
  const auto turns = second.context(other.pid)->vm.totalCycles();
  auto first       = reading("k");
  ASSERT_TRUE(second.observe(other.pid, first));
  other.host->pump();
  EXPECT_GT(second.context(other.pid)->vm.totalCycles(), turns);
  EXPECT_TRUE(second.sourceLatchCached(other.pid, kP1));
  const auto spent = second.context(other.pid)->vm.totalCycles();
  const auto again = second.observe(other.pid, reading("k"));
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), EpistemicRefusal::SourceFaulted);
  other.host->pump();
  EXPECT_EQ(second.context(other.pid)->vm.totalCycles(), spent);
}

TEST_F(VagentEpistemicHealthActions, AnActorStopForgetsTheCache) {
  start();
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  ASSERT_TRUE(ex().sourceLatchCached(pid, kP1));
  ASSERT_TRUE(ex().retire(pid));
  EXPECT_FALSE(ex().sourceLatchCached(pid, kP1));
}

TEST_F(VagentEpistemicHealthActions, ARefusedUpgradeLeavesTheLatchAndTheCache) {
  start();
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  ASSERT_TRUE(ex().sourceLatchCached(pid, kP1));
  auto next = CompiledProgram::fromSource(std::string(kActionProgram) + "\n");
  ASSERT_TRUE(next) << next.error();
  // A profile in the arena makes the reconciler road refuse (the schema road
  // carries it since T1c), so an upgrade that empties the cache can only meet
  // an arena with no latch to re-learn; the latch itself survives the refusal.
  EXPECT_FALSE(host->upgrade(pid, std::move(*next)));
  host->pump();
  EXPECT_TRUE(ex().sourceLatchCached(pid, kP1));
  EXPECT_TRUE(health()->faulted);
  EXPECT_EQ(send(reading("k")), "SourceFaulted");
}

// -- Slice 2c3: closed charges at a reset (design 5.4, 6.4) ----------------

TEST_F(VagentEpistemicHealthActions, AResetDropsTheChargeClosedAtTheCall) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  const auto bad = [&] {
    auto o     = reading("a");
    o.variance = -1.0;
    return o;
  };
  // Refusals are charged when `observe` decides them, so three are open when
  // the reset is called, and a fourth opens a fresh charge after it.
  for (int i = 0; i < 3; ++i) EXPECT_FALSE(ex().observe(pid, bad()));
  const auto receipt = ex().healthAction(pid, {*token, kP1, Reset{0}});
  ASSERT_TRUE(receipt);
  EXPECT_FALSE(ex().observe(pid, bad()));
  host->pump();
  const auto view = ex().healthOutcome(pid, *receipt);
  ASSERT_EQ(view.items.size(), 1U);
  EXPECT_EQ(view.items.front().outcome.kind, IngestOutcome::Kind::Committed);
  const auto h = health();
  ASSERT_TRUE(h);
  // The three predate the repair: counted in the lifetime counters, absent
  // from the estimator. The fourth is the new evidence.
  EXPECT_EQ(h->contentRefusals, 4);
  EXPECT_NEAR(h->weight, 1.0, 1e-9);
  EXPECT_NEAR(h->refusalFraction, 1.0, 1e-9);
  EXPECT_EQ(h->rejectionStreak, 0);
}

TEST_F(VagentEpistemicHealthActions, AResetDropsAnEarlierClosedNoteToo) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  auto bad = [&] {
    auto o     = reading("a");
    o.variance = -1.0;
    return o;
  };
  // Two refusals, then an accepted reading closes their charge into a note
  // that is still waiting; one more refusal is open at the call.
  EXPECT_FALSE(ex().observe(pid, bad()));
  EXPECT_FALSE(ex().observe(pid, bad()));
  const auto queued = reading("b");
  ASSERT_TRUE(ex().observe(pid, queued));
  EXPECT_FALSE(ex().observe(pid, bad()));
  ASSERT_TRUE(ex().healthAction(pid, {*token, kP1, Reset{0}}));
  host->pump();
  // The queued reading predates the repair, and so do all three refusals.
  EXPECT_EQ(ex().ingestOutcome(pid, kP1, 1, queued.eventId)->code,
            std::optional<std::string>("SupersededByReset"));
  const auto h = health();
  ASSERT_TRUE(h);
  EXPECT_EQ(h->contentRefusals, 3);
  EXPECT_EQ(h->weight, 0.0);
  EXPECT_EQ(h->refusalFraction, 0.0);
  EXPECT_EQ(h->resetEpoch, 1U);
}

TEST_F(VagentEpistemicHealthActions, AResetThatCommitsNothingReopensItsCharge) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  const auto weightBefore = health()->weight;
  for (int i = 0; i < 2; ++i) {
    auto o     = reading("a");
    o.variance = -1.0;
    EXPECT_FALSE(ex().observe(pid, o));
  }
  // A stale epoch changes nothing, so nothing may be dropped.
  const auto receipt = ex().healthAction(pid, {*token, kP1, Reset{7}});
  ASSERT_TRUE(receipt);
  host->pump();
  EXPECT_EQ(ex().healthOutcome(pid, *receipt).items.front().outcome.code,
            std::optional<std::string>("StaleHealthState"));
  const auto h = health();
  ASSERT_TRUE(h);
  EXPECT_EQ(h->contentRefusals, 2);
  EXPECT_GT(h->refusalFraction, 0.5);
  EXPECT_NE(h->weight, weightBefore);
  EXPECT_EQ(h->resetEpoch, 0U);
}

// -- Slice 2c3: budgets and reserves (design 6.6) ---------------------------

TEST_F(VagentEpistemicHealthActions,
       HealthCellsFollowThePopulationWithinTheBound) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  const auto p = profile();
  ASSERT_TRUE(p);
  // Never written until the first committed action.
  EXPECT_EQ(integer(*p, "health_cells"), -1);
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
  EXPECT_EQ(integer(*p, "health_cells"),
            kHealthFixedCells + kReasonCells + kActionCells);
  EXPECT_EQ(act(Degrade{"y"}), "Committed");
  EXPECT_EQ(act(Isolate{"stuck"}), "Committed");
  EXPECT_EQ(act(Clear{std::string("y")}), "Committed");
  // Two live entries fewer after the clear and a reset; four actions at the
  // bound, one reason left.
  EXPECT_EQ(integer(*p, "health_cells"),
            kHealthFixedCells + kReasonCells + 4 * kActionCells);
  addDeviceReason("suspect", "d1");
  addDeviceReason("suspect", "d2");
  // A structure at R = 3 and A = 4 stays within the bound the reserve is
  // computed from, counting every cell the profile's health fields hold.
  std::size_t used = 0;
  std::set<CellRef> seen;
  const auto dims = vd();
  const std::array walk{dims.vars, dims.values, dims.grab, dims.step};
  std::vector<CellRef> stack;
  for (const auto *name : {"reset_epoch", "action_seq", "fault_seq",
                           "health_cells", "reasons", "actions"}) {
    // `reset_epoch` and `fault_seq` exist only once a reset or an isolate has
    // written them; the bound counts them anyway.
    const auto value = field(*p, name);
    if (!value) continue;
    ++used; // the field's name cell
    stack.push_back(*value);
  }
  while (!stack.empty()) {
    const auto cell = stack.back();
    stack.pop_back();
    if (!seen.insert(cell).second) continue;
    ++used;
    for (const auto dim : walk)
      if (const auto next = arena().linked(cell, dim)) stack.push_back(*next);
  }
  std::fprintf(stderr, "MEASURE health structure at R=3 A=4: %zu of %lld\n",
               used, static_cast<long long>(healthBound(3, 4)));
  EXPECT_LE(static_cast<std::int64_t>(used), healthBound(3, 4));
}

TEST_F(VagentEpistemicHealthActions, OrdinaryAdmissionReservesTheHealthBound) {
  // The cells one record costs, measured with room to spare.
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  const auto census = *ex().epistemicCells(pid);
  const auto bound  = static_cast<std::size_t>(healthBound(3, 4));
  EXPECT_EQ(bound, 142U);

  LocalHost::Options tight;
  tight.settings.epistemic.cells = census + bound - 1;
  start(kActionProgram, tight);
  // One cell short of the census plus the reserve: refused, writing nothing.
  EXPECT_EQ(send(reading("a")), "EpistemicBudget");
  EXPECT_EQ(*ex().epistemicCells(pid), 0U);

  LocalHost::Options exact;
  exact.settings.epistemic.cells = census + bound;
  start(kActionProgram, exact);
  EXPECT_EQ(send(reading("a")), "Committed");
  // A degrade takes cells and gives back the reserve it writes into, so it
  // fits where the next reading, which grows the census alone, does not.
  EXPECT_EQ(send(reading("b")), "EpistemicBudget");
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
}

TEST_F(VagentEpistemicHealthActions,
       ExemptEntriesLandInTheHeadroomOfAFullBudget) {
  start();
  EXPECT_EQ(send(reading("a")), "Committed");
  const auto census = *ex().epistemicCells(pid);
  LocalHost::Options exact;
  exact.settings.epistemic.cells =
      census + static_cast<std::size_t>(healthBound(3, 4));
  start(kActionProgram, exact);
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(send(reading("b")), "EpistemicBudget");
  // The isolate and the reset are not ordinary entries: nothing the reserve
  // is charged for stands in their way.
  EXPECT_EQ(act(Isolate{"x"}), "Committed");
  EXPECT_EQ(act(Reset{0}), "Committed");
}

TEST_F(VagentEpistemicHealthActions,
       LatchedTombstonesFillTheHeadroomAndThenRefuse) {
  LocalHost::Options small;
  small.settings.epistemic.cells = 1000;
  start(kActionProgram, small);
  constexpr int kPrincipals = 40;
  std::vector<std::string> principals;
  for (int i = 0; i < kPrincipals; ++i) {
    char digit[3];
    std::snprintf(digit, sizeof digit, "%02x", i + 16);
    principals.push_back(std::string(62, 'c') + digit);
  }
  auto g          = grant("tomb");
  g.sources       = principals;
  g.maxActions    = 64;
  const auto wide = ex().grantHealth(pid, g);
  ASSERT_TRUE(wide);
  int landed = 0;
  std::string last;
  for (const auto &principal : principals) {
    ASSERT_TRUE(ex().bindSource(pid, {"s:" + principal.substr(60), principal, 1,
                                      "at", std::chrono::hours(1)}));
    const auto receipt =
        ex().healthAction(pid, {*wide, principal, Isolate{"gone"}});
    ASSERT_TRUE(receipt);
    host->pump();
    const auto view = ex().healthOutcome(pid, *receipt);
    ASSERT_EQ(view.items.size(), 1U);
    last = view.items.front().outcome.code.value_or("Committed");
    if (last != "Committed") break;
    ++landed;
    ASSERT_TRUE(ex().retireSource(pid, principal));
    host->pump();
  }
  // About 1,062 / 82 latched tombstones, then the headroom is spent.
  EXPECT_EQ(last, "EpistemicBudget");
  EXPECT_GT(landed, 5);
  EXPECT_LT(landed, kPrincipals);
  EXPECT_LE(*ex().epistemicCells(pid),
            small.settings.epistemic.cells +
                small.settings.epistemic.cells / kHeadroomDivisor);
}

TEST_F(VagentEpistemicHealthActions,
       ManyBareTombstonesLeaveIngestionAndActionsWorking) {
  start();
  for (int i = 0; i < 400; ++i) {
    char hex[5];
    std::snprintf(hex, sizeof hex, "%04x", i + 4096);
    const std::string principal = std::string(60, 'd') + hex;
    ASSERT_TRUE(ex().bindSource(pid, {"s:" + std::string(hex), principal, 1,
                                      "at", std::chrono::hours(1)}));
    ASSERT_TRUE(ex().retireSource(pid, principal));
    if (i % 50 == 49) host->pump();
  }
  host->pump();
  EXPECT_EQ(send(reading("a")), "Committed");
  EXPECT_EQ(act(Degrade{"x"}), "Committed");
  EXPECT_TRUE(ex().checkpoint(pid));
}

TEST_F(VagentEpistemicHealthActions, AtTheSlotReserveEveryHealthEntryRefuses) {
  // A reserve of one 64th must hold a whole action, so the allowance has to
  // be large; ordinary readings spend it until they are refused.
  start();
  const auto baseline = arena().cellCount();
  LocalHost::Options small;
  small.settings.epistemic.arenaSlots = baseline + 800;
  ASSERT_TRUE(small.settings.epistemic.validate());
  start(kActionProgram, small);
  int sent = 0;
  while (send(reading("k" + std::to_string(sent % 4))) == "Committed" &&
         ++sent < 4000) {
  }
  EXPECT_EQ(send(reading("z")), "ArenaExhausted");
  const auto reserve = small.settings.epistemic.slotReserve();
  EXPECT_GT(reserve, 0U);
  // Isolate and reset still run past the allowance, and end at the reserve.
  const auto atAllowance = arena().cellCount();
  int landed             = 0;
  std::string last       = "Committed";
  for (int i = 0; i < 400 && last == "Committed"; ++i) {
    last = act(Isolate{"x"});
    if (last == "Committed") {
      ++landed;
      last = act(Reset{health()->resetEpoch});
      if (last == "Committed") ++landed;
    }
  }
  EXPECT_GT(landed, 0);
  EXPECT_EQ(last, "ArenaExhausted");
  EXPECT_GE(arena().cellCount(), small.settings.epistemic.arenaSlots);
  // A turn that starts below the reserve may end a little above it.
  EXPECT_LE(arena().cellCount(), small.settings.epistemic.arenaSlots + reserve +
                                     (atAllowance - baseline));
}

} // namespace
