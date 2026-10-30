/**
 * @file vagent_epistemic_ingress_test.cpp
 * @brief P19 I3-1b: checked observations recorded under source supports.
 */
#include <gtest/gtest.h>

#include <chrono>
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
event Quit { }
agent A {
  var adds: Int := 0
  var drops: Int := 0
  on Quit { terminate }
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

  void start(AgentSettings settings, LocalHost::Options options = {},
             std::string_view source = kProgram) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    options.settings = settings;
    host.emplace(std::move(*program), options);
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    base = std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
               .count();
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

  /// Wall seconds when the fixture started: readings are acquired just
  /// before it, inside the policy's 10 s max_age.
  double base{};
  /// e(seq)'s acquisition time.
  double at(std::uint64_t sequence) const {
    return base - 5.0 + 0.01 * static_cast<double>(sequence);
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
    o.acquiredAt          = at(sequence);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = variance;
    return o;
  }

  void observe(const CheckedObservation &o) {
    const auto receipt = host->executor().observe(pid, o);
    ASSERT_TRUE(receipt) << epistemicRefusalName(receipt.error());
    host->pump();
  }
  std::optional<EpistemicRefusal>
  refusedAtIngress(const CheckedObservation &o) {
    const auto receipt = host->executor().observe(pid, o);
    host->pump();
    return receipt ? std::nullopt : std::optional{receipt.error()};
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
  std::int64_t counter(CellRef profile, std::string_view name) {
    const auto cell = value(profile, name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::optional<bool> flag(CellRef record, std::string_view name) {
    const auto cell = value(record, name);
    return cell ? ctx().arena.asBool(*cell) : std::nullopt;
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

TEST_F(VagentEpistemicIngress, MalformedObservationsAreRefusedAtIngress) {
  auto wrongPredicate      = reading(kP1, "x1", 1, 1.0);
  wrongPredicate.predicate = "elsewhere";
  EXPECT_EQ(refusedAtIngress(wrongPredicate), EpistemicRefusal::UnknownSource);
  auto shortTuple  = reading(kP1, "x2", 2, 1.0);
  shortTuple.tuple = {WireValue::text("self")};
  EXPECT_EQ(refusedAtIngress(shortTuple), EpistemicRefusal::InvalidObservation);
  auto wrongType  = reading(kP1, "x3", 3, 1.0);
  wrongType.tuple = {WireValue::text("self"), WireValue::text("one")};
  EXPECT_EQ(refusedAtIngress(wrongType), EpistemicRefusal::InvalidObservation);
  // A non-finite reading, found by the ingress fuzzer: the wire encoder
  // would refuse it, so ingress refuses it first, and logs it.
  for (const double x : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    const auto nonFinite = reading(kP1, "x4", 4, x);
    EXPECT_EQ(refusedAtIngress(nonFinite),
              EpistemicRefusal::InvalidObservation);
    EXPECT_EQ(outcomeOf(nonFinite)->code, "InvalidObservation");
  }
  EXPECT_TRUE(clauses().empty());
  EXPECT_EQ(intField("adds"), 0);
  // Every refusal is in the outcome log, with its class.
  const auto logged = outcomeOf(shortTuple);
  ASSERT_TRUE(logged);
  EXPECT_EQ(logged->kind, IngestOutcome::Kind::Refused);
  EXPECT_EQ(logged->code, "InvalidObservation");
  EXPECT_EQ(logged->outcomeClass, OutcomeClass::Terminal);
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

TEST_F(VagentEpistemicIngress,
       AnObservationCarriesExactlyItsBindingsIncarnation) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto rebooted        = reading(kP1, "r1", 2, 1.0);
  rebooted.incarnation = 2;
  EXPECT_EQ(refusedAtIngress(rebooted), EpistemicRefusal::UnboundIncarnation);
  EXPECT_EQ(outcomeOf(rebooted)->outcomeClass, OutcomeClass::NeedsHostAction);
  auto older        = reading(kP1, "o1", 2, 1.0);
  older.incarnation = 0;
  EXPECT_EQ(refusedAtIngress(older), EpistemicRefusal::StaleIncarnation);
  EXPECT_EQ(outcomeOf(older)->outcomeClass, OutcomeClass::Terminal);
}

TEST_F(VagentEpistemicIngress, AHigherIncarnationRetiresEveryKeyAndRestarts) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto other  = reading(kP1, "e2", 2, 5.0);
  other.tuple = {WireValue::text("other"), WireValue::real(5.0)};
  observe(other);
  auto direct  = reading(kP1, "e3", 3, 1.0);
  direct.frame = "ecef";
  EXPECT_TRUE(refusedAtIngress(direct));
  const auto prof = profiles().front();
  // Queued under the old boot, then superseded before its turn.
  const auto queued = reading(kP1, "e4", 4, 2.0);
  ASSERT_TRUE(host->executor().observe(pid, queued));
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  // The new boot restarts its sequence and may reuse an event ID; queued
  // before the pump, it still runs after the advance.
  auto restarted        = reading(kP1, "e1", 1, 3.0);
  restarted.incarnation = 2;
  ASSERT_TRUE(host->executor().observe(pid, restarted));
  host->pump();
  EXPECT_EQ(outcomeOf(queued)->code, "StaleIncarnation");
  EXPECT_EQ(outcomeOf(restarted)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(counter(prof, kProfileIncarnation), 2);
  // The latch outlives the restart.
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 1);
  std::size_t current = 0, retired = 0;
  for (const auto clause : clauses())
    for (const auto support : supports(clause))
      for (const auto record : history(support)) {
        const auto status = text(record, kObsStatus);
        if (status == kObsStatusCurrent) {
          ++current;
          EXPECT_EQ(counter(record, kObsIncarnation), 2);
        } else {
          ++retired;
        }
      }
  EXPECT_EQ(current, 1U);
  EXPECT_EQ(retired, 0U);
  // The predecessor's supports were dropped (one clause left, for the key
  // the new boot reported; "other" settled -at) and its records moved to the
  // profile's retired history, where calibration still finds them.
  EXPECT_EQ(clauses().size(), 1U);
  EXPECT_EQ(intField("drops"), 2);
  const auto kept = history(prof);
  ASSERT_EQ(kept.size(), 2U);
  for (const auto record : kept) {
    EXPECT_EQ(text(record, kObsStatus), kObsStatusRetired);
    EXPECT_EQ(counter(record, kObsIncarnation), 1);
  }
  auto stale = reading(kP1, "e5", 5, 1.0);
  EXPECT_EQ(refusedAtIngress(stale), EpistemicRefusal::StaleIncarnation);
}

TEST_F(VagentEpistemicIngress, ABindingCannotLowerTheIncarnationFloor) {
  const auto lower =
      host->executor().bindSource(pid, grant(kP1, "sensor:gps", 0));
  ASSERT_FALSE(lower);
  EXPECT_EQ(lower.error(), EpistemicRefusal::StaleIncarnation);
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 3)));
  auto third        = reading(kP1, "e1", 1, 1.0);
  third.incarnation = 3;
  observe(third);
  EXPECT_EQ(host->executor().sourceChangeOutcome(pid, kP1),
            (IngestOutcome{IngestOutcome::Kind::Committed}));
  // Unbinding drops the binding, not the floor the advance reported, so a
  // lower boot is refused at once rather than accepted and revoked later.
  host->executor().unbindSource(pid, kP1, "at");
  const auto again =
      host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2));
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), EpistemicRefusal::StaleIncarnation);
  // The same boot may bind again.
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 3)));
  host->executor().unbindSource(pid, kP1, "at");
  auto replay        = reading(kP1, "e2", 2, 1.0);
  replay.incarnation = 2;
  EXPECT_EQ(refusedAtIngress(replay), EpistemicRefusal::UnknownSource);
}

