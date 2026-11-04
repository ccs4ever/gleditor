/**
 * @file vagent_epistemic_grants_test.cpp
 * @brief P19 I3-2 slice 2c1: health grants, the host API for health actions,
 *        the post order and the `healthEntry` stub.
 */
#include <gtest/gtest.h>

#include <poll.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/vagent/runtime/agent_executor_internal.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;

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
const std::string kOperator(64, 'b');

class VagentEpistemicGrants : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  std::uint64_t seq{0};

  void SetUp() override {
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    host.emplace(std::move(*program), LocalHost::Options{});
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    ASSERT_TRUE(host->executor().bindSource(
        pid, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)}));
  }

  AgentExecutor &ex() { return host->executor(); }

  EpistemicHealthGrant
  grant(std::string id = "g1", std::vector<std::string> sources = {kP1},
        std::chrono::steady_clock::duration lifetime = std::chrono::hours(1),
        std::uint32_t maxActions                     = 8) {
    EpistemicHealthGrant g{
        std::move(id),      *HealthIssuer::fromPrincipal(kOperator),
        std::move(sources), {},
        lifetime,           maxActions};
    g.actions.push_back(HealthActionKind::Isolate);
    g.actions.push_back(HealthActionKind::Degrade);
    g.actions.push_back(HealthActionKind::Clear);
    g.actions.push_back(HealthActionKind::Reset);
    return g;
  }

  HealthActionRequest request(const HealthGrantToken &token,
                              std::string principal, HealthAction action) {
    return {token, std::move(principal), std::move(action)};
  }

  std::optional<std::string> refusalOf(const HealthOutcomeView &view) {
    if (view.items.empty()) return std::nullopt;
    return view.items.front().outcome.code;
  }

  static double wallSeconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  CheckedObservation reading(std::uint64_t sequence, double x,
                             std::string principal     = kP1,
                             std::uint64_t incarnation = 1) {
    CheckedObservation o;
    o.principal           = std::move(principal);
    o.incarnation         = incarnation;
    o.eventId             = "e" + std::to_string(sequence);
    o.sequence            = sequence;
    o.predicate           = "at";
    o.tuple               = {WireValue::text("k"), WireValue::real(x)};
    o.acquiredAt          = wallSeconds() - 5.0 + 0.01 * double(sequence);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
  }

  /// Make the profile exist, so the source's health has a snapshot.
  void commitOne(LocalHost &h, const ActorPid &p, std::uint64_t sequence) {
    h.pump();
    const auto o = reading(sequence, 1.0);
    ASSERT_TRUE(h.executor().observe(p, o));
    h.pump();
    const auto out = h.executor().ingestOutcome(p, o.principal, 1, o.eventId);
    ASSERT_TRUE(out);
    ASSERT_EQ(out->kind, IngestOutcome::Kind::Committed);
  }

  /// A fresh host with the same program and its own agent, for the tests
  /// that need a second executor (resume) or other settings.
  static std::optional<LocalHost> makeHost(LocalHost::Options options = {}) {
    auto program = CompiledProgram::fromSource(kProgram);
    if (!program) return std::nullopt;
    return std::optional<LocalHost>(std::in_place, std::move(*program),
                                    std::move(options));
  }
};

TEST_F(VagentEpistemicGrants, GrantRefusals) {
  const auto bad = [&](EpistemicHealthGrant g) {
    const auto r = ex().grantHealth(pid, g);
    EXPECT_FALSE(r);
    return r ? EpistemicRefusal::NotArbitrated : r.error();
  };
  EXPECT_EQ(bad(grant("")), EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("bad id!")), EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {})), EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {"short"})), EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {kP1, kP1})), EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {kP1}, std::chrono::seconds(0))),
            EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {kP1}, std::chrono::hours(25))),
            EpistemicRefusal::InvalidHealthGrant);
  EXPECT_EQ(bad(grant("g", {kP1}, std::chrono::hours(1), 0)),
            EpistemicRefusal::InvalidHealthGrant);
  auto noActions = grant();
  noActions.actions.clear();
  EXPECT_EQ(bad(noActions), EpistemicRefusal::InvalidHealthGrant);
  auto dup = grant();
  dup.actions.clear();
  dup.actions.push_back(HealthActionKind::Reset);
  dup.actions.push_back(HealthActionKind::Reset);
  EXPECT_EQ(bad(dup), EpistemicRefusal::InvalidHealthGrant);
}

