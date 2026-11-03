/**
 * @file vagent_epistemic_settle_test.cpp
 * @brief P19 I3-2b: belief events a source change queues settle after the
 *        change commits, one top-level event per turn, and a failing `-pred`
 *        handler cannot undo the change that raised it.
 */
#include <gleditor/logging.hpp>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/vagent/runtime/settle_discard.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::CellRef;

constexpr std::string_view kPolicy = R"vagent(
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
)vagent";

// Between one settle turn's cost (about 3.3k cycles) and the whole batch's
// (about 42.7k), so the batch only settles if each turn has its own grant.
constexpr std::uint64_t kBatchGrant = 20000;
const std::string kP1(64, '1');
const std::string kP2(64, '2');

std::string predicateDecl(std::string_view name) {
  return "predicate " + std::string(name) + "(key who: Str, x: Float)" +
         std::string(kPolicy);
}

class VagentEpistemicSettle : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  double base{};
  std::uint64_t sequence{};
  std::vector<std::string> predicates;
  std::string lastSource;

  /// @p body is the agent's members. @p extra is declarations placed before
  /// the agent (non-arbitrated predicates).
  void start(std::string_view body,
             std::vector<std::string> arbitrated = {"at"},
             AgentSettings settings = {}, LocalHost::Options options = {},
             std::string_view extra = {}) {
    predicates = arbitrated;
    sequence   = 0;
    std::string source;
    for (const auto &p : predicates) source += predicateDecl(p);
    source += extra;
    source += "\nagent A {\n" + std::string(body) + "\n}\n";
    lastSource   = source;
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
    bindAll(kP1, 1);
    bindAll(kP2, 1);
  }

  void bindAll(const std::string &principal, std::uint64_t incarnation) {
    for (const auto &p : predicates)
      ASSERT_TRUE(host->executor().bindSource(
          pid, {(principal == kP1 ? "s1:" : "s2:") + p, principal, incarnation,
                p, std::chrono::hours(1)}));
    host->pump();
  }

  CheckedObservation reading(const std::string &principal, std::string key,
                             double x, std::string predicate = "at",
                             std::uint64_t incarnation = 1) {
    ++sequence;
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = incarnation;
    o.eventId     = predicate + key + std::to_string(sequence);
    o.sequence    = sequence;
    o.predicate   = std::move(predicate);
    o.tuple       = {WireValue::text(std::move(key)), WireValue::real(x)};
    o.acquiredAt  = base - 5.0 + 0.01 * static_cast<double>(sequence);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
  }

  IngestOutcome observe(const CheckedObservation &o) {
    const auto receipt = host->executor().observe(pid, o);
    EXPECT_TRUE(receipt) << epistemicRefusalName(receipt.error());
    host->pump();
    return host->executor()
        .ingestOutcome(pid, o.principal, o.incarnation, o.eventId)
        .value_or(IngestOutcome{});
  }

  void retire(const std::string &principal) {
    ASSERT_TRUE(host->executor().retireSource(pid, principal));
    host->pump();
  }
  void advance(const std::string &principal, std::uint64_t incarnation) {
    bindAll(principal, incarnation);
  }

  IngestOutcome changeOutcome(const std::string &principal) {
    return host->executor()
        .sourceChangeOutcome(pid, principal)
        .value_or(IngestOutcome{});
  }
  static std::vector<std::string> names(const SettleReport &report) {
    return {report.names.begin(), report.names.end()};
  }

  const zigzag::vortex::WorkerContext &ctx() {
    return *host->executor().context(pid);
  }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(ctx().arena);
  }
  AgentDims dims() { return registerDimensions(arena()); }
  std::int64_t intField(std::string_view name) {
    const auto cell = fieldValue(ctx().arena, valueDims(ctx().core),
                                 *host->executor().agentCell(pid), name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  bool queueEmpty() {
    return !arena().linked(*host->executor().agentCell(pid), dims().pending);
  }
  std::size_t clauseCount(std::string_view predicate = "at") {
    for (const auto pred : zigzag::rankAfter(
             arena(), *host->executor().agentCell(pid), dims().belief))
      if (arena().textOf(pred) == predicate)
        return std::ranges::distance(
            zigzag::rankAfter(arena(), pred, ctx().core.dims().clause));
    return 0;
  }
  std::optional<CellRef> profile(const std::string &principal) {
    for (const auto p : zigzag::rankAfter(
             arena(), *host->executor().agentCell(pid), dims().trust))
      if (arena().textOf(p) == principal) return p;
    return std::nullopt;
  }
  std::int64_t counter(CellRef record, std::string_view name) {
    const auto cell =
        fieldValue(ctx().arena, valueDims(ctx().core), record, name);
    return cell ? ctx().arena.asInt64(*cell).value_or(-1) : -1;
  }
  std::uint64_t refused(std::string_view code) {
    return host->executor().stat(pid, std::string(kStatRefusedPrefix) +
                                          std::string(code));
  }
};

// A `fail` statement is contained by the handler's own choice point, at HEAD
// as before, so it never fails the turn. What fails a turn is a hard failure:
// here a cascade past `vagent.belief_event_depth` rounds, which `-at` starts
// on every run.
constexpr std::string_view kCounting = R"vagent(
  var adds: Int := 0
  on +at(?w, ?x) { adds := adds + 1 }
  on -at(?w, ?x) { believe boom(1) }
  on +boom(?n) { believe boom(n + 1) }
)vagent";
constexpr std::string_view kBoom     = "\npredicate boom(n: Int)\n";

