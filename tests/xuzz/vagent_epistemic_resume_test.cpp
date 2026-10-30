/**
 * @file vagent_epistemic_resume_test.cpp
 * @brief P19 I3-1d: source profiles and observation histories survive a
 *        checkpoint and resume, and the arena's incarnation floor holds on a
 *        host that has never seen the source.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "epistemic_invariants.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;
using xanadu::identity::ActorPid;
using zigzag::CellRef;

constexpr std::string_view kSource       = R"vagent(
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
constexpr std::string_view kPredicates[] = {"at"};

const std::string kP1(64, '1');
const std::string kP2(64, '2');

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

/// One host running one agent, with the readings and reads the tests use.
struct Side {
  std::optional<LocalHost> host;
  ActorPid pid;

  AgentExecutor &executor() { return host->executor(); }
  const zigzag::vortex::WorkerContext &ctx() {
    return *executor().context(pid);
  }
  const zigzag::ArenaManifold &arena() { return ctx().arena; }
  AgentDims dims() {
    return registerDimensions(const_cast<zigzag::ArenaManifold &>(arena()));
  }
  void ready() { executor().setWallClockSynchronised(true); }
  void bind(const std::string &principal, std::uint64_t incarnation = 1) {
    ASSERT_TRUE(
        executor().bindSource(pid, {"sensor:" + principal, principal,
                                    incarnation, "at", std::chrono::hours(1)}));
    host->pump();
  }
  std::optional<IngestOutcome> observe(const CheckedObservation &o) {
    const auto receipt = executor().observe(pid, o);
    host->pump();
    if (!receipt) return std::nullopt;
    return executor().ingestOutcome(pid, o.principal, o.incarnation, o.eventId);
  }
  std::vector<CellRef> profiles() {
    return zigzag::rankAfter(arena(), *executor().agentCell(pid),
                             dims().trust) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::optional<CellRef> profile(const std::string &principal) {
    for (const auto p : profiles())
      if (arena().textOf(p) == principal) return p;
    return std::nullopt;
  }
  std::optional<CellRef> value(CellRef record, std::string_view name) {
    return fieldValue(arena(), valueDims(ctx().core), record, name);
  }
  std::int64_t counter(CellRef record, std::string_view name) {
    const auto cell = value(record, name);
    return cell ? arena().asInt64(*cell).value_or(-1) : -1;
  }
  std::uint64_t stat(std::string_view code) {
    return executor().stat(pid,
                           std::string(kStatRefusedPrefix) + std::string(code));
  }
  /// Per (principal, key): the statuses of its history, oldest first.
  std::map<std::pair<std::string, std::string>, std::vector<std::string>>
  histories() {
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> out;
    const auto d = dims();
    for (const auto pred :
         zigzag::rankAfter(arena(), *executor().agentCell(pid), d.belief)) {
      if (arena().textOf(pred) != "at") continue;
      for (const auto clause :
           zigzag::rankAfter(arena(), pred, ctx().core.dims().clause)) {
        const auto term = arena().linked(clause, ctx().core.dims().grab);
        const auto key  = arena().linked(*term, ctx().core.dims().grab);
        for (const auto support : zigzag::rankAfter(arena(), clause, d.source))
          for (const auto record :
               zigzag::rankAfter(arena(), support, d.epistemic)) {
            const auto status = value(record, kObsStatus);
            out[{arena().textOf(support), arena().textOf(*key)}].push_back(
                status ? arena().textOf(*status) : "<none>");
          }
      }
    }
    return out;
  }
  /// The clone occurrences on a profile's rank: one per retained record.
  std::size_t views(CellRef profile) {
    std::size_t n = 0;
    for (auto v = arena().linked(profile, ctx().core.dims().clone); v;
         v      = arena().linked(*v, ctx().core.dims().clone))
      ++n;
    return n;
  }
};

class VagentEpistemicResume : public ::testing::Test {
protected:
  Side a, b;
  double base{};

  void SetUp() override {
    auto program = CompiledProgram::fromSource(kSource);
    ASSERT_TRUE(program) << program.error();
    a.host.emplace(std::move(*program));
    const auto launched = a.host->launch("A", "a");
    ASSERT_TRUE(launched);
    a.pid = *launched;
    a.host->pump();
    a.ready();
    base = wallSeconds();
  }

  CheckedObservation reading(const std::string &principal, std::string key,
                             std::uint64_t sequence, double x,
                             std::uint64_t incarnation = 1) {
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = incarnation;
    o.eventId     = key + std::to_string(sequence);
    o.sequence    = sequence;
    o.predicate   = "at";
    o.tuple       = {WireValue::text(std::move(key)), WireValue::real(x)};
    o.acquiredAt  = base - 5.0 + 0.01 * static_cast<double>(sequence);
    o.acquiredUncertainty = 0.01;
    o.clockBasis          = "wall";
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
  }

  /// Two sources, two keys each, with retired records in every history.
  void populate() {
    a.bind(kP1);
    a.bind(kP2);
    std::uint64_t seq = 0;
    for (const auto &principal : {kP1, kP2})
      for (const auto *key : {"left", "right"})
        for (int i = 0; i < 3; ++i)
          ASSERT_EQ(a.observe(reading(principal, key, ++seq, 1.0 + i))->kind,
                    IngestOutcome::Kind::Committed);
  }

  /// Checkpoint `a` and resume it on `b`, compiled from @p source.
  bool resumeOnB(std::string_view source = kSource) {
    const auto saved = a.executor().checkpoint(a.pid);
    EXPECT_TRUE(saved) << (saved ? "" : toString(saved.error()));
    if (!saved) return false;
    auto program = CompiledProgram::fromSource(source);
    EXPECT_TRUE(program) << program.error();
    b.host.emplace(std::move(*program));
    const auto resumed = b.executor().resume(*saved);
    EXPECT_TRUE(resumed);
    if (!resumed) return false;
    b.pid = *resumed;
    b.host->pump();
    if (!b.executor().context(b.pid)) return false;
    b.ready();
    return true;
  }
};

TEST_F(VagentEpistemicResume, ProfilesHistoriesAndCloneRanksSurvive) {
  populate();
  const auto before      = a.histories();
  const auto cellsBefore = *a.executor().epistemicCells(a.pid);
  const auto p1          = *a.profile(kP1);
  const auto viewsBefore = a.views(p1);
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(b.histories(), before);
  // Less the two liveness stamps each profile drops on resume.
  EXPECT_EQ(*b.executor().epistemicCells(b.pid) + 2 * b.profiles().size(),
            cellsBefore);
  ASSERT_EQ(b.profiles().size(), 2U);
  const auto q1 = *b.profile(kP1);
  EXPECT_EQ(b.views(q1), viewsBefore);
  EXPECT_EQ(b.counter(q1, kProfileIncarnation), 1);
  const EpistemicView view(b.executor(), b.pid, kPredicates);
  const auto broken = view.violation();
  EXPECT_FALSE(broken) << *broken;
}

TEST_F(VagentEpistemicResume, DedupWindowsAndStreamsCarryOver) {
  populate();
  ASSERT_TRUE(resumeOnB());
  b.bind(kP1);
  // The last committed event is still a duplicate, and its sequence is
  // still the stream's floor.
  b.observe(reading(kP1, "right", 6, 3.0));
  EXPECT_EQ(b.stat(kTurnResultDuplicate), 1U);
  auto behind      = reading(kP1, "right", 5, 9.0);
  behind.eventId   = "behind";
  const auto stale = b.observe(behind);
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->code, kTurnResultStaleSequence);
  EXPECT_EQ(b.observe(reading(kP1, "right", 13, 4.0))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicResume, AdvanceAndRetireWalkTheRestoredRanks) {
  populate();
  ASSERT_TRUE(resumeOnB());
  b.bind(kP1, 2);
  for (const auto &[key, statuses] : b.histories())
    if (key.first == kP1)
      for (const auto &status : statuses)
        EXPECT_EQ(status, kObsStatusRetired) << key.second;
  ASSERT_TRUE(b.executor().retireSource(b.pid, kP2));
  b.host->pump();
  for (const auto &[key, statuses] : b.histories())
    EXPECT_NE(key.first, kP2) << key.second;
  EXPECT_EQ(b.views(*b.profile(kP2)), 0U);
}

TEST_F(VagentEpistemicResume, LivenessStartsAgainFromTheFirstContact) {
  populate();
  ASSERT_TRUE(a.value(*a.profile(kP1), kProfileLastContact));
  ASSERT_TRUE(resumeOnB());
  const auto q1 = *b.profile(kP1);
  EXPECT_FALSE(b.value(q1, kProfileLastContact));
  EXPECT_FALSE(b.value(q1, kProfileLastReceive));
  b.bind(kP1);
  b.observe(reading(kP1, "left", 20, 1.0));
  EXPECT_TRUE(b.value(q1, kProfileLastContact));
}

TEST_F(VagentEpistemicResume, TheArenaFloorRefusesABootTheNewHostAccepted) {
  a.bind(kP1, 3);
  ASSERT_EQ(a.observe(reading(kP1, "left", 1, 1.0, 3))->kind,
            IngestOutcome::Kind::Committed);
  // kP2 is retired before the checkpoint: its tombstone travels too.
  a.bind(kP2, 2);
  ASSERT_TRUE(a.executor().retireSource(a.pid, kP2));
  a.host->pump();
  ASSERT_TRUE(resumeOnB());
  // The new host has never seen either floor, so it accepts both binds; the
  // arena refuses them in the advance turn and the host revokes them.
  for (const auto &[principal, incarnation] :
       {std::pair{kP1, std::uint64_t{2}}, std::pair{kP2, std::uint64_t{2}}}) {
    b.bind(principal, incarnation);
    const auto outcome = b.executor().sourceChangeOutcome(b.pid, principal);
    ASSERT_TRUE(outcome);
    EXPECT_EQ(outcome->code, kTurnResultStaleIncarnation);
    EXPECT_EQ(
        b.executor()
            .observe(b.pid, reading(principal, "left", 2, 1.0, incarnation))
            .error(),
        EpistemicRefusal::UnknownSource);
    // Now the host knows the floor and refuses at once.
    const auto again =
        b.executor().bindSource(b.pid, {"sensor:x", principal, incarnation,
                                        "at", std::chrono::hours(1)});
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), EpistemicRefusal::StaleIncarnation);
  }
  // Above the floor, the source continues.
  b.bind(kP1, 4);
  EXPECT_EQ(b.observe(reading(kP1, "left", 1, 1.0, 4))->kind,
            IngestOutcome::Kind::Committed);
}

TEST_F(VagentEpistemicResume, AChangedPolicyRefusesTheResume) {
  populate();
  std::string changed(kSource);
  changed.replace(changed.find("10s"), 3, "12s");
  // The program reference covers the policy, so resume refuses before any
  // state is restored; restoreState compares the hashes again regardless.
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  auto program = CompiledProgram::fromSource(changed);
  ASSERT_TRUE(program) << program.error();
  b.host.emplace(std::move(*program));
  EXPECT_FALSE(b.executor().resume(*saved));
}

TEST_F(VagentEpistemicResume, AFullEpistemicBudgetStillCheckpoints) {
  // Fill the cell budget with distinct sources until ingestion refuses it.
  std::uint64_t seq = 0;
  for (int s = 0; s < 64; ++s) {
    char digit[3];
    std::snprintf(digit, sizeof digit, "%02x", s + 16);
    const std::string principal = std::string(62, 'a') + digit;
    a.bind(principal);
    bool refused = false;
    for (const auto *key : {"k1", "k2", "k3", "k4"})
      for (int i = 0; i < 5 && !refused; ++i) {
        auto o       = reading(principal, key, ++seq, 1.0 + i);
        o.eventId    = principal.substr(60) + key + std::to_string(i);
        o.sequence   = seq;
        const auto r = a.observe(o);
        refused      = r && r->kind == IngestOutcome::Kind::Refused;
      }
    if (refused) break;
  }
  EXPECT_GE(*a.executor().epistemicCells(a.pid),
            AgentSettings{}.epistemic.cells * 9 / 10);
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(*b.executor().epistemicCells(b.pid) + 2 * b.profiles().size(),
            *a.executor().epistemicCells(a.pid));
}

} // namespace
