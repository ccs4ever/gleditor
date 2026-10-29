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
  }

  std::vector<CellRef> profiles() {
    const auto dims = registerDimensions(arena());
    return zigzag::rankAfter(arena(), *host->executor().agentCell(pid),
                             dims.trust) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::size_t listLength(CellRef list) {
    std::size_t n = 0;
    for (auto e = arena().linked(list, ctx().core.dims().grab); e;
         e      = arena().linked(*e, ctx().core.dims().step))
      ++n;
    return n;
  }
  std::optional<CellRef> value(CellRef record, std::string_view name) {
    return fieldValue(ctx().arena, valueDims(ctx().core), record, name);
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

TEST_F(VagentEpistemicIngress, EachPrincipalGetsOneProfileOnTheTrustRank) {
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP1, "e2", 2, 1.5));
  observe(reading(kP2, "q1", 1, 1.0));
  const auto ps = profiles();
  ASSERT_EQ(ps.size(), 2U);
  EXPECT_EQ(arena().textOf(ps[0]), kP1);
  EXPECT_EQ(arena().textOf(ps[1]), kP2);
  EXPECT_EQ(arena().asInt64(*value(ps[0], kProfileIncarnation)), 1);
  EXPECT_EQ(arena().asBool(*value(ps[0], kProfileFaulted)), false);
  EXPECT_EQ(arena().asInt64(*value(ps[0], kProfileRefusedCount)), 0);
  EXPECT_EQ(listLength(*value(ps[0], kProfileDedup)), 2U);
  // The record's profile field is a clone occurrence of the profile master.
  const auto rec  = history(supports(clauses().front()).front()).front();
  const auto view = value(rec, kObsProfile);
  ASSERT_TRUE(view);
  EXPECT_EQ(arena().cloneMaster(*view, ctx().core.dims().clone), ps[0]);
}

TEST_F(VagentEpistemicIngress, AnExactRetryCommitsNothingAndAReuseConflicts) {
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP1, "e1", 1, 1.0));
  EXPECT_EQ(refused(kTurnResultDuplicate), 1U);
  auto reused = reading(kP1, "e1", 1, 3.0);
  observe(reused);
  EXPECT_EQ(refused(kTurnResultConflictingEvent), 1U);
  const auto cls = clauses();
  ASSERT_EQ(cls.size(), 1U);
  EXPECT_EQ(history(supports(cls[0]).front()).size(), 1U);
  EXPECT_EQ(intField("adds"), 1);
  // A relay of the same event carries the same digest: still a duplicate.
  auto relayed      = reading(kP1, "e1", 1, 1.0);
  relayed.relayedBy = kP2;
  observe(relayed);
  EXPECT_EQ(refused(kTurnResultDuplicate), 2U);
}

TEST_F(VagentEpistemicIngress, ASequenceThatDoesNotAdvanceIsStale) {
  observe(reading(kP1, "e5", 5, 1.0));
  observe(reading(kP1, "e4", 4, 2.0));
  observe(reading(kP1, "e5b", 5, 2.0));
  EXPECT_EQ(refused(kTurnResultStaleSequence), 2U);
  EXPECT_EQ(history(supports(clauses().front()).front()).size(), 1U);
}

TEST_F(VagentEpistemicIngress, APrincipalBeyondTheBudgetIsRefused) {
  AgentSettings settings;
  settings.epistemic.maxSources = 1;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  observe(reading(kP2, "q1", 1, 1.0));
  EXPECT_EQ(refused(kTurnResultEpistemicBudget), 1U);
  EXPECT_EQ(profiles().size(), 1U);
  EXPECT_EQ(supports(clauses().front()).size(), 1U);
}

TEST_F(VagentEpistemicIngress, AnIncarnationChangeWaitsForIncarnationSupport) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto rebooted        = reading(kP1, "r1", 1, 1.0);
  rebooted.incarnation = 2;
  observe(rebooted);
  EXPECT_EQ(refused(kTurnResultUnsupportedIncarnation), 1U);
}

TEST_F(VagentEpistemicIngress, TheDedupWindowKeepsOnlyTheNewestEntries) {
  AgentSettings settings;
  settings.epistemic.dedup = 2;
  start(settings);
  for (std::uint64_t i = 1; i <= 4; ++i)
    observe(reading(kP1, "e" + std::to_string(i), i, 1.0));
  const auto dedup = *value(profiles().front(), kProfileDedup);
  ASSERT_EQ(listLength(dedup), 2U);
  const auto first = arena().linked(dedup, ctx().core.dims().grab);
  EXPECT_EQ(text(*first, kObsEventId), "e3");
  // e1 fell out of the window, so its replay falls to the sequence rule.
  observe(reading(kP1, "e1", 1, 1.0));
  EXPECT_EQ(refused(kTurnResultStaleSequence), 1U);
}