TEST_F(VagentEpistemicGrants, DuplicateGrantIdAndUnknownActor) {
  ASSERT_TRUE(ex().grantHealth(pid, grant("g1")));
  const auto again = ex().grantHealth(pid, grant("g1"));
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), EpistemicRefusal::InvalidHealthGrant);

  const auto other = ex().grantHealth(ActorPid{}, grant("g2"));
  ASSERT_FALSE(other);
  EXPECT_EQ(other.error(), EpistemicRefusal::UnknownActor);
}

TEST_F(VagentEpistemicGrants, IssuerCannotBeABoundSourceOrNamedSource) {
  auto self        = grant("g1", {kP1, kOperator});
  const auto named = ex().grantHealth(pid, self);
  ASSERT_FALSE(named);
  EXPECT_EQ(named.error(), EpistemicRefusal::SelfCertification);

  ASSERT_TRUE(ex().bindSource(
      pid, {"sensor:op", kOperator, 1, "at", std::chrono::hours(1)}));
  const auto bound = ex().grantHealth(pid, grant("g2"));
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error(), EpistemicRefusal::SelfCertification);
}

TEST_F(VagentEpistemicGrants, TokensAreDistinctAndNonZero) {
  const auto a = ex().grantHealth(pid, grant("g1"));
  const auto b = ex().grantHealth(pid, grant("g2"));
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  EXPECT_NE(a->digest(), b->digest());
  const std::array<std::byte, 16> zero{};
  EXPECT_FALSE(HealthGrantToken::fromBytes(zero));
}

TEST_F(VagentEpistemicGrants, WrongTokenAndUnnamedSourceAndKindAreRefused) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);

  std::array<std::byte, 16> bytes{};
  bytes[0]         = std::byte{1};
  const auto wrong = *HealthGrantToken::fromBytes(bytes);
  const auto r1    = ex().healthAction(pid, request(wrong, kP1, Isolate{"x"}));
  ASSERT_FALSE(r1);
  EXPECT_EQ(r1.error(), EpistemicRefusal::UnauthorizedHealthAction);

  const auto r2 = ex().healthAction(pid, request(*token, kP2, Isolate{"x"}));
  ASSERT_FALSE(r2);
  EXPECT_EQ(r2.error(), EpistemicRefusal::UnauthorizedHealthAction);

  auto isolateOnly = grant("g2");
  isolateOnly.actions.clear();
  isolateOnly.actions.push_back(HealthActionKind::Isolate);
  const auto t2 = ex().grantHealth(pid, isolateOnly);
  ASSERT_TRUE(t2);
  const auto r3 = ex().healthAction(pid, request(*t2, kP1, Degrade{"x"}));
  ASSERT_FALSE(r3);
  EXPECT_EQ(r3.error(), EpistemicRefusal::UnauthorizedHealthAction);
}

TEST_F(VagentEpistemicGrants, RevokedTokenIsRefusedAndCountsCancelled) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  const auto revoked = ex().revokeHealthGrant(pid, "g1");
  ASSERT_TRUE(revoked);
  const auto r = ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), EpistemicRefusal::UnauthorizedHealthAction);

  const auto again = ex().revokeHealthGrant(pid, "g1");
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), EpistemicRefusal::UnknownGrant);
}

TEST_F(VagentEpistemicGrants, StubAnswersHealthUnavailableAndChangesNothing) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  const auto receipt =
      ex().healthAction(pid, request(*token, kP1, Isolate{"operator"}));
  ASSERT_TRUE(receipt);
  host->pump();
  const auto view = ex().healthOutcome(pid, *receipt);
  ASSERT_EQ(view.state, HealthOutcomeView::State::Known);
  ASSERT_EQ(view.items.size(), 1U);
  EXPECT_EQ(view.items.front().outcome.kind, IngestOutcome::Kind::Refused);
  EXPECT_EQ(refusalOf(view), std::string(kTurnResultHealthUnavailable));
}

TEST_F(VagentEpistemicGrants, AuditSinkSeesEveryCommittedOrRefusedAction) {
  std::vector<HealthAudit> audits;
  ex().setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  ASSERT_TRUE(ex().healthAction(pid, request(*token, kP1, Isolate{"op"})));
  host->pump();
  ASSERT_EQ(audits.size(), 1U);
  EXPECT_EQ(audits.front().principal, kP1);
  // The audit id is the grant's label plus an instance suffix.
  EXPECT_TRUE(audits.front().grantId.starts_with("g1@"))
      << audits.front().grantId;
  EXPECT_EQ(audits.front().issuer, kOperator);
  EXPECT_EQ(audits.front().kind, HealthActionKind::Isolate);
}