TEST_F(VagentEpistemicSettle, AFailingHandlerDoesNotUndoARetire) {
  start(kCounting, {"at"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0));
  observe(reading(kP1, "k2", 2.0));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->settled, 0U);
  EXPECT_EQ(out.settle->discarded, 2U);
  // Two events of one type name it once.
  EXPECT_EQ(names(*out.settle), (std::vector<std::string>{"-at"}));
  EXPECT_EQ(refused(kSettleFailedLabel), 2U);
  EXPECT_EQ(clauseCount(), 0U);
  EXPECT_TRUE(queueEmpty());
  // The next ingestion commits.
  EXPECT_EQ(observe(reading(kP2, "k1", 1.0)).kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicSettle, AFailingHandlerDoesNotUndoAnAdvance) {
  start(kCounting, {"at"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0));
  advance(kP1, 2);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
  EXPECT_EQ(names(*out.settle), (std::vector<std::string>{"-at"}));
  ASSERT_TRUE(profile(kP1));
  EXPECT_EQ(counter(*profile(kP1), kProfileIncarnation), 2);
  // A discarded event still raised the replay floor.
  const auto lower = host->executor().bindSource(
      pid, {"s1:at", kP1, 1, "at", std::chrono::hours(1)});
  ASSERT_FALSE(lower);
  EXPECT_EQ(lower.error(), EpistemicRefusal::StaleIncarnation);
  auto fresh = reading(kP1, "k9", 3.0, "at", 2);
  EXPECT_EQ(observe(fresh).kind, IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicSettle, ACommittedChangeWithNothingQueuedHasNoReport) {
  start(kCounting, {"at"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0));
  advance(kP1, 1);
  EXPECT_FALSE(changeOutcome(kP1).settle);
}

TEST_F(VagentEpistemicSettle, TheOtherPredicatesEventSettlesExactlyOnce) {
  start(R"vagent(
  var hdrops: Int := 0
  on -at(?w, ?x) { believe boom(1) }
  on +boom(?n) { believe boom(n + 1) }
  on -heading(?w, ?x) { hdrops := hdrops + 1 }
)vagent",
        {"at", "heading"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0, "at"));
  observe(reading(kP1, "k1", 1.0, "heading"));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->settled, 1U);
  EXPECT_EQ(out.settle->discarded, 1U);
  EXPECT_EQ(intField("hdrops"), 1);
  EXPECT_TRUE(queueEmpty());
}

TEST_F(VagentEpistemicSettle, OnlyTheFirstFourDiscardedTypesAreNamed) {
  start(R"vagent(
  on -a1(?w, ?x) { believe boom(1) }
  on -a2(?w, ?x) { believe boom(1) }
  on -a3(?w, ?x) { believe boom(1) }
  on -a4(?w, ?x) { believe boom(1) }
  on -a5(?w, ?x) { believe boom(1) }
  on +boom(?n) { believe boom(n + 1) }
)vagent",
        {"a1", "a2", "a3", "a4", "a5"}, {}, {}, kBoom);
  for (const char *p : {"a1", "a2", "a3", "a4", "a5"})
    observe(reading(kP1, "k", 1.0, p));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 5U);
  EXPECT_EQ(names(*out.settle),
            (std::vector<std::string>{"-a5", "-a4", "-a3", "-a2"}));
}