TEST_F(VagentEpistemicIngress, RetiringASourceDropsItsSupportsAndKeepsAFloor) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto other  = reading(kP1, "e2", 2, 5.0);
  other.tuple = {WireValue::text("other"), WireValue::real(5.0)};
  observe(other);
  observe(reading(kP2, "o1", 1, 1.0));
  const auto before = *host->executor().epistemicCells(pid);
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  host->pump();
  // "self" keeps kP2's support; "other" had only kP1's.
  EXPECT_EQ(clauses().size(), 1U);
  EXPECT_EQ(intField("drops"), 1);
  ASSERT_EQ(clauses().size(), 1U);
  ASSERT_EQ(supports(clauses().front()).size(), 1U);
  EXPECT_EQ(arena().textOf(supports(clauses().front()).front()), kP2);
  EXPECT_LT(*host->executor().epistemicCells(pid), before);
  const auto tomb = profiles().front();
  EXPECT_EQ(arena().textOf(tomb), kP1);
  EXPECT_EQ(flag(tomb, kProfileRetired), true);
  EXPECT_EQ(counter(tomb, kProfileIncarnation), 1);
  EXPECT_FALSE(value(tomb, kProfileDedup));
  EXPECT_FALSE(arena().linked(tomb, ctx().core.dims().clone));
  EXPECT_EQ(refusedAtIngress(reading(kP1, "e3", 3, 1.0)),
            EpistemicRefusal::UnknownSource);
  // Rebinding the retired boot is refused: a RAM-only boot counter that
  // repeats needs a host-issued incarnation, retirement does not reset it.
  const auto retiredBoot =
      host->executor().bindSource(pid, grant(kP1, "sensor:gps", 1));
  ASSERT_FALSE(retiredBoot);
  EXPECT_EQ(retiredBoot.error(), EpistemicRefusal::StaleIncarnation);
  EXPECT_EQ(refusedAtIngress(reading(kP1, "e3", 3, 1.0)),
            EpistemicRefusal::UnknownSource);
  // A strictly higher one revives the profile, but an old event is too old.
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  auto revived        = reading(kP1, "n1", 1, 1.0);
  revived.incarnation = 2;
  observe(revived);
  EXPECT_EQ(outcomeOf(revived)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(flag(tomb, kProfileRetired), false);
  auto old        = reading(kP1, "n2", 2, 1.0);
  old.incarnation = 2;
  old.acquiredAt  = base - 60.0;
  EXPECT_EQ(refusedAtIngress(old), EpistemicRefusal::StaleObservation);
}

TEST_F(VagentEpistemicIngress, ATombstoneTakesNoShareOfThePrincipalBudget) {
  AgentSettings settings;
  settings.epistemic.maxSources = 1;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  host->pump();
  observe(reading(kP2, "o1", 1, 2.0));
  EXPECT_EQ(refused(kTurnResultEpistemicBudget), 0U);
  EXPECT_EQ(profiles().size(), 2U);
  EXPECT_EQ(clauses().size(), 1U);
}