TEST_F(VagentEpistemicGrants, ThrowingAuditSinkDoesNotBreakTheTurn) {
  ex().setHealthAuditSink([](const HealthAudit &) { throw 1; });
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  const auto receipt =
      ex().healthAction(pid, request(*token, kP1, Isolate{"op"}));
  ASSERT_TRUE(receipt);
  host->pump();
  EXPECT_EQ(ex().healthOutcome(pid, *receipt).state,
            HealthOutcomeView::State::Known);
}

TEST_F(VagentEpistemicGrants, UnknownReceiptIsUnknown) {
  const auto view = ex().healthOutcome(pid, HealthReceipt{"nope", 99});
  EXPECT_EQ(view.state, HealthOutcomeView::State::Unknown);
}

using Item = AgentExecutor::PostBox::Item;

Item ordinary(std::string tag) {
  Item i;
  i.percept = true;
  i.type    = std::move(tag);
  return i;
}

Item control(std::string tag) {
  Item i;
  QueuedHealth q;
  q.principal = std::move(tag);
  i.health    = std::move(q);
  return i;
}

std::vector<std::string> order(AgentExecutor::PostBox &box) {
  std::vector<std::string> out;
  for (auto &i : box.take())
    out.push_back(i.health ? "C" + i.health->principal : "O" + i.type);
  return out;
}

TEST(VagentEpistemicPostOrder, AnIsolateQueuedBehindAHeldItemRunsFirst) {
  AgentExecutor::PostBox box;
  box.push(ordinary("held"));
  box.push(ordinary("later"));
  box.pushControl(control("isolate1"));
  box.pushControl(control("isolate2"));
  EXPECT_EQ(order(box), (std::vector<std::string>{"Cisolate1", "Cisolate2",
                                                  "Oheld", "Olater"}));
}

TEST(VagentEpistemicPostOrder, PutBackKeepsControlItemsFirst) {
  AgentExecutor::PostBox box;
  box.push(ordinary("new"));
  box.pushControl(control("fresh"));
  std::vector<Item> kept;
  kept.push_back(ordinary("k1"));
  kept.push_back(control("kc"));
  kept.push_back(ordinary("k2"));
  box.putBack(std::move(kept));
  // Kept items return ahead of the new ones, and control still leads, in
  // the order each group had.
  EXPECT_EQ(order(box),
            (std::vector<std::string>{"Ckc", "Cfresh", "Ok1", "Ok2", "Onew"}));
}

TEST(VagentEpistemicPostOrder, OnlyPushAndPushControlWakeTheReactor) {
  AgentExecutor::PostBox box;
  const auto fd = box.descriptor();
  ASSERT_TRUE(fd);
  const auto readable = [&] {
    pollfd probe{*fd, POLLIN, 0};
    return ::poll(&probe, 1, 0) == 1 && (probe.revents & POLLIN) != 0;
  };
  EXPECT_FALSE(readable());
  std::vector<Item> kept;
  kept.push_back(ordinary("k"));
  box.putBack(std::move(kept));
  EXPECT_FALSE(readable()) << "a post that stays kept must not spin the loop";
  box.pushControl(control("c"));
  EXPECT_TRUE(readable());
  box.drainDescriptor();
  EXPECT_FALSE(readable());
  box.push(ordinary("o"));
  EXPECT_TRUE(readable());
}

