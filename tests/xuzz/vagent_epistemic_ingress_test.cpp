/**
 * @file vagent_epistemic_ingress_test.cpp
 * @brief P19 I3-1b: checked observations recorded under source supports.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

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
agent A {
  var adds: Int := 0
  var drops: Int := 0
  on +at(?w, ?x) { adds := adds + 1 }
  on -at(?w, ?x) { drops := drops + 1 }
}
)vagent";

const std::string kP1(64, '1');
const std::string kP2(64, '2');

class VagentEpistemicIngress : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;

  void SetUp() override {
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    host.emplace(std::move(*program));
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
  }

  CheckedObservation reading(const std::string &principal, std::string eventId,
                             std::uint64_t sequence, double x,
                             double variance = 0.25) {
    CheckedObservation o;
    o.principal           = principal;
    o.incarnation         = 1;
    o.eventId             = std::move(eventId);
    o.sequence            = sequence;
    o.predicate           = "at";
    o.tuple               = {WireValue::text("self"), WireValue::real(x)};
    o.acquiredAt          = 100.0 + static_cast<double>(sequence);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = variance;
    return o;
  }

  void observe(const CheckedObservation &o) {
    ASSERT_TRUE(host->executor().observe(pid, o));
    host->pump();
  }

  const zigzag::vortex::WorkerContext &ctx() {
    return *host->executor().context(pid);
  }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(ctx().arena);
  }
  std::int64_t intField(std::string_view name) {
    const auto cell = fieldValue(ctx().arena, valueDims(ctx().core),
                                 *host->executor().agentCell(pid), name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::vector<CellRef> clauses() {
    const auto dims = registerDimensions(arena());
    for (const auto pred : zigzag::rankAfter(
             arena(), *host->executor().agentCell(pid), dims.belief))
      if (arena().textOf(pred) == "at")
        return zigzag::rankAfter(arena(), pred, ctx().core.dims().clause) |
               std::ranges::to<std::vector<CellRef>>();
    return {};
  }
  std::vector<CellRef> supports(CellRef clause) {
    const auto dims = registerDimensions(arena());
    return zigzag::rankAfter(arena(), clause, dims.source) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::vector<CellRef> history(CellRef support) {
    const auto dims = registerDimensions(arena());
    return zigzag::rankAfter(arena(), support, dims.epistemic) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::string text(CellRef record, std::string_view name) {
    const auto cell =
        fieldValue(ctx().arena, valueDims(ctx().core), record, name);
    return cell ? ctx().arena.textOf(*cell) : std::string("<absent>");
  }
  std::optional<double> real(CellRef record, std::string_view name) {
    const auto cell =
        fieldValue(ctx().arena, valueDims(ctx().core), record, name);
    return cell ? ctx().arena.asDouble(*cell) : std::nullopt;
  }
  std::uint64_t refused(std::string_view code) {
    return host->executor().stat(pid, std::string(kStatRefusedPrefix) +
                                          std::string(code));
  }
};

TEST_F(VagentEpistemicIngress, TwoSourcesShareAClauseWithIndependentRecords) {
  observe(reading(kP1, "p1-1", 1, 1.0, 0.5));
  observe(reading(kP2, "p2-1", 1, 1.0, 2.0));
  const auto cls = clauses();
  ASSERT_EQ(cls.size(), 1U);
  const auto sup = supports(cls[0]);
  ASSERT_EQ(sup.size(), 2U);
  // Supports are named by the authenticated principal, not an alias.
  EXPECT_EQ(arena().textOf(sup[0]), kP1);
  EXPECT_EQ(arena().textOf(sup[1]), kP2);
  const auto h1 = history(sup[0]), h2 = history(sup[1]);
  ASSERT_EQ(h1.size(), 1U);
  ASSERT_EQ(h2.size(), 1U);
  EXPECT_EQ(text(h1[0], "event_id"), "p1-1");
  EXPECT_EQ(text(h2[0], "event_id"), "p2-1");
  EXPECT_EQ(real(h1[0], "variance"), 0.5);
  EXPECT_EQ(real(h2[0], "variance"), 2.0);
  for (const auto r : {h1[0], h2[0]}) {
    EXPECT_EQ(text(r, kObsStatus), kObsStatusCurrent);
    EXPECT_EQ(text(r, kObsVerdict), kObsVerdictUntested);
  }
  // One proposition, so `+at` fired once.
  EXPECT_EQ(intField("adds"), 1);
}

TEST_F(VagentEpistemicIngress, ANewEventWithTheSameTupleRetiresTheLastOne) {
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP1, "e2", 2, 1.0));
  const auto cls = clauses();
  ASSERT_EQ(cls.size(), 1U);
  const auto sup = supports(cls[0]);
  ASSERT_EQ(sup.size(), 1U);
  const auto h = history(sup[0]);
  ASSERT_EQ(h.size(), 2U);
  EXPECT_EQ(text(h[0], "event_id"), "e1");
  EXPECT_EQ(text(h[0], kObsStatus), kObsStatusRetired);
  EXPECT_EQ(text(h[1], "event_id"), "e2");
  EXPECT_EQ(text(h[1], kObsStatus), kObsStatusCurrent);
  EXPECT_EQ(intField("adds"), 1);
}

TEST_F(VagentEpistemicIngress, AChangedValueCarriesItsSourcesHistoryAlong) {
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP2, "q1", 1, 1.0));
  observe(reading(kP1, "e2", 2, 2.0));
  const auto cls = clauses();
  ASSERT_EQ(cls.size(), 2U);
  // P2 still supports the old value; P1 moved to the new one, history and all.
  const auto oldSup = supports(cls[0]), newSup = supports(cls[1]);
  ASSERT_EQ(oldSup.size(), 1U);
  EXPECT_EQ(arena().textOf(oldSup[0]), kP2);
  ASSERT_EQ(history(oldSup[0]).size(), 1U);
  ASSERT_EQ(newSup.size(), 1U);
  EXPECT_EQ(arena().textOf(newSup[0]), kP1);
  const auto h = history(newSup[0]);
  ASSERT_EQ(h.size(), 2U);
  EXPECT_EQ(text(h[0], "event_id"), "e1");
  EXPECT_EQ(text(h[0], kObsStatus), kObsStatusRetired);
  EXPECT_EQ(text(h[1], "event_id"), "e2");
  EXPECT_EQ(text(h[1], kObsStatus), kObsStatusCurrent);
  // Exactly one current record for P1 on this key.
  EXPECT_EQ(intField("adds"), 2);
  EXPECT_EQ(intField("drops"), 0);
}

TEST_F(VagentEpistemicIngress, TheLastSourceLeavingAValueDropsItsClause) {
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP1, "e2", 2, 2.0));
  const auto cls = clauses();
  ASSERT_EQ(cls.size(), 1U);
  const auto sup = supports(cls[0]);
  ASSERT_EQ(sup.size(), 1U);
  EXPECT_EQ(history(sup[0]).size(), 2U);
  EXPECT_EQ(intField("drops"), 1);
}

TEST_F(VagentEpistemicIngress, MalformedObservationsAreUndoneWithATypedCode) {
  auto wrongPredicate      = reading(kP1, "x1", 1, 1.0);
  wrongPredicate.predicate = "elsewhere";
  observe(wrongPredicate);
  EXPECT_EQ(refused(kTurnResultIngestionFault), 1U);
  auto shortTuple  = reading(kP1, "x2", 2, 1.0);
  shortTuple.tuple = {WireValue::text("self")};
  observe(shortTuple);
  EXPECT_EQ(refused(kTurnResultInvalidObservation), 1U);
  EXPECT_TRUE(clauses().empty());
  EXPECT_EQ(intField("adds"), 0);
  // The result field is empty again after each refusal.
  const auto cell =
      fieldValue(ctx().arena, valueDims(ctx().core),
                 *host->executor().agentCell(pid), kFieldTurnResult);
  ASSERT_TRUE(cell);
  EXPECT_EQ(ctx().arena.textOf(*cell), "");
}

TEST_F(VagentEpistemicIngress,
       StructurallyInvalidInputIsRefusedBeforeQueueing) {
  auto bad           = reading(kP1, "n1", 1, 1.0);
  bad.acquiredAt     = std::numeric_limits<double>::quiet_NaN();
  const auto receipt = host->executor().observe(pid, bad);
  ASSERT_FALSE(receipt);
  EXPECT_EQ(receipt.error(), EpistemicRefusal::InvalidObservation);
  auto huge =
      reading(kP1, "n2", std::numeric_limits<std::uint64_t>::max(), 1.0);
  EXPECT_FALSE(host->executor().observe(pid, huge));
  host->pump();
  EXPECT_TRUE(clauses().empty());
}

TEST(VagentEpistemicIngressPlain, AProgramWithoutArbitrationHasNoRoutine) {
  auto program = CompiledProgram::fromSource("agent A { var n: Int := 0 }");
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  CheckedObservation o;
  o.principal = kP1;
  o.eventId   = "e";
  o.predicate = "at";
  ASSERT_TRUE(host.executor().observe(*pid, o));
  host.pump();
  EXPECT_EQ(
      host.executor().stat(*pid, std::string(kStatRefusedPrefix) +
                                     std::string(kTurnResultIngestionFault)),
      1U);
}

} // namespace