TEST_F(VagentEpistemicIngress, RetiringAnUnseenPrincipalLeavesABareTombstone) {
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 4)));
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  host->pump();
  ASSERT_EQ(profiles().size(), 1U);
  EXPECT_EQ(counter(profiles().front(), kProfileIncarnation), 4);
  EXPECT_EQ(
      host->executor().bindSource(pid, grant(kP1, "sensor:gps", 4)).error(),
      EpistemicRefusal::StaleIncarnation);
  // Retiring a principal nothing names leaves nothing behind.
  ASSERT_TRUE(host->executor().retireSource(pid, kP2));
  host->executor().unbindSource(pid, kP2, "at");
  ASSERT_TRUE(host->executor().retireSource(pid, std::string(64, '5')));
  host->pump();
  EXPECT_EQ(profiles().size(), 2U); // kP1's and kP2's (bound) tombstones
  const auto bad = host->executor().retireSource(pid, "nothex");
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), EpistemicRefusal::InvalidPrincipal);
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
  older.acquiredAt = at(1) - 0.5;
  observe(older);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
  // Within both uncertainties it is the same instant, so it is accepted.
  auto close       = reading(kP1, "e3", 3, 2.0);
  close.acquiredAt = at(1) - 0.005;
  observe(close);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
  // A new clock epoch resets the comparison.
  auto rebased       = reading(kP1, "e4", 4, 3.0);
  rebased.acquiredAt = at(1) - 1.0;
  rebased.clockEpoch = 1;
  observe(rebased);
  EXPECT_EQ(refused(kTurnResultObservationTimeMismatch), 1U);
}

TEST_F(VagentEpistemicIngress, DifferentKeysNeverOrderEachOther) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto other       = reading(kP1, "e2", 2, 5.0);
  other.tuple      = {WireValue::text("other"), WireValue::real(5.0)};
  other.acquiredAt = at(1) - 4.0;
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

TEST_F(VagentEpistemicIngress, BindingRefusesAMalformedPrincipal) {
  auto &executor = host->executor();
  for (const auto &principal :
       {std::string(63, '3'), std::string(64, 'A'), std::string(64, 'g')}) {
    const auto bound = executor.bindSource(pid, grant(principal, "sensor:x"));
    ASSERT_FALSE(bound) << principal;
    EXPECT_EQ(bound.error(), EpistemicRefusal::InvalidPrincipal);
  }
}

TEST_F(VagentEpistemicIngress, AnAliasNamesOnePrincipalAndOneBinding) {
  auto &executor  = host->executor();
  const auto kP3  = std::string(64, '3');
  const auto used = executor.bindSource(pid, grant(kP3, "sensor:gps"));
  ASSERT_FALSE(used);
  EXPECT_EQ(used.error(), EpistemicRefusal::AliasInUse);
  const auto twice = executor.bindSource(pid, grant(kP1, "sensor:second"));
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error(), EpistemicRefusal::DuplicateAlias);
}

TEST_F(VagentEpistemicIngress, AnUnboundOrExpiredSourceIsUnknown) {
  host->executor().unbindSource(pid, kP1, "at");
  EXPECT_EQ(refusedAtIngress(reading(kP1, "e1", 1, 1.0)),
            EpistemicRefusal::UnknownSource);
  const auto kP3   = std::string(64, '3');
  auto expired     = grant(kP3, "sensor:lapsed");
  expired.lifetime = -std::chrono::seconds(1);
  ASSERT_TRUE(host->executor().bindSource(pid, expired));
  EXPECT_EQ(refusedAtIngress(reading(kP3, "e1", 1, 1.0)),
            EpistemicRefusal::UnknownSource);
  EXPECT_TRUE(clauses().empty());
}

TEST_F(VagentEpistemicIngress, AnUnsynchronisedWallClockRefusesRetryably) {
  host->executor().setWallClockSynchronised(false);
  const auto o = reading(kP1, "e1", 1, 1.0);
  EXPECT_EQ(refusedAtIngress(o), EpistemicRefusal::ClockUnsynchronised);
  EXPECT_EQ(outcomeOf(o)->outcomeClass, OutcomeClass::Retryable);
  host->executor().setWallClockSynchronised(true);
  observe(o);
  EXPECT_EQ(clauses().size(), 1U);
}

TEST_F(VagentEpistemicIngress, AcquisitionMustBeRecentAndNotInTheFuture) {
  auto future       = reading(kP1, "e1", 1, 1.0);
  future.acquiredAt = base + 3600.0;
  EXPECT_EQ(refusedAtIngress(future),
            EpistemicRefusal::ObservationTimeMismatch);
  auto stale       = reading(kP1, "e2", 2, 1.0);
  stale.acquiredAt = base - 60.0; // max_age is 10 s
  EXPECT_EQ(refusedAtIngress(stale), EpistemicRefusal::StaleObservation);
  auto wrongBasis       = reading(kP1, "e3", 3, 1.0);
  wrongBasis.clockBasis = "monotonic";
  EXPECT_EQ(refusedAtIngress(wrongBasis),
            EpistemicRefusal::ObservationTimeMismatch);
  EXPECT_TRUE(clauses().empty());
}

TEST_F(VagentEpistemicIngress,
       AContinuousReadingNeedsItsFrameUnitsAndVariance) {
  auto frame  = reading(kP1, "e1", 1, 1.0);
  frame.frame = "ecef";
  EXPECT_EQ(refusedAtIngress(frame),
            EpistemicRefusal::ObservationFrameMismatch);
  auto units  = reading(kP1, "e2", 2, 1.0);
  units.units = "ft";
  EXPECT_EQ(refusedAtIngress(units),
            EpistemicRefusal::ObservationFrameMismatch);
  auto missing     = reading(kP1, "e3", 3, 1.0);
  missing.variance = std::nullopt;
  EXPECT_EQ(refusedAtIngress(missing), EpistemicRefusal::MissingVariance);
  for (const double variance :
       {1e-6, 1000.0, -1.0, std::numeric_limits<double>::infinity()}) {
    EXPECT_EQ(refusedAtIngress(reading(kP1, "e4", 4, 1.0, variance)),
              EpistemicRefusal::InvalidVariance)
        << variance;
  }
  EXPECT_TRUE(clauses().empty());
}