TEST_F(VagentEpistemicSettle, ABatchTooBigForOneTurnSettlesEventByEvent) {
  AgentSettings settings;
  settings.epistemic.keysPerSource = 12;
  LocalHost::Options options;
  options.maxInstructions = kBatchGrant;
  start(R"vagent(
  var spin: Int := 0
  var drops: Int := 0
  on -at(?w, ?x) { for j in 1 to 120 do spin := spin + 1
                   drops := drops + 1 }
)vagent",
        {"at"}, settings, options);
  for (int i = 0; i < 12; ++i)
    observe(reading(kP1, "k" + std::to_string(i), 1.0 + i));
  const auto cycles = ctx().vm.totalCycles();
  retire(kP1);
  // The control: one turn could not have held the batch. If a handler's cost
  // changes and this fails, the grant above no longer proves anything.
  EXPECT_GT(ctx().vm.totalCycles() - cycles, kBatchGrant);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->settled, 12U);
  EXPECT_EQ(out.settle->discarded, 0U);
  EXPECT_EQ(intField("drops"), 12);
}

TEST_F(VagentEpistemicSettle, ACascadeRunsInsideItsParentsTurnInOrder) {
  start(R"vagent(
  var order: Int := 0
  on -at(?w, ?x) [x < 1.5] { order := order * 10 + 1
                             believe note(1) }
  on -at(?w, ?x) [x > 1.5] { order := order * 10 + 2
                             believe note(2) }
  on +note(?n) { order := order * 10 + 7 }
)vagent",
        {"at"}, {}, {}, "\npredicate note(n: Int)\n");
  observe(reading(kP1, "k1", 1.0));
  observe(reading(kP1, "k2", 2.0));
  retire(kP1);
  // Top-level order kept (the clone rank drops the newest key first); each
  // cascade event precedes the next top-level one, where breadth-first
  // rounds would have given 2177.
  EXPECT_EQ(intField("order"), 2717);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->settled, 2U);
}

TEST_F(VagentEpistemicSettle, ARunawayCascadeFailsItsParentsTurnOnly) {
  start(R"vagent(
  var drops: Int := 0
  on -at(?w, ?x) [x < 1.5] { believe note(1) }
  on -at(?w, ?x) [x > 1.5] { drops := drops + 1 }
  on +note(?n) { believe note(n + 1) }
)vagent",
        {"at"}, {}, {}, "\npredicate note(n: Int)\n");
  observe(reading(kP1, "k1", 1.0));
  observe(reading(kP1, "k2", 2.0));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
  EXPECT_EQ(out.settle->settled, 1U);
  EXPECT_EQ(intField("drops"), 1);
  EXPECT_EQ(clauseCount("note"), 0U);
  EXPECT_TRUE(queueEmpty());
}

TEST_F(VagentEpistemicSettle, TheSettleLoopRunsBeforeAQueuedObservation) {
  start(R"vagent(
  var trace: Int := 0
  on -at(?w, ?x) { trace := trace * 10 + 1 }
  on +at(?w, ?x) { trace := trace * 10 + 2 }
)vagent");
  observe(reading(kP1, "k1", 1.0));
  ASSERT_EQ(intField("trace"), 2);
  // Both before the agent runs either.
  ASSERT_TRUE(host->executor().retireSource(pid, kP1));
  ASSERT_TRUE(host->executor().observe(pid, reading(kP2, "k1", 1.0)));
  host->pump();
  EXPECT_EQ(intField("trace"), 212);
}