TEST_F(VagentEpistemicIngress, OnlyADirectEventRefreshesContact) {
  auto relayed      = reading(kP1, "e1", 1, 1.0);
  relayed.relayedBy = kP2;
  observe(relayed);
  const auto prof = profiles().front();
  EXPECT_TRUE(value(prof, kProfileLastReceive));
  EXPECT_FALSE(value(prof, kProfileLastContact));
  observe(reading(kP1, "e2", 2, 1.0));
  EXPECT_TRUE(value(prof, kProfileLastContact));
}

TEST_F(VagentEpistemicIngress, AnOlderAcquisitionCannotReplaceANewerReading) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto older       = reading(kP1, "e2", 2, 2.0);
  older.acquiredAt = 100.5; // e1 was acquired at 101.0
  observe(older);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
  // Within both uncertainties it is the same instant, so it is accepted.
  auto close       = reading(kP1, "e3", 3, 2.0);
  close.acquiredAt = 100.995;
  observe(close);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
  // A new clock epoch resets the comparison.
  auto rebased       = reading(kP1, "e4", 4, 3.0);
  rebased.acquiredAt = 1.0;
  rebased.clockEpoch = 1;
  observe(rebased);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
}

TEST_F(VagentEpistemicIngress, DifferentKeysNeverOrderEachOther) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto other       = reading(kP1, "e2", 2, 5.0);
  other.tuple      = {WireValue::text("other"), WireValue::real(5.0)};
  other.acquiredAt = 50.0;
  observe(other);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 0U);
  EXPECT_EQ(clauses().size(), 2U);
}

TEST_F(VagentEpistemicIngress, APrincipalSupportsAtMostItsKeyLimit) {
  AgentSettings settings;
  settings.epistemic.keysPerSource = 1;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  auto other  = reading(kP1, "e2", 2, 5.0);
  other.tuple = {WireValue::text("other"), WireValue::real(5.0)};
  observe(other);
  EXPECT_EQ(refused(kTurnResultEpistemicKeyLimit), 1U);
  EXPECT_EQ(clauses().size(), 1U);
  // Another value on the same key is still one key.
  observe(reading(kP1, "e3", 3, 2.0));
  EXPECT_EQ(refused(kTurnResultEpistemicKeyLimit), 1U);
}

TEST_F(VagentEpistemicIngress, TheHistoryBoundExcisesRetiredRecordsAndViews) {
  AgentSettings settings;
  settings.epistemic.history = 2;
  start(settings);
  for (std::uint64_t i = 1; i <= 5; ++i)
    observe(reading(kP1, "e" + std::to_string(i), i, 1.0));
  const auto h = history(supports(clauses().front()).front());
  ASSERT_EQ(h.size(), 2U);
  EXPECT_EQ(text(h[0], kObsEventId), "e4");
  EXPECT_EQ(text(h[0], kObsStatus), kObsStatusRetired);
  EXPECT_EQ(text(h[1], kObsEventId), "e5");
  EXPECT_EQ(text(h[1], kObsStatus), kObsStatusCurrent);
  // One clone occurrence per surviving record.
  std::size_t views = 0;
  for (auto v = arena().linked(profiles().front(), ctx().core.dims().clone); v;
       v      = arena().linked(*v, ctx().core.dims().clone))
    ++views;
  EXPECT_EQ(views, 2U);
}

TEST_F(VagentEpistemicIngress, TheEpistemicCellCountPlateausAtTheHistoryBound) {
  AgentSettings settings;
  settings.epistemic.history = 2;
  settings.epistemic.dedup   = 2;
  start(settings);
  std::vector<std::size_t> counts;
  for (std::uint64_t i = 1; i <= 6; ++i) {
    observe(reading(kP1, "e" + std::to_string(i), i, 1.0));
    counts.push_back(*host->executor().epistemicCells(pid));
  }
  EXPECT_LT(counts[0], counts[1]);
  // Excision and window trimming return what each new reading adds, and
  // every reading committed: a plateau from refused turns would prove nothing.
  EXPECT_EQ(counts[3], counts[4]);
  EXPECT_EQ(counts[4], counts[5]);
  const auto h = history(supports(clauses().front()).front());
  ASSERT_EQ(h.size(), 2U);
  EXPECT_EQ(text(h[1], kObsEventId), "e6");
  EXPECT_EQ(host->executor().stat(pid, kStatDropped), 0U);
}

TEST_F(VagentEpistemicIngress, TheCellCeilingRefusesWithoutWritingAnything) {
  AgentSettings settings;
  settings.epistemic.cells = 20;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  EXPECT_EQ(refused(kTurnResultEpistemicBudget), 1U);
  EXPECT_TRUE(profiles().empty());
  EXPECT_TRUE(clauses().empty());
  EXPECT_EQ(*host->executor().epistemicCells(pid), 0U);
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