TEST_F(VagentEpistemicIngress, APerSourceQueueBoundPushesBack) {
  AgentSettings settings;
  settings.epistemic.ingressQueuePerSource = 2;
  start(settings);
  auto &executor = host->executor();
  EXPECT_TRUE(executor.observe(pid, reading(kP1, "e1", 1, 1.0)));
  EXPECT_TRUE(executor.observe(pid, reading(kP1, "e2", 2, 1.0)));
  const auto third = executor.observe(pid, reading(kP1, "e3", 3, 1.0));
  ASSERT_FALSE(third);
  EXPECT_EQ(third.error(), EpistemicRefusal::IngressBackpressure);
  // Another source's slots are its own.
  EXPECT_TRUE(executor.observe(pid, reading(kP2, "o1", 1, 2.0)));
  host->pump();
  // Draining frees the slots.
  observe(reading(kP1, "e3", 3, 1.0));
  EXPECT_EQ(outcomeOf(reading(kP1, "e3", 3, 1.0))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, TheOutcomeLogFollowsAnEventToItsTurn) {
  const auto o = reading(kP1, "e1", 1, 1.0);
  ASSERT_TRUE(host->executor().observe(pid, o));
  EXPECT_EQ(outcomeOf(o)->kind, IngestOutcome::Kind::Pending);
  host->pump();
  EXPECT_EQ(outcomeOf(o), (IngestOutcome{IngestOutcome::Kind::Committed}));
  // One entry per event, and a committed one stays committed whatever a
  // resend or a relay meets later.
  observe(o);
  EXPECT_EQ(outcomeOf(o)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(refused(kTurnResultDuplicate), 1U);
  EXPECT_FALSE(host->executor().ingestOutcome(pid, kP1, 1, "never"));
}

TEST_F(VagentEpistemicIngress, ARevocationWhileQueuedRefusesAtItsTurn) {
  const auto o = reading(kP1, "e1", 1, 1.0);
  ASSERT_TRUE(host->executor().observe(pid, o));
  host->executor().unbindSource(pid, kP1, "at");
  host->pump();
  const auto logged = outcomeOf(o);
  ASSERT_TRUE(logged);
  EXPECT_EQ(logged->kind, IngestOutcome::Kind::Refused);
  EXPECT_EQ(logged->code, "UnknownSource");
  EXPECT_TRUE(clauses().empty());
  EXPECT_TRUE(profiles().empty());
}

TEST_F(VagentEpistemicIngress, ADirectContentRefusalChargesHealthAndContact) {
  auto relayed      = reading(kP1, "e1", 1, 1.0);
  relayed.relayedBy = kP2;
  observe(relayed);
  const auto prof = profiles().front();
  ASSERT_FALSE(value(prof, kProfileLastContact));
  auto frame  = reading(kP1, "e2", 2, 1.0);
  frame.frame = "ecef";
  EXPECT_EQ(refusedAtIngress(frame),
            EpistemicRefusal::ObservationFrameMismatch);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 1);
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 0);
  EXPECT_EQ(counter(prof, kProfileRejectCount), 0);
  EXPECT_EQ(text(prof, kProfileLastRefusal), "ObservationFrameMismatch");
  // The device demonstrably sent it.
  EXPECT_TRUE(value(prof, kProfileLastContact));
}

TEST_F(VagentEpistemicIngress, ARelayedRefusalIsDiagnosticOnly) {
  auto relayed      = reading(kP1, "e1", 1, 1.0);
  relayed.relayedBy = kP2;
  observe(relayed);
  const auto prof = profiles().front();
  auto frame      = reading(kP1, "e2", 2, 1.0);
  frame.frame     = "ecef";
  frame.relayedBy = kP2;
  EXPECT_EQ(refusedAtIngress(frame),
            EpistemicRefusal::ObservationFrameMismatch);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 0);
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 1);
  EXPECT_FALSE(value(prof, kProfileLastContact));
}

TEST_F(VagentEpistemicIngress, InTurnRefusalsAreChargedAfterTheUndo) {
  observe(reading(kP1, "e1", 1, 1.0));
  const auto prof = profiles().front();
  observe(reading(kP1, "e0", 1, 1.0)); // StaleSequence
  EXPECT_EQ(refused(kTurnResultStaleSequence), 1U);
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 1);
  auto older       = reading(kP1, "e2", 2, 1.0);
  older.acquiredAt = at(1) - 0.5; // ObservationTimeMismatch
  observe(older);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 1);
  EXPECT_EQ(text(prof, kProfileLastRefusal), "ObservationTimeMismatch");
  // A duplicate is an exact retry, not a refusal.
  observe(reading(kP1, "e1", 1, 1.0));
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 1);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 1);
}

TEST_F(VagentEpistemicIngress, HostConditionsAreNeverCharged) {
  AgentSettings settings;
  settings.epistemic.ingressQueuePerSource = 1;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  const auto prof = profiles().front();
  host->executor().setWallClockSynchronised(false);
  EXPECT_EQ(refusedAtIngress(reading(kP1, "e2", 2, 1.0)),
            EpistemicRefusal::ClockUnsynchronised);
  host->executor().setWallClockSynchronised(true);
  ASSERT_TRUE(host->executor().observe(pid, reading(kP1, "e3", 3, 1.0)));
  EXPECT_FALSE(host->executor().observe(pid, reading(kP1, "e4", 4, 1.0)));
  host->executor().unbindSource(pid, kP1, "at");
  host->pump(); // e3 refuses UnknownSource at its turn
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 0);
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 0);
  EXPECT_EQ(text(prof, kProfileLastRefusal), "<absent>");
}