TEST(VagentEpistemicResetFloor,
     TheSnapshotDecidesWhichQueuedReadingIsSuperseded) {
  AgentExecutor::Ingress ingress;
  const AgentExecutor::Ingress::SourceKey source{"pid", kP1};
  // A reading queued before the call carries generation 0.
  const auto before = ingress.resetMarks[source].requested;
  const auto call   = ingress.requestReset(source);
  // One queued between the call and its commit carries the call's own.
  const auto between = ingress.resetMarks[source].requested;
  EXPECT_EQ(before, 0U);
  EXPECT_EQ(call, 1U);
  EXPECT_EQ(between, call);

  // Nothing is superseded until a reset commits: a failed, revoked, stubbed
  // or NoChange reset leaves both readings in the queue.
  EXPECT_FALSE(ingress.supersededByReset(source, before));
  EXPECT_FALSE(ingress.supersededByReset(source, between));

  ingress.commitReset(source, call);
  EXPECT_TRUE(ingress.supersededByReset(source, before));
  EXPECT_FALSE(ingress.supersededByReset(source, between));

  // A late commit of an older call never lowers the floor.
  ingress.commitReset(source, 0);
  EXPECT_TRUE(ingress.supersededByReset(source, before));
  EXPECT_FALSE(ingress.supersededByReset({"pid", kP2}, before));

  // The floor is erased with the actor.
  ingress.forget("pid");
  EXPECT_FALSE(ingress.supersededByReset(source, before));
}

TEST_F(VagentEpistemicGrants, AStubbedResetSupersedesNoQueuedReading) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  commitOne(*host, pid, 1);

  // The reading is queued first and the reset is posted behind it; control
  // items lead, so the reset's turn runs while the reading is in flight.
  ASSERT_TRUE(ex().observe(pid, reading(2, 1.0)));
  const auto reset = ex().healthAction(pid, request(*token, kP1, Reset{0}));
  ASSERT_TRUE(reset);
  host->pump();
  EXPECT_EQ(refusalOf(ex().healthOutcome(pid, *reset)),
            std::string(kTurnResultHealthUnavailable));
  // The stub committed nothing, so no floor was raised for the reading.
  const auto out = ex().ingestOutcome(pid, kP1, 1, "e2");
  ASSERT_TRUE(out);
  EXPECT_EQ(out->kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicGrants, ARevokedTokenQueuedAheadOfItsTurnIsRefused) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  std::vector<HealthAudit> audits;
  ex().setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  host->pump();
  const auto receipt =
      ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_TRUE(receipt);
  EXPECT_EQ(ex().healthOutcome(pid, *receipt).state,
            HealthOutcomeView::State::Pending);
  const auto cancelled = ex().revokeHealthGrant(pid, "g1");
  ASSERT_TRUE(cancelled);
  EXPECT_EQ(*cancelled, 1U);
  host->pump();
  const auto view = ex().healthOutcome(pid, *receipt);
  ASSERT_EQ(view.state, HealthOutcomeView::State::Known);
  EXPECT_EQ(refusalOf(view), std::string(kTurnResultUnauthorizedHealthAction));
  // Refused at the turn, so the stub never ran, and the refusal is audited.
  ASSERT_EQ(audits.size(), 1U);
  EXPECT_EQ(audits.front().outcome.code,
            std::string(kTurnResultUnauthorizedHealthAction));
}

TEST_F(VagentEpistemicGrants, ARefusedActionChangesNothing) {
  std::vector<HealthAudit> audits;
  ex().setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  auto one = grant("g1", {kP1}, std::chrono::hours(1), 1);
  one.actions.clear();
  one.actions.push_back(HealthActionKind::Isolate);
  const auto token = ex().grantHealth(pid, one);
  ASSERT_TRUE(token);

  // Refused for kind, source, a bad code and a bad principal: no receipt is
  // spent, nothing is queued, nothing is audited and the allowance is whole.
  EXPECT_FALSE(ex().healthAction(pid, request(*token, kP1, Degrade{"x"})));
  EXPECT_FALSE(ex().healthAction(pid, request(*token, kP2, Isolate{"x"})));
  EXPECT_FALSE(
      ex().healthAction(pid, request(*token, kP1, Isolate{"bad code!"})));
  host->pump();
  EXPECT_TRUE(audits.empty());

  const auto first = ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_TRUE(first);
  EXPECT_EQ(first->counter, 1U) << "refusals spend no receipt counter";
  host->pump();
  EXPECT_EQ(refusalOf(ex().healthOutcome(pid, *first)),
            std::string(kTurnResultHealthUnavailable));

  // maxActions is 1 and the run spent it: the next request is refused.
  const auto spent = ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_FALSE(spent);
  EXPECT_EQ(spent.error(), EpistemicRefusal::UnauthorizedHealthAction);
  EXPECT_EQ(audits.size(), 1U);
}