TEST_F(VagentEpistemicSettle, AStarvedSettleTurnIsDiscardedWithoutTheVm) {
  start(R"vagent(
  var spin: Int := 0
  on -at(?w, ?x) { for j in 1 to 100000000 do spin := spin + 1 }
)vagent");
  observe(reading(kP1, "k1", 1.0));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
  EXPECT_TRUE(queueEmpty());
  EXPECT_EQ(clauseCount(), 0U);
  // Consistent: the actor still ingests.
  EXPECT_EQ(observe(reading(kP2, "k1", 1.0)).kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicSettle, TheDiscardSucceedsWithTheSlotGateClosed) {
  // x > 100 hard-fails (so the probe's settle turns are undone and its cell
  // count is the change turn's); x < 100 is a benign count.
  constexpr auto kBody = R"vagent(
  var drops: Int := 0
  on -at(?w, ?x) [x > 100.0] { believe boom(1) }
  on -at(?w, ?x) [x < 100.0] { drops := drops + 1 }
  on +boom(?n) { believe boom(n + 1) }
)vagent";
  const auto run       = [&](std::size_t slots,
                       double x) -> std::pair<std::size_t, std::size_t> {
    AgentSettings settings;
    settings.epistemic.keysPerSource = 16;
    if (slots != 0) settings.epistemic.arenaSlots = slots;
    start(kBody, {"at"}, settings, {}, kBoom);
    for (int i = 0; i < 16; ++i)
      observe(reading(kP1, "k" + std::to_string(i), x + i));
    const auto before = ctx().arena.cellCount();
    retire(kP1);
    return {before, ctx().arena.cellCount()};
  };
  const auto [before, after] = run(0, 101.0);
  // The retire turn is admitted one slot under the allowance and ends past
  // the allowance plus its reserve.
  const std::size_t slots = before + 1;
  ASSERT_GE(after, slots + slots / 64) << before << " " << after;
  // Control: with room, the same shape of change settles.
  run(0, 1.0);
  ASSERT_TRUE(changeOutcome(kP1).settle);
  EXPECT_EQ(changeOutcome(kP1).settle->settled, 16U);
  EXPECT_EQ(intField("drops"), 16);
  run(slots, 1.0);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 16U);
  EXPECT_EQ(out.settle->settled, 0U);
  EXPECT_EQ(intField("drops"), 0);
  EXPECT_TRUE(queueEmpty());
}

TEST_F(VagentEpistemicSettle, TheReserveLetsSettleTurnsRunPastTheAllowance) {
  constexpr auto kBody = R"vagent(
  var drops: Int := 0
  on -at(?w, ?x) [x > 100.0] { believe boom(1) }
  on -at(?w, ?x) [x < 100.0] { drops := drops + 1 }
  on +boom(?n) { believe boom(n + 1) }
)vagent";
  const auto run       = [&](std::size_t slots, double x) -> std::size_t {
    AgentSettings settings;
    if (slots != 0) settings.epistemic.arenaSlots = slots;
    start(kBody, {"at"}, settings, {}, kBoom);
    for (int i = 0; i < 2; ++i)
      observe(reading(kP1, "k" + std::to_string(i), x + i));
    retire(kP1);
    return ctx().arena.cellCount();
  };
  // The probe's settle turns fail and are undone: the count is the change
  // turn's. Allow one slot fewer, so the change is admitted and ends at the
  // allowance, where only the reserve is left to the settle turns.
  const auto after = run(0, 101.0);
  run(after - 1, 1.0);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->settled, 2U);
  EXPECT_EQ(out.settle->discarded, 0U);
  EXPECT_EQ(intField("drops"), 2);
  EXPECT_GE(ctx().arena.cellCount(), after - 1);
  // Ordinary entries stay refused by the allowance itself.
  EXPECT_EQ(observe(reading(kP2, "k0", 1.0)).code,
            std::optional<std::string>(std::string(kTurnResultArenaExhausted)));
}

TEST_F(VagentEpistemicSettle, ATinyAllowanceKeepsWorking) {
  AgentSettings settings;
  settings.epistemic.arenaSlots = 1;
  ASSERT_TRUE(settings.epistemic.validate());
  EXPECT_EQ(settings.epistemic.slotReserve(), 0U);
  start(kCounting, {"at"}, settings, {}, kBoom);
  // Every epistemic turn is refused by the allowance, and nothing breaks.
  EXPECT_EQ(observe(reading(kP1, "k1", 1.0)).code,
            std::optional<std::string>(std::string(kTurnResultArenaExhausted)));
  EXPECT_TRUE(queueEmpty());
}

TEST_F(VagentEpistemicSettle, ASettleNeverChargesTheSource) {
  start(kCounting, {"at"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0));
  const auto turnsBefore = intField(kFieldTurnCount);
  retire(kP1);
  ASSERT_TRUE(profile(kP1));
  const auto prof = *profile(kP1);
  for (const auto name : {kProfileRejectionStreak, kProfileRejectCount,
                          kProfileContentRefusals, kProfileRefusedCount})
    EXPECT_EQ(counter(prof, name), 0) << name;
  // The change turn and one settle turn; no bookkeeping turn followed.
  EXPECT_EQ(intField(kFieldTurnCount), turnsBefore + 2);
}