TEST_F(VagentEpistemicIngress, RefusalsCoalesceIntoOneChargeAndNeedAProfile) {
  // No profile yet: the refusal is logged, and nothing is created for it.
  auto frame  = reading(kP1, "f0", 1, 1.0);
  frame.frame = "ecef";
  EXPECT_EQ(refusedAtIngress(frame),
            EpistemicRefusal::ObservationFrameMismatch);
  EXPECT_TRUE(profiles().empty());
  EXPECT_EQ(refused(kTurnResultNoProfile), 1U);
  observe(reading(kP1, "e1", 1, 1.0));
  const auto prof = profiles().front();
  for (int i = 0; i < 5; ++i) {
    auto bad  = reading(kP1, "f" + std::to_string(i + 1), 2, 1.0);
    bad.units = "ft";
    EXPECT_FALSE(host->executor().observe(pid, bad));
  }
  host->pump();
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 5);
  EXPECT_EQ(refused(kTurnResultNoProfile), 1U);
}

TEST_F(VagentEpistemicIngress, AStoppedActorKeepsNoIngressState) {
  const auto o = reading(kP1, "e1", 1, 1.0);
  observe(o);
  ASSERT_TRUE(outcomeOf(o));
  host->percept(pid, "Quit");
  host->pump();
  EXPECT_FALSE(host->executor().context(pid));
  EXPECT_FALSE(outcomeOf(o));
  EXPECT_EQ(host->executor().observe(pid, o).error(),
            EpistemicRefusal::UnknownSource);
}

// -- Review round 1 findings (i3-1bc-review/) ---------------------------------