TEST_F(VagentEpistemicGrants, QuotaExhaustionCannotKeepASafetyActionOut) {
  const auto defaultCap = AgentSettings{}.epistemic.healthPending;
  ASSERT_EQ(defaultCap, 16);
  std::vector<HealthGrantToken> tokens;
  for (int i = 0; i < 9; ++i) {
    const auto t = ex().grantHealth(pid, grant("g" + std::to_string(i)));
    ASSERT_TRUE(t) << i;
    tokens.push_back(*t);
  }
  host->pump();
  // Six grants fill every slot a degrade or clear may take (12 of 16).
  for (int i = 0; i < 6; ++i)
    for (int k = 0; k < 2; ++k)
      ASSERT_TRUE(ex().healthAction(pid, request(tokens[i], kP1, Degrade{"d"})))
          << i << "/" << k;
  const auto blocked =
      ex().healthAction(pid, request(tokens[6], kP1, Degrade{"d"}));
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error(), EpistemicRefusal::IngressBackpressure);
  const auto clear = ex().healthAction(pid, request(tokens[6], kP1, Clear{}));
  ASSERT_FALSE(clear);
  EXPECT_EQ(clear.error(), EpistemicRefusal::IngressBackpressure);

  // The four reserved slots still take isolates and a reset.
  EXPECT_TRUE(ex().healthAction(pid, request(tokens[6], kP1, Isolate{"i"})));
  EXPECT_TRUE(ex().healthAction(pid, request(tokens[6], kP1, Reset{0})));
  EXPECT_TRUE(ex().healthAction(pid, request(tokens[7], kP1, Isolate{"i"})));
  EXPECT_TRUE(ex().healthAction(pid, request(tokens[7], kP1, Isolate{"i"})));
  // The queue as a whole is bounded: the seventeenth is refused.
  const auto full =
      ex().healthAction(pid, request(tokens[8], kP1, Isolate{"i"}));
  ASSERT_FALSE(full);
  EXPECT_EQ(full.error(), EpistemicRefusal::IngressBackpressure);
  host->pump();
}

TEST(VagentEpistemicControlCap, EveryHealthTurnCountsAndTheRestWaitsForAWake) {
  LocalHost::Options options;
  options.settings.epistemic.controlPerWake        = 2;
  options.settings.epistemic.healthPendingPerGrant = 8;
  auto program = CompiledProgram::fromSource(kProgram);
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program), options);
  const auto launched = host.launch("A", "a");
  ASSERT_TRUE(launched);
  host.pump();
  auto &ex = host.executor();
  ASSERT_TRUE(ex.bindSource(
      *launched, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)}));
  host.pump();

  EpistemicHealthGrant g{"g1",
                         *HealthIssuer::fromPrincipal(kOperator),
                         {kP1},
                         {},
                         std::chrono::hours(1),
                         8};
  g.actions.push_back(HealthActionKind::Isolate);
  const auto token = ex.grantHealth(*launched, g);
  ASSERT_TRUE(token);
  std::vector<HealthReceipt> receipts;
  for (int i = 0; i < 5; ++i) {
    const auto r = ex.healthAction(
        *launched, {*token, kP1, Isolate{"c" + std::to_string(i)}});
    ASSERT_TRUE(r) << i;
    receipts.push_back(*r);
  }
  // However the wakes fall, every item finishes, in order, none lost to the
  // cap; the cap defers, it does not drop.
  host.pump();
  for (const auto &r : receipts)
    EXPECT_EQ(ex.healthOutcome(*launched, r).state,
              HealthOutcomeView::State::Known);
}

TEST_F(VagentEpistemicGrants, ActionSeqIsMonotonicPerActor) {
  std::vector<HealthAudit> audits;
  ex().setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  host->pump();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(ex().healthAction(pid, request(*token, kP1, Isolate{"x"})));
    host->pump();
  }
  // A refused-at-the-turn action takes the next number too.
  ASSERT_TRUE(ex().healthAction(pid, request(*token, kP1, Isolate{"x"})));
  ASSERT_TRUE(ex().revokeHealthGrant(pid, "g1"));
  host->pump();
  ASSERT_EQ(audits.size(), 4U);
  for (std::size_t i = 0; i < audits.size(); ++i)
    EXPECT_EQ(audits[i].actionSeq, i + 1) << i;
}