TEST_F(VagentEpistemicSettle,
       AnIngestionWedgeKeepsRefusingAndRetireStillCommits) {
  start(kCounting, {"at"}, {}, {}, kBoom);
  EXPECT_EQ(observe(reading(kP1, "self", 1.0)).kind,
            IngestOutcome::Kind::Committed);
  // A reading that replaces the key raises `-at` for a handler that always
  // fails: ingestion stays all-or-nothing and refuses.
  const auto first = observe(reading(kP1, "self", 2.0));
  EXPECT_EQ(first.kind, IngestOutcome::Kind::Refused);
  EXPECT_EQ(first.code, std::optional<std::string>("IngestionFault"));
  const auto again = observe(reading(kP1, "self", 3.0));
  EXPECT_EQ(again.code, std::optional<std::string>("IngestionFault"));
  // The wedge is the ingestion's. The repair path no longer shares it.
  retire(kP1);
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
  // (An operator reset admitting one non-replacing reading needs 2c2.)
}

TEST_F(VagentEpistemicSettle, ACheckpointRefusesLinkedPendingEvents) {
  start(R"vagent(
  var drops: Int := 0
  on -at(?w, ?x) { drops := drops + 1 }
)vagent");
  observe(reading(kP1, "k1", 1.0));
  // Nothing leaves events queued between wakes but a crash in the loop;
  // stand one in. No handler matches its type, so settling it is a no-op.
  const auto ghost = arena().makeCell(std::string_view("+nothing"));
  ASSERT_TRUE(arena().link(*host->executor().agentCell(pid), dims().pending,
                           zigzag::DimVector::POS, ghost));
  const auto refused = host->executor().checkpoint(pid);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), CheckpointRefusal::SettlePending);
  EXPECT_EQ(toString(refused.error()), "settle pending");
  // A source change settles whatever is queued.
  advance(kP1, 2);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  // The stood-in event and the one the advance queued.
  EXPECT_EQ(out.settle->settled, 2U);
  EXPECT_TRUE(queueEmpty());
  const auto saved = host->executor().checkpoint(pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  // Resume keeps no pending events.
  auto program = CompiledProgram::fromSource(lastSource);
  ASSERT_TRUE(program) << program.error();
  LocalHost second(std::move(*program));
  const auto resumed = second.executor().resume(*saved);
  ASSERT_TRUE(resumed);
  second.pump();
  const auto &resumedCtx = *second.executor().context(*resumed);
  auto &resumedArena = const_cast<zigzag::ArenaManifold &>(resumedCtx.arena);
  EXPECT_FALSE(resumedArena.linked(*second.executor().agentCell(*resumed),
                                   registerDimensions(resumedArena).pending));
}

TEST_F(VagentEpistemicSettle, AnUpgradedProgramKeepsItsSettleEntry) {
  const std::string head = predicateDecl("at");
  const auto source      = [&](std::string_view handler, int version) {
    return head + std::string(kBoom) +
           "\nagent A {\n  var drops: Int := 0\n  on -at(?w, ?x) " +
           std::string(handler) +
           "\n  on +boom(?n) { believe boom(n + 1) }\n}\n" +
           std::string(version, '\n');
  };
  auto first = CompiledProgram::fromSource(source("{ drops := drops + 1 }", 0));
  ASSERT_TRUE(first) << first.error();
  ASSERT_TRUE(first->agents().at("A").settleEntry);
  host.emplace(std::move(*first));
  predicates          = {"at"};
  const auto launched = host->launch("A", "a");
  ASSERT_TRUE(launched);
  pid = *launched;
  host->pump();
  host->executor().setWallClockSynchronised(true);
  base = std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
             .count();
  host->executor().drainNotifications();
  // The new version's handler always fails, so only a settle entry that
  // survived the upgrade can report the discard.
  auto second = CompiledProgram::fromSource(source("{ believe boom(1) }", 1));
  ASSERT_TRUE(second) << second.error();
  ASSERT_TRUE(second->agents().at("A").settleEntry);
  const auto upgraded = host->upgrade(pid, std::move(*second));
  ASSERT_TRUE(upgraded);
  host->pump();
  host->executor().drainNotifications();
  bindAll(kP1, 1);
  observe(reading(kP1, "k1", 1.0));
  retire(kP1);
  const auto out = changeOutcome(kP1);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
}

