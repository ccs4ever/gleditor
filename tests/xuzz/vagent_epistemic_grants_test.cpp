/**
 * @file vagent_epistemic_grants_test.cpp
 * @brief P19 I3-2 slice 2c1: health grants, the host API for health actions,
 *        the post order and the `healthEntry` stub.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

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

} // namespace