TEST_F(VagentEpistemicGrants,
       AnInProcessUpgradeKeepsGrantsBindingsAndSequence) {
  std::vector<HealthAudit> audits;
  ex().setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  host->pump();
  const auto before =
      ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_TRUE(before);
  host->pump();
  ASSERT_EQ(audits.size(), 1U);

  auto next = CompiledProgram::fromSource(std::string(kProgram) + "\n");
  ASSERT_TRUE(next) << next.error();
  auto held = host->retainProgram(std::move(*next));
  ASSERT_TRUE(held);
  const auto upgraded = host->upgrade(pid, held);
  ASSERT_TRUE(upgraded);
  host->pump();

  // The pid survives, so the grant, the outcome log, the audit sequence and
  // the binding (a reading is accepted without a rebind) all stay.
  EXPECT_EQ(ex().healthOutcome(pid, *before).state,
            HealthOutcomeView::State::Known);
  const auto after = ex().healthAction(pid, request(*token, kP1, Isolate{"y"}));
  ASSERT_TRUE(after);
  host->pump();
  ASSERT_EQ(audits.size(), 2U);
  EXPECT_EQ(audits[1].actionSeq, 2U);
  EXPECT_TRUE(ex().observe(pid, reading(1, 1.0)));
  host->pump();
  const auto out = ex().ingestOutcome(pid, kP1, 1, "e1");
  ASSERT_TRUE(out);
  EXPECT_EQ(out->kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicGrants,
       AResumeLosesGrantsBindingsAndOutcomesButNotTheArena) {
  const auto token = ex().grantHealth(pid, grant("g1"));
  ASSERT_TRUE(token);
  commitOne(*host, pid, 5);
  const auto receipt =
      ex().healthAction(pid, request(*token, kP1, Isolate{"x"}));
  ASSERT_TRUE(receipt);
  host->pump();
  ASSERT_EQ(ex().healthOutcome(pid, *receipt).state,
            HealthOutcomeView::State::Known);

  const auto saved = ex().checkpoint(pid);
  ASSERT_TRUE(saved);
  auto other = makeHost();
  ASSERT_TRUE(other);
  const auto resumed = other->executor().resume(*saved);
  ASSERT_TRUE(resumed);
  other->pump();
  other->executor().setWallClockSynchronised(true);
  auto &ex2 = other->executor();

  // Grants are host memory: the old token authorizes nothing on the new pid.
  const auto denied =
      ex2.healthAction(*resumed, request(*token, kP1, Isolate{"x"}));
  ASSERT_FALSE(denied);
  EXPECT_EQ(denied.error(), EpistemicRefusal::UnauthorizedHealthAction);
  // The receipt names the old instance and is unknown here.
  EXPECT_EQ(ex2.healthOutcome(*resumed, *receipt).state,
            HealthOutcomeView::State::Unknown);
  // Bindings are lost: a reading is refused until the source is rebound.
  const auto unbound = ex2.observe(*resumed, reading(6, 1.0));
  EXPECT_FALSE(unbound);

  // Rebound at the same incarnation, a replay is still refused from the
  // profile's last sequence, which the arena kept (matrix 1-10).
  ASSERT_TRUE(ex2.bindSource(
      *resumed, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)}));
  other->pump();
  auto replay    = reading(5, 9.0);
  replay.eventId = "replayed";
  ASSERT_TRUE(ex2.observe(*resumed, replay));
  other->pump();
  const auto stale = ex2.ingestOutcome(*resumed, kP1, 1, "replayed");
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->kind, IngestOutcome::Kind::Refused);
  EXPECT_EQ(stale->code, std::string(kTurnResultStaleSequence));

  // An incarnation below the one the arena holds is refused at the bind.
  ASSERT_TRUE(ex2.bindSource(
      *resumed, {"sensor:gps", kP1, 2, "at", std::chrono::hours(1)}));
  other->pump();
  const auto lower = ex2.bindSource(
      *resumed, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)});
  EXPECT_FALSE(lower);

  // The audit sequence restarts under a new instance label.
  std::vector<HealthAudit> audits;
  ex2.setHealthAuditSink([&](const HealthAudit &a) { audits.push_back(a); });
  const auto fresh = ex2.grantHealth(*resumed, grant("g1"));
  ASSERT_TRUE(fresh);
  ASSERT_TRUE(ex2.healthAction(*resumed, request(*fresh, kP1, Isolate{"x"})));
  other->pump();
  ASSERT_EQ(audits.size(), 1U);
  EXPECT_EQ(audits.front().actionSeq, 1U);
}

} // namespace