TEST_F(VagentEpistemicSettle, ARetireOfSixteenEventsCostsOneTurnEach) {
  AgentSettings settings;
  settings.epistemic.keysPerSource = 16;
  start(R"vagent(
  var drops: Int := 0
  on -at(?w, ?x) { drops := drops + 1 }
)vagent",
        {"at"}, settings);
  for (int i = 0; i < 16; ++i)
    observe(reading(kP1, "k" + std::to_string(i), 1.0 + i));
  const auto turns  = intField(kFieldTurnCount);
  const auto cycles = ctx().vm.totalCycles();
  retire(kP1);
  const auto spent = ctx().vm.totalCycles() - cycles;
  EXPECT_EQ(intField(kFieldTurnCount), turns + 1 + 16);
  EXPECT_EQ(intField("drops"), 16);
  const auto before = ctx().vm.totalCycles();
  observe(reading(kP2, "k0", 1.0));
  const auto ingestion = ctx().vm.totalCycles() - before;
  const auto single    = ctx().vm.totalCycles();
  retire(kP2);
  const auto one = ctx().vm.totalCycles() - single;
  // The design's price: sixteen events cost about 3.3 ingestions of work.
  RecordProperty("retire16_cycles", static_cast<int>(spent));
  RecordProperty("retire1_cycles", static_cast<int>(one));
  RecordProperty("ingestion_cycles", static_cast<int>(ingestion));
  EXPECT_LT(spent, 4 * ingestion);
  EXPECT_LT(one, ingestion);
}

TEST_F(VagentEpistemicSettle, TheReserveIsClampedBelowTheRefSpace) {
  constexpr std::size_t kRefSpace = zigzag::ephemeralBit;
  EpistemicLimits limits;
  EXPECT_EQ(limits.slotReserve(), limits.arenaSlots / kSlotReserveDivisor);
  EXPECT_FALSE(limits.slotReserveClamped());
  limits.arenaSlots = kRefSpace - 1000;
  EXPECT_EQ(limits.slotReserve(), 1000U);
  EXPECT_TRUE(limits.slotReserveClamped());
  EXPECT_LE(limits.arenaSlots + limits.slotReserve(), kRefSpace);
  for (const auto slots : {kRefSpace, kRefSpace + 1, kRefSpace * 2}) {
    limits.arenaSlots = slots;
    EXPECT_EQ(limits.slotReserve(), 0U) << slots;
  }
}

TEST_F(VagentEpistemicSettle, ConstructionWarnsOnceWhenTheReserveIsClamped) {
  std::ostringstream out;
  auto sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  auto logger = gleditor::logging::category("vortex.agent");
  logger->sinks().push_back(sink);
  const auto previous = logger->level();
  logger->set_level(spdlog::level::warn);
  const auto construct = [&](std::size_t slots) {
    AgentSettings settings;
    settings.epistemic.arenaSlots = slots;
    auto program                  = CompiledProgram::fromSource(
        predicateDecl("at") + "\nagent A { var n: Int := 0 }\n");
    EXPECT_TRUE(program);
    LocalHost made(std::move(*program), {.settings = settings});
  };
  construct(AgentSettings{}.epistemic.arenaSlots);
  const auto quiet = out.str();
  construct(static_cast<std::size_t>(zigzag::ephemeralBit) - 1000);
  logger->set_level(previous);
  std::erase(logger->sinks(), sink);
  EXPECT_EQ(quiet.find("settle reserve"), std::string::npos) << quiet;
  const auto text = out.str();
  const auto at   = text.find("settle reserve of 1000");
  ASSERT_NE(at, std::string::npos) << text;
  EXPECT_EQ(text.find("settle reserve", at + 1), std::string::npos) << text;
}

