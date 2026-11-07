/**
 * @file vagent_epistemic_admission_test.cpp
 * @brief P19 T1b2: the swap gate, the admission tests in the scope that
 *        pushes, the health commit scope, and the operator path around them.
 *
 * Races are chosen, not hoped for: `SwapObserver` runs code at named points of
 * a racing call, and the real-thread tests park a producer at one with a
 * latch, so every interleaving here is the same on every run.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_ingress.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "epistemic_time.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;
using Point = AgentExecutor::SwapPoint;

constexpr std::string_view kSource = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
event Quit { }
event Ping { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on Ping { adds := adds + 1 }
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

constexpr std::string_view kPlain = R"vagent(
event Quit { }
event Ping { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on Ping { adds := adds + 1 }
}
)vagent";

const std::string kP1(64, '1');
const std::string kOperator(64, 'b');

std::size_t versions = 0;
CompiledProgram nextVersion(std::string_view source) {
  ++versions;
  auto program = CompiledProgram::fromSource(std::string(source) +
                                             std::string(versions, '\n'));
  EXPECT_TRUE(program) << program.error();
  return std::move(*program);
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

class AdmissionTest : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  std::optional<HealthGrantToken> token;
  std::uint64_t seq{0};
  /// The host's steady clock, which the test moves. Atomic: the real-thread
  /// tests read it from the producer.
  std::chrono::steady_clock::time_point origin =
      std::chrono::steady_clock::now();
  std::atomic<std::int64_t> offsetMs{0};

  void SetUp() override { start(kSource); }

  void start(std::string_view source) {
    auto program = CompiledProgram::fromSource(std::string(source));
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.clock        = AgentClock::system();
    options.clock.steady = [this] {
      return origin + std::chrono::milliseconds(offsetMs.load());
    };
    host.emplace(std::move(*program), std::move(options));
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    ex().setWallClockSynchronised(true);
    // Populated state needs a translator, and this one carries nothing: the
    // tests are about the door, not the copy.
    if (source.find("predicate at") == std::string_view::npos) return;
    ASSERT_TRUE(ex().bindSource(pid, binding()));
    auto granted = ex().grantHealth(pid, healthGrant());
    ASSERT_TRUE(granted);
    token.emplace(*granted);
    settle();
  }

  AgentExecutor &ex() { return host->executor(); }
  void settle() {
    host->pump();
    ex().drainNotifications();
  }
  void advance(std::chrono::milliseconds by) { offsetMs += by.count(); }

  static EpistemicSourceGrant binding(std::uint64_t incarnation = 1) {
    return {"sensor:gps", kP1, incarnation, "at", std::chrono::hours(1)};
  }
  static EpistemicHealthGrant healthGrant() {
    EpistemicHealthGrant g{"g1",
                           *HealthIssuer::fromPrincipal(kOperator),
                           {kP1},
                           {HealthActionKind::Isolate,
                            HealthActionKind::Degrade, HealthActionKind::Clear,
                            HealthActionKind::Reset},
                           std::chrono::hours(1),
                           64};
    return g;
  }
  CheckedObservation reading() {
    ++seq;
    CheckedObservation o;
    o.principal   = kP1;
    o.incarnation = 1;
    o.eventId     = "e" + std::to_string(seq);
    o.sequence    = seq;
    o.predicate   = "at";
    o.tuple       = {WireValue::text("self"), WireValue::real(1.0)};
    setTimes(o, wallSeconds() - 0.5, 0.01);
    o.clockBasis = "wall";
    o.frame      = "enu";
    o.units      = "m";
    o.variance   = 0.25;
    return o;
  }
  HealthActionRequest isolate() { return {*token, kP1, Isolate{"stuck"}}; }

  AgentExecutor::QuiescenceReport report() {
    const auto r = ex().quiescence(pid);
    EXPECT_TRUE(r);
    return *r;
  }
  std::shared_ptr<CompiledProgram> retained(std::string_view source = kSource) {
    return host->retainProgram(nextVersion(source));
  }
  static ActorGrant grantFor(const CompiledProgram &program) {
    return {std::string(program.programRef()),
            std::string(program.programRef()),
            10'000'000,
            1U << 22,
            32768,
            4096,
            1U << 20,
            std::chrono::system_clock::now() + std::chrono::hours(1),
            true};
  }
};

// -- the refusal and the gate's own state -----------------------------------

TEST_F(AdmissionTest, AdmissionClosedIsNamedAndRetryable) {
  EXPECT_EQ(epistemicRefusalName(EpistemicRefusal::AdmissionClosed),
            "AdmissionClosed");
  EXPECT_EQ(outcomeClassOf("AdmissionClosed"), OutcomeClass::Retryable);
}

TEST_F(AdmissionTest, APostBeforeTheGateIsCountedAndAPostAfterItIsRefused) {
  // Before the gate: a queued reading is in the post box, so the swap counts
  // it.
  const auto queued = reading();
  ASSERT_TRUE(ex().observe(pid, queued));
  const auto busy = report();
  EXPECT_EQ(busy.posts, 1U);
  EXPECT_EQ(busy.queued, 1U);
  EXPECT_EQ(busy.inFlight, 1U);
  auto r = host->upgrade(pid, retained());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::NotQuiescent);
  EXPECT_EQ(r.error().detail.subject, "posts");
  EXPECT_EQ(r.error().detail.savedCount, 1U);
  settle();

  // After the gate: the door closes between `observe`'s two sections, where a
  // flag read at entry would still say open.
  ex().setSwapObserver([&](Point point, const ActorPid &target) {
    if (point == Point::BetweenObserveSections)
      EXPECT_TRUE(ex().pauseAdmission(target, std::chrono::hours(1)));
  });
  const auto late  = reading();
  const auto raced = ex().observe(pid, late);
  ASSERT_FALSE(raced);
  EXPECT_EQ(raced.error(), EpistemicRefusal::AdmissionClosed);
  ex().setSwapObserver({});
  // Not queued, not logged, not charged, not noted: the device did nothing.
  const auto after = report();
  EXPECT_EQ(after.posts, 0U);
  EXPECT_EQ(after.queued, 0U);
  EXPECT_EQ(after.inFlight, 0U);
  EXPECT_FALSE(ex().ingestOutcome(pid, kP1, 1, late.eventId));

  // Source changes are gated in the scope that pushes them too.
  const auto bind = ex().bindSource(pid, binding());
  ASSERT_FALSE(bind);
  EXPECT_EQ(bind.error(), EpistemicRefusal::AdmissionClosed);
  const auto retire = ex().retireSource(pid, kP1);
  ASSERT_FALSE(retire);
  EXPECT_EQ(retire.error(), EpistemicRefusal::AdmissionClosed);
  EXPECT_EQ(report().posts, 0U);

  // Revocation is deliberately ungated: it must always succeed.
  ex().unbindSource(pid, kP1, "at");
  ex().resumeAdmission(pid);
  const auto unbound = ex().observe(pid, late);
  ASSERT_FALSE(unbound);
  EXPECT_EQ(unbound.error(), EpistemicRefusal::UnknownSource);

  // The same bytes go through once the door reopens.
  ASSERT_TRUE(ex().bindSource(pid, binding()));
  settle();
  ASSERT_TRUE(ex().observe(pid, late));
  settle();
  const auto outcome = ex().ingestOutcome(pid, kP1, 1, late.eventId);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(outcome->kind, IngestOutcome::Kind::Committed);
}

TEST_F(AdmissionTest,
       ThePauseRefusesEveryAdmittingCallAndQueuedReadingsStillRun) {
  ASSERT_TRUE(ex().observe(pid, reading()));
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::hours(1)));
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  EXPECT_EQ(ex().observe(pid, reading()).error(),
            EpistemicRefusal::AdmissionClosed);
  EXPECT_EQ(ex().healthAction(pid, isolate()).error(),
            EpistemicRefusal::AdmissionClosed);
  EXPECT_EQ(ex().healthActionBatch(pid, {*token, {kP1}, Isolate{"x"}}).error(),
            EpistemicRefusal::AdmissionClosed);
  // What was queued before the pause runs.
  settle();
  EXPECT_EQ(report().posts, 0U);
  const auto unknown = ex().pauseAdmission(ActorPid{"x", "y", 9, 9});
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().actor, ActorError::UnknownProcess);
  // Idempotent, and an unknown pid is a no-op.
  EXPECT_EQ(ex().resumeAdmission(pid), &ex());
  EXPECT_EQ(ex().resumeAdmission(pid), &ex());
  EXPECT_EQ(ex().resumeAdmission(ActorPid{"x", "y", 9, 9}), &ex());
  EXPECT_EQ(report().admission, AdmissionState::Open);
  EXPECT_TRUE(ex().observe(pid, reading()));
}

// -- the health commit scope ------------------------------------------------

TEST_F(AdmissionTest, AHealthActionRacingTheGateLeaksNothing) {
  ex().setSwapObserver([&](Point point, const ActorPid &target) {
    if (point == Point::AfterGrantMatch)
      EXPECT_TRUE(ex().pauseAdmission(target, std::chrono::hours(1)));
  });
  // More refusals than `healthPendingPerGrant` (2): a ticket that leaked from
  // any of them would make the later calls meet back-pressure.
  for (int i = 0; i < 4; ++i) {
    const auto single = ex().healthAction(pid, isolate());
    ASSERT_FALSE(single);
    EXPECT_EQ(single.error(), EpistemicRefusal::AdmissionClosed);
    const auto batch =
        ex().healthActionBatch(pid, {*token, {kP1}, Isolate{"stuck"}});
    ASSERT_FALSE(batch);
    EXPECT_EQ(batch.error(), EpistemicRefusal::AdmissionClosed);
    ex().resumeAdmission(pid);
  }
  ex().setSwapObserver({});
  const auto none = report();
  EXPECT_EQ(none.pendingHealth, 0U);
  EXPECT_EQ(none.posts, 0U);
  // Both slots of the grant are still free.
  EXPECT_TRUE(ex().healthAction(pid, isolate()));
  EXPECT_TRUE(ex().healthAction(pid, {*token, kP1, Degrade{"slow"}}));
  EXPECT_EQ(report().pendingHealth, 2U);
  EXPECT_EQ(ex().healthAction(pid, {*token, kP1, Clear{}}).error(),
            EpistemicRefusal::IngressBackpressure);
}

TEST_F(AdmissionTest, AQueuedHealthTicketRefusesTheUpgrade) {
  const auto receipt = ex().healthAction(pid, isolate());
  ASSERT_TRUE(receipt);
  const auto busy = report();
  EXPECT_EQ(busy.pendingHealth, 1U);
  EXPECT_EQ(busy.posts, 1U);
  auto r = host->upgrade(pid, retained());
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::NotQuiescent);
  // The item is in the post box, which the report names first.
  EXPECT_EQ(r.error().detail.subject, "posts");
  // A checkpoint that sees the item sees the ticket: they leave together.
  EXPECT_EQ(ex().checkpoint(pid).error(),
            CheckpointRefusal::HostDeliveryPending);
  settle();
  EXPECT_EQ(report().pendingHealth, 0U);
  EXPECT_EQ(ex().healthOutcome(pid, *receipt).state,
            HealthOutcomeView::State::Known);
  EXPECT_TRUE(host->upgrade(pid, retained()));
}

TEST_F(AdmissionTest, InFlightIsEmptyAtQuiescence) {
  // Up to the per-source share of the queue (8 by default).
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(ex().observe(pid, reading()));
  EXPECT_EQ(report().queued, 6U);
  EXPECT_EQ(report().inFlight, 6U);
  settle();
  const auto drained = report();
  EXPECT_EQ(drained.queued, 0U);
  EXPECT_EQ(drained.inFlight, 0U);
  EXPECT_TRUE(drained.quiet());
}

TEST_F(AdmissionTest,
       OpenChargesAndResetMarksSurviveAndCloseUnderTheNewProgram) {
  // A refusal decided before the swap and a health action that committed.
  auto bad  = reading();
  bad.frame = "ecef";
  ASSERT_FALSE(ex().observe(pid, bad));
  settle();
  const auto receipt = ex().healthAction(pid, {*token, kP1, Degrade{"slow"}});
  ASSERT_TRUE(receipt);
  settle();
  std::vector<std::uint64_t> sequences;
  ex().setHealthAuditSink(
      [&](const HealthAudit &audit) { sequences.push_back(audit.actionSeq); });
  ASSERT_TRUE(ex().healthAction(pid, {*token, kP1, Clear{}}));
  settle();
  ASSERT_EQ(sequences.size(), 1U);
  const auto before = ex().sourceHealth(pid, kP1);
  ASSERT_TRUE(before);

  ASSERT_TRUE(host->upgrade(pid, retained()));
  settle();

  // The outcome ring answers across the swap, and the audit numbering goes on.
  const auto view = ex().healthOutcome(pid, *receipt);
  EXPECT_EQ(view.state, HealthOutcomeView::State::Known);
  ASSERT_TRUE(ex().healthAction(pid, {*token, kP1, Degrade{"again"}}));
  settle();
  ASSERT_EQ(sequences.size(), 2U);
  EXPECT_EQ(sequences[1], sequences[0] + 1);
  // The source is where it was, and a reading under the new program still
  // meets its binding without a rebind.
  const auto after = ex().sourceHealth(pid, kP1);
  ASSERT_TRUE(after);
  EXPECT_EQ(after->resetEpoch, before->resetEpoch);
  EXPECT_EQ(after->refusedCount, before->refusedCount);
  EXPECT_EQ(after->uncountedSinceCommit, before->uncountedSinceCommit);
  ASSERT_TRUE(ex().observe(pid, reading()));
  settle();
  EXPECT_TRUE(report().quiet());
}

// -- the tail, and every exit -----------------------------------------------

TEST_F(AdmissionTest, TheTailRunsUnderTheGate) {
  ASSERT_TRUE(ex().healthAction(pid, isolate()));
  ASSERT_TRUE(ex().observe(pid, reading()));
  settle();
  ASSERT_TRUE(ex().sourceLatchCached(pid, kP1));

  std::optional<AdmissionState> during;
  std::optional<bool> latchedDuring;
  std::optional<EpistemicRefusal> postDuring, healthDuring;
  ex().setSwapObserver([&](Point point, const ActorPid &target) {
    if (point != Point::DuringDrain) return;
    during        = ex().quiescence(target)->admission;
    latchedDuring = ex().sourceLatchCached(target, kP1);
    postDuring    = ex().observe(target, reading()).error();
    healthDuring  = ex().healthAction(target, isolate()).error();
  });
  ASSERT_TRUE(host->upgrade(pid, retained()));
  ASSERT_TRUE(during);
  EXPECT_EQ(*during, AdmissionState::Swapping);
  // The latch cache was cleared before the first post-swap drain, and nothing
  // was admitted against the cache that predates the commit.
  EXPECT_EQ(latchedDuring, false);
  EXPECT_EQ(postDuring, EpistemicRefusal::AdmissionClosed);
  EXPECT_EQ(healthDuring, EpistemicRefusal::AdmissionClosed);
  EXPECT_EQ(report().admission, AdmissionState::Open);
}

TEST_F(AdmissionTest, AdmissionReopensOnEveryExit) {
  // After a refusal.
  ASSERT_TRUE(ex().observe(pid, reading()));
  ASSERT_FALSE(host->upgrade(pid, retained()));
  EXPECT_EQ(report().admission, AdmissionState::Open);
  settle();

  // After an exception in the callback: the translator throws, the upgrade
  // refuses and the old actor stands.
  ASSERT_TRUE(ex().observe(pid, reading()));
  settle();
  ex().setOwnedStateTranslator(
      [](const TranslationPlan &, const ApplyRequest &)
          -> std::expected<TranslationReport, TransferRefusal> {
        throw std::runtime_error("translator");
      });
  const auto thrown = host->upgrade(pid, retained());
  ASSERT_FALSE(thrown);
  EXPECT_EQ(thrown.error().actor, ActorError::ResourceLimit);
  EXPECT_EQ(report().admission, AdmissionState::Open);
  ex().setOwnedStateTranslator(defaultEpistemicTranslator());
  EXPECT_TRUE(ex().observe(pid, reading()));
  settle();

  // After a commit.
  ASSERT_TRUE(host->upgrade(pid, retained()));
  EXPECT_EQ(report().admission, AdmissionState::Open);

  // After an exception in the tail: the commit stands and the call succeeds.
  ex().setSwapObserver([](Point point, const ActorPid &) {
    if (point == Point::AfterCommit) throw std::runtime_error("tail");
  });
  const auto tail = host->upgrade(pid, retained());
  EXPECT_TRUE(tail) << (tail ? "" : toString(tail.error()));
  ex().setSwapObserver({});
  EXPECT_EQ(report().admission, AdmissionState::Open);
  EXPECT_TRUE(ex().observe(pid, reading()));
}

TEST_F(AdmissionTest, ThePauseIsKeptAcrossAnUpgradeAndTheGateLeavesItAlone) {
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::hours(1)));
  // A paused actor still upgrades; the swap gate is separate from the pause.
  std::optional<AdmissionState> during;
  ex().setSwapObserver([&](Point point, const ActorPid &target) {
    if (point == Point::DuringDrain)
      during = ex().quiescence(target)->admission;
  });
  ASSERT_TRUE(host->upgrade(pid, retained()));
  EXPECT_EQ(during, AdmissionState::Swapping);
  EXPECT_EQ(report().admission, AdmissionState::Paused);
}

// -- the mailbox ------------------------------------------------------------

TEST_F(AdmissionTest, TheMailboxRefusesAnArbitratingUpgrade) {
  Envelope ping;
  ping.kind = std::string(kKindEvent);
  ping.type = "Ping";
  ASSERT_TRUE(host->send(pid, ping));
  EXPECT_EQ(report().mailbox, 1U);
  const auto held = retained();
  auto r          = host->upgrade(pid, held);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::NotQuiescent);
  EXPECT_EQ(r.error().detail.subject, "mailbox");
  EXPECT_EQ(r.error().detail.savedCount, 1U);
  // The reconciler road reads the same count.
  r = host->upgrade(pid, nextVersion(kSource));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().detail.subject, "mailbox");
  settle();
  EXPECT_EQ(report().mailbox, 0U);
  EXPECT_TRUE(host->upgrade(pid, held));
}

TEST_F(AdmissionTest, ANonArbitratingAgentWithAMailboxUpgradesAsBefore) {
  start(kPlain);
  Envelope ping;
  ping.kind = std::string(kKindEvent);
  ping.type = "Ping";
  ASSERT_TRUE(host->send(pid, ping));
  EXPECT_EQ(report().mailbox, 1U);
  EXPECT_TRUE(host->upgrade(pid, retained(kPlain)));
}

// -- the pause expires and is renewed ---------------------------------------

TEST_F(AdmissionTest, ThePauseExpiresAndIsRenewedForTheDrain) {
  // Lapses by itself at the setting's default, without any call.
  ASSERT_TRUE(ex().pauseAdmission(pid));
  advance(std::chrono::milliseconds(kDefaultAdmissionPauseMs - 1000));
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  advance(std::chrono::milliseconds(2000));
  EXPECT_EQ(report().admission, AdmissionState::Open);
  EXPECT_TRUE(ex().observe(pid, reading()));
  settle();

  // A pause never shortens: the longer deadline stands.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::seconds(100)));
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::seconds(1)));
  advance(std::chrono::seconds(50));
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  ex().resumeAdmission(pid);

  // A drain longer than the default ttl: `upgradeQuiescing` renews the pause
  // over its timeout, so the clock passing the old deadline mid-drain does not
  // reopen admission under it.
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(ex().observe(pid, reading()));
  // A ping in the mailbox makes the drain dispatch an event, which is where
  // the turn observer moves the clock.
  Envelope ping;
  ping.kind = std::string(kKindEvent);
  ping.type = "Ping";
  ASSERT_TRUE(host->send(pid, ping));
  ASSERT_TRUE(ex().pauseAdmission(pid));
  std::atomic<bool> moved{false};
  ex().setTurnObserver([&](const ActorPid &,
                           const zigzag::vortex::WorkerContext &,
                           zigzag::CellRef) {
    if (!moved.exchange(true)) advance(std::chrono::seconds(60));
  });
  const auto upgraded =
      host->upgradeQuiescing(pid, retained(), std::chrono::seconds(300));
  EXPECT_TRUE(upgraded) << (upgraded ? "" : toString(upgraded.error()));
  EXPECT_TRUE(moved);
  ex().setTurnObserver({});
  EXPECT_EQ(report().admission, AdmissionState::Open);
  EXPECT_TRUE(ex().observe(pid, reading()));
}

TEST_F(AdmissionTest, AnExplicitTtlIsClampedToTheSettingsRange) {
  // A forgotten long pause lapses as the longest configurable one would.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::hours(9999)));
  advance(std::chrono::milliseconds(kMaxAdmissionPauseMs - 1000));
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  advance(std::chrono::milliseconds(2000));
  EXPECT_EQ(report().admission, AdmissionState::Open);
  // And a short one holds for at least the shortest.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::milliseconds(1)));
  advance(std::chrono::milliseconds(kMinAdmissionPauseMs / 2));
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  advance(std::chrono::milliseconds(kMinAdmissionPauseMs));
  EXPECT_EQ(report().admission, AdmissionState::Open);
}

TEST_F(AdmissionTest, RequirePausedRefusesAPauseThatLapsed) {
  settle();
  const auto held = retained();
  // Clamped up to the setting's floor of 1 s, then lapsed.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::milliseconds(10)));
  advance(std::chrono::seconds(2));
  auto r = ex().upgrade(pid, held, grantFor(*held), true);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::NotQuiescent);
  EXPECT_EQ(r.error().detail.subject, "admission");
  EXPECT_EQ(r.error().detail.savedCount,
            static_cast<std::uint64_t>(AdmissionState::Open));
  // The same call without the requirement goes through, and so does one that
  // does hold a pause.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::hours(1)));
  r = ex().upgrade(pid, held, grantFor(*held), true);
  EXPECT_TRUE(r) << (r ? "" : toString(r.error()));
}

TEST_F(AdmissionTest, UpgradeQuiescingKeepsAnOperatorsLongerPause) {
  // DIST m7: the call reopens only what it opened.
  ASSERT_TRUE(ex().pauseAdmission(pid, std::chrono::minutes(5)));
  const auto upgraded =
      host->upgradeQuiescing(pid, retained(), std::chrono::seconds(30));
  ASSERT_TRUE(upgraded) << toString(upgraded.error());
  EXPECT_EQ(report().admission, AdmissionState::Paused);
  advance(std::chrono::minutes(6));
  EXPECT_EQ(report().admission, AdmissionState::Open);
}

TEST_F(AdmissionTest, UpgradeQuiescingWithoutAPauseReopens) {
  const auto upgraded =
      host->upgradeQuiescing(pid, retained(), std::chrono::seconds(30));
  ASSERT_TRUE(upgraded) << toString(upgraded.error());
  EXPECT_EQ(report().admission, AdmissionState::Open);
}

TEST_F(AdmissionTest, UpgradeQuiescingDrainsAndUpgrades) {
  for (int i = 0; i < 6; ++i) ASSERT_TRUE(ex().observe(pid, reading()));
  EXPECT_FALSE(report().quiet());
  const auto upgraded =
      host->upgradeQuiescing(pid, retained(), std::chrono::seconds(30));
  ASSERT_TRUE(upgraded) << toString(upgraded.error());
  EXPECT_EQ(report().admission, AdmissionState::Open);
  EXPECT_TRUE(report().quiet());
  ASSERT_TRUE(ex().observe(pid, reading()));
  settle();
}

TEST_F(AdmissionTest, UpgradeQuiescingReopensAdmissionWhenItRefuses) {
  // The pre-flight refuses a changed policy before the drain matters, and the
  // pause `upgradeQuiescing` opened does not outlive the call.
  auto changed = CompiledProgram::fromSource([&] {
    std::string source(kSource);
    source.replace(source.find("10s"), 3, "12s");
    return source;
  }());
  ASSERT_TRUE(changed);
  const auto r = host->upgradeQuiescing(
      pid, host->retainProgram(std::move(*changed)), std::chrono::seconds(30));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PolicyChanged);
  EXPECT_EQ(report().admission, AdmissionState::Open);
}

// -- real threads, parked at a chosen point ---------------------------------

TEST_F(AdmissionTest, AnObserveParkedBetweenItsSectionsIsRefusedByTheSwap) {
  const auto owner = std::this_thread::get_id();
  std::latch parked(1), go(1);
  std::optional<std::expected<TransportReceipt, EpistemicRefusal>> result;
  std::thread producer;
  ex().setSwapObserver([&](Point point, const ActorPid &) {
    if (std::this_thread::get_id() != owner) {
      if (point == Point::BetweenObserveSections) {
        parked.count_down();
        go.wait();
      }
    } else if (point == Point::DuringDrain) {
      // The swap is in its tail with `swapping` set; let the producer reach
      // its second section and wait for it, so the gate is held throughout.
      go.count_down();
      producer.join();
    }
  });
  const auto parkedReading = reading();
  producer =
      std::thread([&] { result.emplace(ex().observe(pid, parkedReading)); });
  parked.wait();
  // Nothing of the producer's is in the post box yet: the swap finds quiet.
  EXPECT_TRUE(report().quiet());
  ASSERT_TRUE(host->upgrade(pid, retained()));
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_EQ(result->error(), EpistemicRefusal::AdmissionClosed);
  const auto after = report();
  EXPECT_EQ(after.posts, 0U);
  EXPECT_EQ(after.queued, 0U);
  EXPECT_FALSE(ex().ingestOutcome(pid, kP1, 1, parkedReading.eventId));
  EXPECT_EQ(after.admission, AdmissionState::Open);
}

TEST_F(AdmissionTest, AHealthActionParkedAfterItsGrantMatchIsRefusedByTheSwap) {
  const auto owner = std::this_thread::get_id();
  std::latch parked(1), go(1);
  std::optional<std::expected<HealthReceipt, EpistemicRefusal>> result;
  std::thread producer;
  ex().setSwapObserver([&](Point point, const ActorPid &) {
    if (std::this_thread::get_id() != owner) {
      if (point == Point::AfterGrantMatch) {
        parked.count_down();
        go.wait();
      }
    } else if (point == Point::DuringDrain) {
      go.count_down();
      producer.join();
    }
  });
  producer =
      std::thread([&] { result.emplace(ex().healthAction(pid, isolate())); });
  parked.wait();
  ASSERT_TRUE(host->upgrade(pid, retained()));
  ASSERT_TRUE(result);
  ASSERT_FALSE(*result);
  EXPECT_EQ(result->error(), EpistemicRefusal::AdmissionClosed);
  // No ticket and no queued item: nothing of the call survives its refusal.
  const auto after = report();
  EXPECT_EQ(after.pendingHealth, 0U);
  EXPECT_EQ(after.posts, 0U);
  ex().setSwapObserver({});
  EXPECT_TRUE(ex().healthAction(pid, isolate()));
}

} // namespace