TEST_F(VagentEpistemicIngress,
       AKeylessPredicateHoldsOneCurrentReadingPerSource) {
  start({}, {}, R"vagent(
predicate at(x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
agent A { var adds: Int := 0 }
)vagent");
  for (std::uint64_t i = 1; i <= 6; ++i) {
    auto o  = reading(kP1, "e" + std::to_string(i), i, 0.0);
    o.tuple = {WireValue::real(static_cast<double>(i))};
    observe(o);
    EXPECT_EQ(outcomeOf(o)->kind, IngestOutcome::Kind::Committed) << i;
  }
  // The newest reading supersedes the last; no key limit is reached.
  ASSERT_EQ(clauses().size(), 1U);
  EXPECT_EQ(refused(kTurnResultEpistemicKeyLimit), 0U);
}

TEST_F(VagentEpistemicIngress, AResendPastMaxAgeIsARetryNotAFault) {
  LocalHost::Options options;
  options.manualClock = true;
  start({}, options);
  const auto o = reading(kP1, "e1", 1, 1.0);
  observe(o);
  const auto prof = profiles().front();
  host->advance(std::chrono::seconds(30));
  // The store-and-forward flush of an event it already holds.
  EXPECT_EQ(refusedAtIngress(o), EpistemicRefusal::StaleObservation);
  EXPECT_EQ(outcomeOf(o)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 0);
}

TEST_F(VagentEpistemicIngress, ARecordCarriesWhatProducedIt) {
  observe(reading(kP1, "e1", 1, 1.0));
  const auto record = history(supports(clauses().front()).front()).front();
  EXPECT_EQ(counter(record, kObsVersion), kObservationVersion);
  EXPECT_TRUE(text(record, kObsDomain).starts_with("at/2@"));
  EXPECT_EQ(text(record, kObsModel), "beta_reputation");
  EXPECT_EQ(counter(record, kObsModelEpoch), 0);
  EXPECT_EQ(counter(record, kObsCalibrationEpoch), 0);
  EXPECT_EQ(counter(record, kObsFeatureSchema), 0);
  EXPECT_TRUE(value(record, kObsFeatures));
  EXPECT_EQ(text(record, kObsClockInstance).size(), 16U);
  EXPECT_EQ(text(record, kObsHealth), kObsHealthNominal);
  EXPECT_EQ(text(record, kObsStatus), kObsStatusCurrent);
  EXPECT_EQ(text(record, kObsVerdict), kObsVerdictUntested);
}

TEST_F(VagentEpistemicIngress, ClockEpochsAndMappingsNeverRegress) {
  auto later                = reading(kP1, "e1", 1, 1.0);
  later.clockEpoch          = 5;
  later.clockMappingVersion = 2;
  observe(later);
  // On another key, so only the stream (not the per-key order) sees it.
  auto olderEpoch       = reading(kP1, "e2", 2, 2.0);
  olderEpoch.tuple      = {WireValue::text("other"), WireValue::real(2.0)};
  olderEpoch.clockEpoch = 4;
  observe(olderEpoch);
  EXPECT_EQ(outcomeOf(olderEpoch)->code, "ObservationTimeMismatch");
  auto olderMapping       = reading(kP1, "e3", 3, 3.0);
  olderMapping.tuple      = {WireValue::text("other"), WireValue::real(3.0)};
  olderMapping.clockEpoch = 5;
  olderMapping.clockMappingVersion = 1;
  observe(olderMapping);
  EXPECT_EQ(outcomeOf(olderMapping)->code, "ObservationTimeMismatch");
  // A newer epoch resets the per-key order: its times are a new scale.
  auto rebased       = reading(kP1, "e4", 4, 4.0);
  rebased.clockEpoch = 6;
  rebased.acquiredAt = at(1) - 2.0;
  observe(rebased);
  EXPECT_EQ(outcomeOf(rebased)->kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress,
       APredecessorsRetiredKeysDoNotCrowdOutItsSuccessor) {
  AgentSettings settings;
  settings.epistemic.keysPerSource = 2;
  start(settings);
  for (const auto &[key, n] : {std::pair{"k1", 1}, std::pair{"k2", 2}}) {
    auto o  = reading(kP1, key, n, 1.0);
    o.tuple = {WireValue::text(key), WireValue::real(1.0)};
    observe(o);
  }
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  auto fresh        = reading(kP1, "n1", 1, 1.0);
  fresh.incarnation = 2;
  fresh.tuple       = {WireValue::text("k3"), WireValue::real(1.0)};
  observe(fresh);
  EXPECT_EQ(outcomeOf(fresh)->kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, AStarvedTurnIsExhaustedNotADefect) {
  LocalHost::Options options;
  options.maxInstructions = 1000;
  start({}, options);
  const auto o = reading(kP1, "e1", 1, 1.0);
  observe(o);
  EXPECT_EQ(outcomeOf(o)->code, kTurnResultTurnExhausted);
  EXPECT_EQ(outcomeOf(o)->outcomeClass, OutcomeClass::NeedsHostAction);
}

TEST_F(VagentEpistemicIngress,
       EveryBindRunsTheSourceChangeSoAFailedOneRetries) {
  observe(reading(kP1, "e1", 1, 1.0));
  host->executor().unbindSource(pid, kP1, "at");
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps")));
  host->pump();
  // The same boot: the turn ran and changed nothing.
  EXPECT_EQ(host->executor().sourceChangeOutcome(pid, kP1),
            (IngestOutcome{IngestOutcome::Kind::Committed}));
  EXPECT_EQ(counter(profiles().front(), kProfileIncarnation), 1);
}

TEST_F(VagentEpistemicIngress, PastTheArenaAllowanceIngestionRefusesExhausted) {
  AgentSettings settings;
  settings.epistemic.arenaSlots = 1;
  start(settings);
  const auto o = reading(kP1, "e1", 1, 1.0);
  observe(o);
  EXPECT_EQ(outcomeOf(o)->code, kTurnResultArenaExhausted);
  EXPECT_EQ(outcomeOf(o)->outcomeClass, OutcomeClass::NeedsHostAction);
  EXPECT_TRUE(clauses().empty());
}

TEST_F(VagentEpistemicIngress, NothingUnboundOrMalformedIsLogged) {
  const auto kP3 = std::string(64, '3');
  EXPECT_EQ(refusedAtIngress(reading(kP3, "e1", 1, 1.0)),
            EpistemicRefusal::UnknownSource);
  EXPECT_FALSE(outcomeOf(reading(kP3, "e1", 1, 1.0)));
  auto huge = reading(kP1, std::string(kMaxEventIdBytes + 1, 'x'), 1, 1.0);
  EXPECT_EQ(refusedAtIngress(huge), EpistemicRefusal::InvalidObservation);
  EXPECT_FALSE(outcomeOf(huge));
  auto relay      = reading(kP1, "e2", 2, 1.0);
  relay.relayedBy = "not-a-principal";
  EXPECT_EQ(refusedAtIngress(relay), EpistemicRefusal::InvalidObservation);
}

TEST_F(VagentEpistemicIngress, OneSourcesFloodNeverEvictsAnothersOutcomes) {
  AgentSettings settings;
  settings.epistemic.outcomeLog = 4;
  start(settings);
  const auto kept = reading(kP1, "e1", 1, 1.0);
  observe(kept);
  for (int i = 0; i < 20; ++i) {
    auto bad  = reading(kP2, "b" + std::to_string(i), 1, 1.0);
    bad.frame = "ecef";
    EXPECT_TRUE(refusedAtIngress(bad));
  }
  EXPECT_EQ(outcomeOf(kept)->kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, EachSourceAndRelayGetsAFairShareOfTheQueue) {
  AgentSettings settings;
  settings.epistemic.ingressQueue          = 4;
  settings.epistemic.ingressQueuePerSource = 4;
  start(settings);
  auto &executor = host->executor();
  // Two bound sources: two slots each.
  EXPECT_TRUE(executor.observe(pid, reading(kP1, "e1", 1, 1.0)));
  EXPECT_TRUE(executor.observe(pid, reading(kP1, "e2", 2, 1.0)));
  EXPECT_EQ(executor.observe(pid, reading(kP1, "e3", 3, 1.0)).error(),
            EpistemicRefusal::IngressBackpressure);
  // A relay carrying the other source's events is held to a share too.
  const auto kRelay = std::string(64, '9');
  for (const auto *id : {"o1", "o2"}) {
    auto copy      = reading(kP2, id, id[1] - '0', 1.0);
    copy.relayedBy = kRelay;
    EXPECT_TRUE(executor.observe(pid, copy));
  }
  host->pump();
  EXPECT_EQ(outcomeOf(reading(kP1, "e2", 2, 1.0))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, AWakeRunsABoundedNumberOfIngestions) {
  AgentSettings settings;
  settings.epistemic.ingestPerWake = 1;
  start(settings);
  for (std::uint64_t i = 1; i <= 3; ++i)
    ASSERT_TRUE(host->executor().observe(
        pid, reading(kP1, "e" + std::to_string(i), i, 1.0)));
  host->pump(1);
  EXPECT_EQ(outcomeOf(reading(kP1, "e1", 1, 1.0))->kind,
            IngestOutcome::Kind::Committed);
  EXPECT_EQ(outcomeOf(reading(kP1, "e3", 3, 1.0))->kind,
            IngestOutcome::Kind::Pending);
  host->pump();
  EXPECT_EQ(outcomeOf(reading(kP1, "e3", 3, 1.0))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, TheWallClockIsUnsynchronisedUntilReported) {
  auto program = CompiledProgram::fromSource(kProgram);
  ASSERT_TRUE(program);
  LocalHost fresh(std::move(*program));
  const auto agent = fresh.launch("A", "a");
  ASSERT_TRUE(agent);
  fresh.pump();
  ASSERT_TRUE(fresh.executor().bindSource(*agent, grant(kP1, "sensor:gps")));
  EXPECT_EQ(
      fresh.executor().observe(*agent, reading(kP1, "e1", 1, 1.0)).error(),
      EpistemicRefusal::ClockUnsynchronised);
}

TEST_F(VagentEpistemicIngress, AnIncarnationMustFitTheArena) {
  const auto bound = host->executor().bindSource(
      pid, grant(kP1, "sensor:gps", std::numeric_limits<std::uint64_t>::max()));
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error(), EpistemicRefusal::InvalidIncarnation);
}

TEST_F(VagentEpistemicIngress, ARevivedSourceTakesAShareOfTheBudgetAgain) {
  AgentSettings settings;
  settings.epistemic.maxSources = 1;
  start(settings);
  observe(reading(kP1, "e1", 1, 1.0));
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  host->pump();
  observe(reading(kP2, "o1", 1, 1.0));
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  host->pump();
  EXPECT_EQ(host->executor().sourceChangeOutcome(pid, kP1)->code,
            kTurnResultEpistemicBudget);
  // The tombstone takes no reading, so the binding is revoked.
  auto revived        = reading(kP1, "n1", 1, 1.0);
  revived.incarnation = 2;
  EXPECT_EQ(refusedAtIngress(revived), EpistemicRefusal::UnknownSource);
}

TEST_F(VagentEpistemicIngress, TheVarianceBoundsAreInclusive) {
  // The policy admits [0.0001, 100.0].
  observe(reading(kP1, "lo", 1, 1.0, 0.0001));
  EXPECT_EQ(outcomeOf(reading(kP1, "lo", 1, 1.0))->kind,
            IngestOutcome::Kind::Committed);
  observe(reading(kP1, "hi", 2, 2.0, 100.0));
  EXPECT_EQ(outcomeOf(reading(kP1, "hi", 2, 2.0))->kind,
            IngestOutcome::Kind::Committed);
  EXPECT_EQ(refusedAtIngress(reading(kP1, "over", 3, 3.0, 100.0001)),
            EpistemicRefusal::InvalidVariance);
  EXPECT_EQ(refusedAtIngress(reading(kP1, "under", 4, 3.0, 0.00009)),
            EpistemicRefusal::InvalidVariance);
}

TEST_F(VagentEpistemicIngress, TheTimeWindowsWidenByUncertaintyAndSkew) {
  LocalHost::Options options;
  options.manualClock = true;
  start({}, options);
  // max_age 10 s and the default clock_skew; uncertainty widens both sides.
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  auto old                = reading(kP1, "old", 1, 1.0);
  old.acquiredUncertainty = 2.0;
  old.acquiredAt          = now - 11.0; // inside max_age + uncertainty
  observe(old);
  EXPECT_EQ(outcomeOf(old)->kind, IngestOutcome::Kind::Committed);
  auto older                = reading(kP1, "older", 2, 1.0);
  older.acquiredUncertainty = 0.5;
  older.acquiredAt          = now - 11.0; // outside
  EXPECT_EQ(refusedAtIngress(older), EpistemicRefusal::StaleObservation);
  auto ahead                = reading(kP1, "ahead", 3, 1.0);
  ahead.acquiredUncertainty = 2.0;
  ahead.acquiredAt          = now + 1.5; // inside uncertainty
  observe(ahead);
  EXPECT_EQ(outcomeOf(ahead)->kind, IngestOutcome::Kind::Committed);
  auto further                = reading(kP1, "further", 4, 1.0);
  further.acquiredUncertainty = 0.0;
  further.acquiredAt          = now + 5.0;
  EXPECT_EQ(refusedAtIngress(further),
            EpistemicRefusal::ObservationTimeMismatch);
}

TEST_F(VagentEpistemicIngress,
       AClaimedUncertaintyIsBoundedBeforeItWidensAnything) {
  start({}, {}, R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    max_uncertainty: 1s, cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
agent A { var adds: Int := 0 }
)vagent");
  // Dated 100000 s ago, or a year ahead: a huge claimed uncertainty would
  // otherwise stretch both windows to admit it.
  for (const double offset : {-100000.0, 365.0 * 86400.0}) {
    auto far       = reading(kP1, "f" + std::to_string(offset), 1, 1.0);
    far.acquiredAt = base + offset;
    far.acquiredUncertainty = 1e9;
    EXPECT_EQ(refusedAtIngress(far), EpistemicRefusal::UncertainAcquisition)
        << offset;
  }
  auto edge                = reading(kP1, "edge", 1, 1.0);
  edge.acquiredUncertainty = 1.0;
  observe(edge);
  EXPECT_EQ(outcomeOf(edge)->kind, IngestOutcome::Kind::Committed);
  auto over                = reading(kP1, "over", 2, 1.0);
  over.acquiredUncertainty = 1.001;
  EXPECT_EQ(refusedAtIngress(over), EpistemicRefusal::UncertainAcquisition);
  // A content refusal: the device sent a timestamp it cannot vouch for.
  EXPECT_EQ(counter(profiles().front(), kProfileContentRefusals), 1);
}

// -- Review round 2 findings
// ---------------------------------------------------

TEST_F(VagentEpistemicIngress, SourceChangesInOnePumpRunInCallOrder) {
  observe(reading(kP1, "e1", 1, 1.0));
  // Retire, then bind the next boot, before the agent runs either.
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 2)));
  host->pump();
  EXPECT_EQ(host->executor().sourceChangeOutcome(pid, kP1),
            (IngestOutcome{IngestOutcome::Kind::Committed}));
  auto next        = reading(kP1, "n1", 1, 1.0);
  next.incarnation = 2;
  observe(next);
  EXPECT_EQ(outcomeOf(next)->kind, IngestOutcome::Kind::Committed);
  EXPECT_EQ(flag(profiles().front(), kProfileRetired), false);
  // Two advances in one pump: the later boot wins, and its floor is not
  // made strict by the earlier one.
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 5)));
  ASSERT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 6)));
  host->pump();
  EXPECT_EQ(counter(profiles().front(), kProfileIncarnation), 6);
  EXPECT_TRUE(host->executor().bindSource(pid, grant(kP1, "sensor:gps", 6)));
}