TEST_F(VagentEpistemicSettle, TheWholeQueueFallbackCountsEveryEvent) {
  zigzag::ArenaManifold standalone;
  const auto pendingDim = standalone.makeCell("d.pending");
  const auto agentCell  = standalone.makeCell("agent");
  std::vector<CellRef> events;
  for (const char *type : {"-a", "-b", "-a"}) {
    events.push_back(standalone.makeCell(std::string_view(type)));
    ASSERT_TRUE(standalone.link(
        events.size() == 1 ? agentCell : events[events.size() - 2], pendingDim,
        zigzag::DimVector::POS, events.back()));
  }
  SettleReport report;
  // A successor the arena does not hold is refused, as a stale ref would be.
  const auto bogus = zigzag::ephemeralBit | 0x7fff'ffffU;
  ASSERT_FALSE(standalone.contains(bogus));
  ASSERT_EQ(discardQueueHead(standalone, agentCell, pendingDim, events[0],
                             bogus, report),
            DiscardResult::WholeQueue);
  EXPECT_EQ(report.discarded, 3U);
  EXPECT_EQ(names(report), (std::vector<std::string>{"-a", "-b"}));
  EXPECT_FALSE(standalone.linked(agentCell, pendingDim));
}

TEST_F(VagentEpistemicSettle, AnUnlinkTheArenaRefusesLeavesTheQueueAlone) {
  zigzag::ArenaManifold standalone;
  const auto pendingDim = standalone.makeCell("d.pending");
  const auto agentCell  = standalone.makeCell("agent");
  const auto event      = standalone.makeCell(std::string_view("-a"));
  ASSERT_TRUE(
      standalone.link(agentCell, pendingDim, zigzag::DimVector::POS, event));
  SettleReport report;
  // No such dimension: both the unlink and the whole-queue drop are refused.
  EXPECT_EQ(discardQueueHead(standalone, agentCell,
                             zigzag::ephemeralBit | 0x7fff'ffffU, event,
                             std::nullopt, report),
            DiscardResult::Stuck);
  EXPECT_EQ(report.discarded, 0U);
  EXPECT_TRUE(standalone.linked(agentCell, pendingDim));
}

TEST_F(VagentEpistemicSettle, AReboundSecondPredicateKeepsTheAdvancesReport) {
  start(R"vagent(
  on -at(?w, ?x) { believe boom(1) }
  on +boom(?n) { believe boom(n + 1) }
  on -heading(?w, ?x) { believe boom(1) }
)vagent",
        {"at", "heading"}, {}, {}, kBoom);
  observe(reading(kP1, "k1", 1.0, "at"));
  advance(kP1, 2);
  // Two binds in one wake: the advance, then a no-op for the second
  // predicate. The no-op queued nothing, but the report is the principal's.
  const auto out = changeOutcome(kP1);
  EXPECT_EQ(out.kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(out.settle);
  EXPECT_EQ(out.settle->discarded, 1U);
  EXPECT_EQ(names(*out.settle), (std::vector<std::string>{"-at"}));
  // A later advance starts a report of its own.
  observe(reading(kP1, "k2", 1.0, "heading", 2));
  advance(kP1, 3);
  const auto next = changeOutcome(kP1);
  ASSERT_TRUE(next.settle);
  EXPECT_EQ(next.settle->discarded, 1U);
  EXPECT_EQ(names(*next.settle), (std::vector<std::string>{"-heading"}));
}

TEST_F(VagentEpistemicSettle, AReportAccumulatesCountsAcrossNoOpRebinds) {
  SettleReport total;
  SettleReport first;
  first.settled = 1;
  first.discard("-a").discard("-b");
  SettleReport second;
  second.settled = 2;
  second.discard("-b").discard("-c").discard("-d").discard("-e");
  total.merge(first).merge(second);
  EXPECT_EQ(total.settled, 3U);
  EXPECT_EQ(total.discarded, 6U);
  EXPECT_EQ(names(total), (std::vector<std::string>{"-a", "-b", "-c", "-d"}));
}

TEST_F(VagentEpistemicSettle, ACheckpointFromInsideAWakeIsRefused) {
  start(R"vagent(
  on -at(?w, ?x) { emit Seen{} in outer }
)vagent",
        {"at"}, {}, {}, "\nevent Seen { }\n");
  observe(reading(kP1, "k1", 1.0));
  std::optional<CheckpointRefusal> inside;
  host->executor().setHostSink([&](const Envelope &) {
    const auto saved = host->executor().checkpoint(pid);
    if (!saved) inside = saved.error();
  });
  retire(kP1);
  // The sink ran during the settle loop's turn.
  ASSERT_TRUE(inside);
  EXPECT_EQ(*inside, CheckpointRefusal::TurnInProgress);
  EXPECT_EQ(toString(*inside), "turn in progress");
  // Between wakes it is allowed again.
  EXPECT_TRUE(host->executor().checkpoint(pid));
}

} // namespace
