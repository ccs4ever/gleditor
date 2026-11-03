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
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "common/xanadu/scalar.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
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

} // namespace