TEST_F(VagentEpistemicIngress, ReplacementBootsWithNewKeysStayBounded) {
  // A device that reports different keys after every reboot.
  std::size_t afterTen = 0;
  for (std::uint64_t boot = 1; boot <= 30; ++boot) {
    if (boot > 1)
      ASSERT_TRUE(
          host->executor().bindSource(pid, grant(kP1, "sensor:gps", boot)));
    for (std::uint64_t k = 0; k < 4; ++k) {
      auto o =
          reading(kP1, "b" + std::to_string(boot) + "k" + std::to_string(k),
                  k + 1, 1.0);
      o.incarnation = boot;
      o.tuple       = {WireValue::text("key" + std::to_string(boot * 4 + k)),
                       WireValue::real(1.0)};
      observe(o);
      ASSERT_EQ(outcomeOf(o)->kind, IngestOutcome::Kind::Committed)
          << boot << "/" << k;
    }
    if (boot == 10) afterTen = *host->executor().epistemicCells(pid);
  }
  // Only the current boot's keys are believed, and the retired history is
  // bounded, so the epistemic cells stop growing.
  EXPECT_EQ(clauses().size(), 4U);
  EXPECT_LE(history(profiles().front()).size(),
            AgentSettings{}.epistemic.history *
                AgentSettings{}.epistemic.keysPerSource);
  EXPECT_LE(*host->executor().epistemicCells(pid), afterTen + 64);
  observe(reading(kP2, "o1", 1, 1.0));
  EXPECT_EQ(outcomeOf(reading(kP2, "o1", 1, 1.0))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicIngress, RetiringAnUnseenPrincipalSetsNoFloor) {
  const auto kP3 = std::string(64, '3');
  ASSERT_TRUE(host->executor().retireSource(pid, kP3));
  host->pump();
  EXPECT_TRUE(host->executor().bindSource(pid, grant(kP3, "sensor:new", 0)));
}

TEST_F(VagentEpistemicIngress, ARepeatedRefusalIsChargedOnce) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto bad  = reading(kP1, "e2", 2, 1.0);
  bad.frame = "ecef";
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(refusedAtIngress(bad));
  EXPECT_EQ(counter(profiles().front(), kProfileContentRefusals), 1);
}

TEST_F(VagentEpistemicIngress, AnInTurnRefusalResentIsChargedOnce) {
  observe(reading(kP1, "e1", 1, 1.0));
  auto older       = reading(kP1, "e2", 2, 2.0);
  older.acquiredAt = at(1) - 0.5; // refused in the turn: acquired earlier
  observe(older);
  observe(older);
  EXPECT_EQ(outcomeOf(older)->code, "ObservationTimeMismatch");
  EXPECT_EQ(counter(profiles().front(), kProfileContentRefusals), 1);
}

TEST_F(VagentEpistemicIngress, ARelayedRefusalDoesNotHideTheDirectCopy) {
  observe(reading(kP1, "e1", 1, 1.0));
  const auto prof   = profiles().front();
  auto relayed      = reading(kP1, "e2", 2, 1.0);
  relayed.frame     = "ecef";
  relayed.relayedBy = kP2;
  EXPECT_TRUE(refusedAtIngress(relayed));
  auto direct      = relayed;
  direct.relayedBy = std::nullopt;
  EXPECT_TRUE(refusedAtIngress(direct));
  EXPECT_TRUE(refusedAtIngress(direct));
  EXPECT_EQ(counter(prof, kProfileRefusedCount), 1);
  EXPECT_EQ(counter(prof, kProfileContentRefusals), 1);
}

TEST_F(VagentEpistemicIngress, BindingAnUnknownActorIsRefused) {
  ActorPid nobody = pid;
  nobody.spawnSequence += 1000;
  const auto bound = host->executor().bindSource(nobody, grant(kP1, "x"));
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error(), EpistemicRefusal::UnknownActor);
}

TEST(VagentEpistemicIngressPlain, AProgramWithoutArbitrationCannotBind) {
  auto program = CompiledProgram::fromSource("agent A { var n: Int := 0 }");
  ASSERT_TRUE(program) << program.error();
  LocalHost host(std::move(*program));
  const auto pid = host.launch("A", "a");
  ASSERT_TRUE(pid);
  host.pump();
  const auto bound = host.executor().bindSource(
      *pid, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)});
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error(), EpistemicRefusal::NotArbitrated);
}

} // namespace
