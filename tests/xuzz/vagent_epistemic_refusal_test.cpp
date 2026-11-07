/**
 * @file vagent_epistemic_refusal_test.cpp
 * @brief P19 T1a: resume and both upgrade overloads give back one typed
 *        refusal, decided by a pre-flight that changes nothing.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "epistemic_time.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::CellRef;
using zigzag::vortex::mesh::ActorError;
using zigzag::vortex::mesh::ActorGrant;

constexpr std::string_view kSource = R"vagent(
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

constexpr std::string_view kWithoutPredicate = R"vagent(
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
}
)vagent";

constexpr std::string_view kPlainPredicate = R"vagent(
predicate seen(x: Int)
agent B { var n: Int := 0 }
)vagent";

constexpr std::string_view kPlainWithout = R"vagent(
agent B { var n: Int := 0 }
)vagent";

const std::string kP1(64, '1');
const std::string kP2(64, '2');

std::string withPolicyAge(std::string_view source, std::string_view age) {
  std::string out(source);
  out.replace(out.find("10s"), 3, age);
  return out;
}

std::string withoutKey(std::string_view source) {
  std::string out(source);
  out.replace(out.find("key who"), 7, "who");
  return out;
}

/// A program under an explicit reference, which `fromSource` never gives: two
/// sources under one reference is the case that reaches `restoreState`.
std::optional<CompiledProgram> unitProgram(std::string_view source,
                                           std::string ref) {
  auto unit = parseUnit(source);
  EXPECT_TRUE(unit.has_value());
  if (!unit) return std::nullopt;
  auto program = CompiledProgram::fromUnit(std::move(*unit), "t", ref);
  EXPECT_TRUE(program.has_value()) << (program ? "" : program.error());
  if (!program) return std::nullopt;
  return std::move(*program);
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

CheckedObservation reading(const std::string &principal, std::string eventId,
                           std::uint64_t sequence, double base) {
  CheckedObservation o;
  o.principal   = principal;
  o.incarnation = 1;
  o.eventId     = std::move(eventId);
  o.sequence    = sequence;
  o.predicate   = "at";
  o.tuple       = {WireValue::text("self"), WireValue::real(1.0)};
  setTimes(o, base - 5.0 + 0.01 * static_cast<double>(sequence), 0.01);
  o.clockBasis = "wall";
  o.frame      = "enu";
  o.units      = "m";
  o.variance   = 0.25;
  return o;
}

// -- the vocabulary ---------------------------------------------------------

TEST(TransferRefusalTest, RefusalVocabularyIsStable) {
  static_assert(!std::is_default_constructible_v<TransferRefusal>,
                "make is the only constructor");
  const std::vector<TransferCode> codes{
      TransferCode::Runtime,
      TransferCode::NotQuiescent,
      TransferCode::ActiveIntention,
      TransferCode::PolicyChanged,
      TransferCode::PredicateIncompatible,
      TransferCode::PredicateRemoved,
      TransferCode::PeakCopyBudget,
      TransferCode::WorkBudget,
      TransferCode::ProgramReferenceReused,
      TransferCode::RouteCarriesNoEpistemicState,
      TransferCode::UnknownShapeVersion,
      TransferCode::PolicyEncodingChanged,
      TransferCode::NoTranslator,
      TransferCode::UnregisteredOwnedStructure,
      TransferCode::ReplayStateUnavailable,
      TransferCode::StateInconsistent};
  std::set<std::string_view> names;
  for (const auto code : codes) {
    const auto name = toString(code);
    EXPECT_NE(name, "Unknown");
    EXPECT_TRUE(names.insert(name).second) << name;
    const auto refusal = TransferRefusal::make(
        code, TransferRoute::UpgradeSchema, ActorError::NotQuiescent,
        {.subject      = "s",
         .saved        = "a",
         .current      = "b",
         .savedCount   = 1,
         .currentCount = 2,
         .limit        = 3});
    EXPECT_EQ(refusal.code, code);
    EXPECT_EQ(refusal.route, TransferRoute::UpgradeSchema);
    EXPECT_EQ(refusal.actor, ActorError::NotQuiescent);
    EXPECT_EQ(refusal.detail.subject, "s");
    EXPECT_EQ(refusal.detail.savedCount, 1U);
    EXPECT_EQ(refusal.detail.limit, 3U);
    EXPECT_NE(toString(refusal).find(name), std::string::npos);
  }
  // A field that does not apply stays absent rather than carrying a marker.
  const auto bare = TransferRefusal::make(
      TransferCode::Runtime, TransferRoute::Resume, ActorError::NotAuthorized);
  EXPECT_FALSE(bare.detail.subject);
  EXPECT_FALSE(bare.detail.limit);
  EXPECT_EQ(toString(bare), "Runtime(route=Resume)");

  const auto cut = TransferRefusal::make(
      TransferCode::PolicyChanged, TransferRoute::Resume,
      ActorError::ExecutionFailed,
      {.subject = std::string(300, 'p'), .saved = std::string(129, 's')});
  EXPECT_EQ(cut.detail.subject->size(), kRefusalTextLimit);
  EXPECT_EQ(cut.detail.saved->size(), kRefusalTextLimit);
  // The cut never ends inside a UTF-8 sequence (each is two bytes here).
  std::string wide;
  for (int i = 0; i < 100; ++i) wide += "\xC3\xA9";
  const auto utf =
      TransferRefusal::make(TransferCode::PolicyChanged, TransferRoute::Resume,
                            ActorError::ExecutionFailed, {.subject = wide});
  EXPECT_LE(utf.detail.subject->size(), kRefusalTextLimit);
  EXPECT_EQ(utf.detail.subject->size() % 2, 0U);
  // A control byte from an image cannot forge a log line through what().
  const auto forged = TransferRefusal::make(
      TransferCode::StateInconsistent, TransferRoute::Resume,
      ActorError::ExecutionFailed,
      {.subject = "a\nWARN fake\x1b[0m\x7f", .saved = "\r", .current = "\t"});
  EXPECT_EQ(forged.detail.subject, "a?WARN fake?[0m?");
  EXPECT_EQ(forged.detail.saved, "?");
  EXPECT_EQ(forged.detail.current, "?");
  EXPECT_EQ(toString(forged).find('\n'), std::string::npos);
}

// -- resume -----------------------------------------------------------------

class RefusalResume : public ::testing::Test {
protected:
  std::optional<LocalHost> a, b;
  ActorPid pidA;
  std::optional<AgentCheckpoint> saved;
  std::size_t cells{};

  void SetUp() override {
    a.emplace(std::move(*unitProgram(kSource, "ref-1")));
    const auto launched = a->launch("A", "a");
    ASSERT_TRUE(launched);
    pidA = *launched;
    a->pump();
    a->executor().setWallClockSynchronised(true);
    const auto base = wallSeconds();
    ASSERT_TRUE(a->executor().bindSource(
        pidA, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)}));
    a->pump();
    for (std::uint64_t i = 1; i <= 3; ++i) {
      ASSERT_TRUE(a->executor().observe(
          pidA, reading(kP1, "e" + std::to_string(i), i, base)));
      a->pump();
    }
    cells = *a->executor().epistemicCells(pidA);
    ASSERT_GT(cells, 0U);
    const auto checkpoint = a->executor().checkpoint(pidA);
    ASSERT_TRUE(checkpoint) << toString(checkpoint.error());
    saved = *checkpoint;
  }

  LocalHost &hostB(std::string_view source, std::string ref = "ref-1",
                   LocalHost::Options options = {}) {
    b.emplace(std::move(*unitProgram(source, std::move(ref))),
              std::move(options));
    return *b;
  }

  /// A refusal leaves no pid, no actor and nothing for the pump to do.
  void expectNoTrace() {
    EXPECT_EQ(b->runtime().actorCount(), 0U);
    EXPECT_EQ(b->pump(), 0U);
  }
};

TEST_F(RefusalResume,
       ResumeOfAChangedPolicyUnderTheSameReferenceRefusesBeforeAPid) {
  hostB(withPolicyAge(kSource, "12s"));
  const auto resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  const auto &refusal = resumed.error();
  EXPECT_EQ(refusal.code, TransferCode::PolicyChanged);
  EXPECT_EQ(refusal.route, TransferRoute::Resume);
  EXPECT_EQ(refusal.detail.subject, "at");
  ASSERT_TRUE(refusal.detail.saved);
  ASSERT_TRUE(refusal.detail.current);
  EXPECT_NE(*refusal.detail.saved, *refusal.detail.current);
  EXPECT_EQ(refusal.actor, ActorError::ExecutionFailed);
  expectNoTrace();
}

TEST_F(RefusalResume, AResumeRefusalLeavesNoTrace) {
  struct Scenario {
    const char *name;
    std::string source;
    TransferCode code;
    std::optional<ResumeLimits> probe;
    std::size_t ceiling{};
  };
  const std::vector<Scenario> scenarios{
      {"policy", withPolicyAge(kSource, "12s"), TransferCode::PolicyChanged,
       std::nullopt},
      {"removed", std::string(kWithoutPredicate),
       TransferCode::PredicateRemoved, std::nullopt},
      {"work", std::string(kSource), TransferCode::WorkBudget,
       ResumeLimits{cells * kTransferWorkPerCell - 1, 1U << 20}},
      {"peak", std::string(kSource), TransferCode::PeakCopyBudget,
       ResumeLimits{10'000'000, static_cast<std::uint32_t>(cells - 1)}},
      {"ceiling", std::string(kSource), TransferCode::PeakCopyBudget,
       std::nullopt, cells / 2}};
  for (const auto &scenario : scenarios) {
    SCOPED_TRACE(scenario.name);
    AgentSettings settings;
    if (scenario.ceiling != 0)
      settings.epistemic.cells = static_cast<std::int64_t>(scenario.ceiling);
    LocalHost::Options options;
    options.settings = settings;
    hostB(scenario.source, "ref-1", options);
    if (scenario.probe) {
      const auto limits = *scenario.probe;
      b->executor().setResumeGrantProbe(
          [limits](std::string_view)
              -> std::expected<ResumeLimits, ActorError> { return limits; });
    }
    const auto retained = b->executor().resolveProgram("ref-1");
    const auto resumed  = b->executor().resume(*saved);
    ASSERT_FALSE(resumed);
    EXPECT_EQ(resumed.error().code, scenario.code);
    EXPECT_EQ(b->executor().resolveProgram("ref-1"), retained);
    expectNoTrace();
  }
}

TEST_F(RefusalResume, AResumeWithAMissingProgramIsRuntimeAndDecidedFirst) {
  // The reference is content-derived here, so the changed policy is also a
  // program this host does not hold: the missing program is what is reported.
  auto program = CompiledProgram::fromSource(withPolicyAge(kSource, "12s"));
  ASSERT_TRUE(program) << program.error();
  b.emplace(std::move(*program));
  const auto resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::Runtime);
  EXPECT_EQ(resumed.error().actor, ActorError::NotAuthorized);
  expectNoTrace();
}

TEST_F(RefusalResume,
       AResumeRefusedByTheRuntimeAfterThePreflightHasAKnownResidue) {
  hostB(kSource);
  ASSERT_FALSE(b->executor().preflightResume(*saved));
  // The runtime's own authorizer is the next layer down.
  b->runtime().setAuthorizer(
      [](const xanadu::identity::Fingerprint &,
         const auto &) -> std::expected<ActorGrant, ActorError> {
        return std::unexpected(ActorError::NotAuthorized);
      });
  const auto resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::Runtime);
  EXPECT_EQ(resumed.error().actor, ActorError::NotAuthorized);
  // The trust set and the issued capability are the residue the design names;
  // neither is observable here. No pid and no actor is.
  expectNoTrace();
}

TEST_F(RefusalResume, ResumeBudgetRefusalsComeBeforeAPid) {
  hostB(kSource);
  const auto probeOf = [&](std::uint64_t instructions, std::uint32_t maxCells) {
    b->executor().setResumeGrantProbe(
        [=](std::string_view) -> std::expected<ResumeLimits, ActorError> {
          return ResumeLimits{instructions, maxCells};
        });
  };
  const auto work = cells * kTransferWorkPerCell;
  probeOf(work - 1, 1U << 20);
  auto resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::WorkBudget);
  EXPECT_EQ(resumed.error().detail.savedCount, work);
  EXPECT_EQ(resumed.error().detail.limit, work - 1);
  EXPECT_EQ(resumed.error().actor, ActorError::ResourceLimit);

  probeOf(work, static_cast<std::uint32_t>(cells - 1));
  resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::PeakCopyBudget);
  EXPECT_EQ(resumed.error().detail.savedCount, cells);
  EXPECT_EQ(resumed.error().detail.limit, cells - 1);
  expectNoTrace();

  // With no probe the settings half still refuses before a pid.
  AgentSettings settings;
  settings.epistemic.cells = static_cast<std::int64_t>(cells / 2);
  LocalHost::Options options;
  options.settings = settings;
  hostB(kSource, "ref-1", options);
  b->executor().setResumeGrantProbe({});
  resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::PeakCopyBudget);
  EXPECT_EQ(resumed.error().detail.savedCount, cells);
  expectNoTrace();

  // At the allowance exactly, the resume is admitted.
  hostB(kSource);
  probeOf(work, static_cast<std::uint32_t>(cells));
  EXPECT_TRUE(b->executor().resume(*saved));
}

TEST_F(RefusalResume, AHostWithNoProbeReachesTheApplyTimeBackstop) {
  LocalHost::Options options;
  options.maxInstructions = cells * kTransferWorkPerCell - 1;
  hostB(kSource, "ref-1", options);
  b->executor().setResumeGrantProbe({});
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed) << toString(resumed.error());
  EXPECT_FALSE(b->executor().lastRefusal(*resumed));
  b->pump();
  // The first turn ends in fatal(init), and the refusal is kept for the
  // holder of the pid.
  EXPECT_FALSE(b->executor().context(*resumed));
  const auto stored = b->executor().lastRefusal(*resumed);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->code, TransferCode::WorkBudget);
  EXPECT_EQ(stored->route, TransferRoute::Resume);
  EXPECT_EQ(stored->detail.limit, options.maxInstructions);
}

TEST_F(RefusalResume, PreflightResumeIsADryRun) {
  hostB(withPolicyAge(kSource, "12s"));
  const auto dry = b->executor().preflightResume(*saved);
  ASSERT_TRUE(dry);
  const auto real = b->executor().resume(*saved);
  ASSERT_FALSE(real);
  EXPECT_EQ(dry->code, real.error().code);
  EXPECT_EQ(dry->detail.subject, real.error().detail.subject);
  EXPECT_EQ(dry->detail.saved, real.error().detail.saved);
  EXPECT_EQ(dry->detail.current, real.error().detail.current);
  expectNoTrace();

  hostB(kSource);
  EXPECT_FALSE(b->executor().preflightResume(*saved));
  EXPECT_EQ(b->runtime().actorCount(), 0U);
  const auto resumed = b->executor().resume(*saved);
  ASSERT_TRUE(resumed) << toString(resumed.error());
  EXPECT_EQ(b->runtime().actorCount(), 1U);
}

TEST_F(RefusalResume, AMissingProbeAnswerIsTheHostsOwnRuntimeError) {
  hostB(kSource);
  b->executor().setResumeGrantProbe(
      [](std::string_view) -> std::expected<ResumeLimits, ActorError> {
        return std::unexpected(ActorError::NotAuthorized);
      });
  const auto resumed = b->executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::Runtime);
  EXPECT_EQ(resumed.error().actor, ActorError::NotAuthorized);
  expectNoTrace();
}

// -- upgrade ----------------------------------------------------------------

class RefusalUpgrade : public ::testing::Test {
protected:
  std::optional<LocalHost> host;
  ActorPid pid;
  double base{};
  std::size_t versions{};

  void SetUp() override { start(kSource); }

  void start(std::string_view source) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    host.emplace(std::move(*program));
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    base = wallSeconds();
    for (const auto &[principal, alias] :
         {std::pair{kP1, "sensor:gps"}, std::pair{kP2, "sensor:odometry"}})
      ASSERT_TRUE(host->executor().bindSource(
          pid, {alias, principal, 1, "at", std::chrono::hours(1)}));
  }

  void settle() {
    host->pump();
    host->executor().drainNotifications();
  }

  /// A distinct version of the same source, for each call.
  CompiledProgram version(std::string_view source = kSource) {
    ++versions;
    auto program = CompiledProgram::fromSource(std::string(source) +
                                               std::string(versions, '\n'));
    EXPECT_TRUE(program) << program.error();
    return std::move(*program);
  }

  struct Before {
    const zigzag::vortex::WorkerContext *context;
    std::string programRef;
    std::size_t cells;
    std::size_t actors;
  };
  Before before() {
    return {host->executor().context(pid),
            std::string(*host->executor().programRef(pid)),
            host->executor().context(pid)->arena.cellCount(),
            host->runtime().actorCount()};
  }
  void expectUnchanged(const Before &was) {
    EXPECT_EQ(host->executor().context(pid), was.context);
    EXPECT_EQ(*host->executor().programRef(pid), was.programRef);
    EXPECT_EQ(host->executor().context(pid)->arena.cellCount(), was.cells);
    EXPECT_EQ(host->runtime().actorCount(), was.actors);
  }

  void injectIntention() {
    auto &ctx = *host->executor().context(pid);
    auto dims = registerDimensions(ctx.arena);
    appendToRank(ctx.arena, *host->executor().agentCell(pid), dims.intention,
                 ctx.arena.makeCell("intention"));
  }
  void observeOnce() {
    ASSERT_TRUE(host->executor().observe(pid, reading(kP1, "e1", 1, base)));
    host->pump();
  }
  static ActorGrant grantFor(const CompiledProgram &program,
                             std::uint64_t instructions, std::uint32_t cells) {
    return {std::string(program.programRef()),
            std::string(program.programRef()),
            instructions,
            cells,
            32768,
            4096,
            1U << 20,
            std::chrono::system_clock::now() + std::chrono::hours(1),
            true};
  }
  static void registerTranslator(LocalHost &) {}
};

TEST_F(RefusalUpgrade, EveryUpgradeRefusalIsTyped) {
  const auto expectRefusal = [&](const std::expected<void, TransferRefusal> &r,
                                 TransferCode code, std::string_view subject) {
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, code) << toString(r.error());
    EXPECT_EQ(r.error().route, TransferRoute::UpgradeSchema);
    if (!subject.empty()) EXPECT_EQ(r.error().detail.subject, subject);
  };

  // Queued source changes have not run yet.
  auto was  = before();
  auto held = host->retainProgram(version());
  auto r    = host->upgrade(pid, held);
  expectRefusal(r, TransferCode::NotQuiescent, "posts");
  EXPECT_EQ(r.error().detail.savedCount, 1U);
  EXPECT_EQ(r.error().actor, ActorError::NotQuiescent);
  expectUnchanged(was);

  settle();
  injectIntention();
  was = before();
  r   = host->upgrade(pid, held);
  expectRefusal(r, TransferCode::ActiveIntention, "");
  EXPECT_EQ(r.error().detail.savedCount, 1U);
  EXPECT_FALSE(r.error().detail.limit);
  expectUnchanged(was);
}

TEST_F(RefusalUpgrade, ASchemaChangeIsNamedByPredicate) {
  settle();
  const auto was    = before();
  const auto retain = [&](std::string source) {
    auto program = CompiledProgram::fromSource(source);
    EXPECT_TRUE(program) << program.error();
    return host->retainProgram(std::move(*program));
  };

  auto changed = retain(withPolicyAge(kSource, "12s"));
  auto r       = host->upgrade(pid, changed);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PolicyChanged);
  EXPECT_EQ(r.error().detail.subject, "at");
  EXPECT_EQ(r.error().detail.current,
            changed->upgradeSchema().predicates.at("at").epistemicPolicy->hash);
  ASSERT_TRUE(r.error().detail.saved);
  EXPECT_NE(r.error().detail.saved, r.error().detail.current);
  expectUnchanged(was);

  auto typed = retain(withoutKey(kSource));
  r          = host->upgrade(pid, typed);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PredicateIncompatible);
  EXPECT_EQ(r.error().detail.subject, "at");
  EXPECT_EQ(r.error().detail.saved, "at(key Str, Float)");
  EXPECT_EQ(r.error().detail.current, "at(Str, Float)");
  expectUnchanged(was);

  auto dropped = retain(std::string(kWithoutPredicate));
  r            = host->upgrade(pid, dropped);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PredicateRemoved);
  EXPECT_EQ(r.error().detail.subject, "at");
  ASSERT_TRUE(r.error().detail.saved);
  expectUnchanged(was);
}

TEST_F(RefusalUpgrade, TheBudgetsAreDecidedFromTheGrant) {
  settle();
  const auto was = before();
  auto held      = host->retainProgram(version());
  // The schema comparison alone costs more than one instruction.
  auto r = host->upgrade(pid, held, 1);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::WorkBudget);
  EXPECT_EQ(r.error().actor, ActorError::ResourceLimit);
  EXPECT_EQ(r.error().detail.limit, 1U);
  EXPECT_GT(*r.error().detail.savedCount, 1U);
  expectUnchanged(was);

  // With profiles and a translator to carry them, the copy is budgeted too.
  observeOnce();
  settle();
  registerTranslator(*host);
  const auto populated = before();
  const auto cells     = *host->executor().epistemicCells(pid);
  r = host->executor().upgrade(pid, held, grantFor(*held, 10'000'000, 1));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PeakCopyBudget);
  EXPECT_EQ(r.error().detail.savedCount, cells);
  EXPECT_EQ(r.error().detail.limit, 1U);
  expectUnchanged(populated);

  r = host->executor().upgrade(pid, held, grantFor(*held, 10, 1U << 20));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::WorkBudget);
  EXPECT_GE(*r.error().detail.savedCount, cells * kTransferWorkPerCell);
  expectUnchanged(populated);
}

TEST_F(RefusalUpgrade, PreflightUpgradeIsADryRun) {
  auto held = host->retainProgram(version());
  auto dry  = host->executor().preflightUpgrade(
      pid, *held, AgentExecutor::UpgradeRoute::Schema);
  ASSERT_TRUE(dry);
  const auto was  = before();
  const auto real = host->upgrade(pid, held);
  ASSERT_FALSE(real);
  EXPECT_EQ(dry->code, real.error().code);
  EXPECT_EQ(dry->detail.subject, real.error().detail.subject);
  EXPECT_EQ(dry->detail.savedCount, real.error().detail.savedCount);
  expectUnchanged(was);

  settle();
  EXPECT_FALSE(host->executor().preflightUpgrade(
      pid, *held, AgentExecutor::UpgradeRoute::Schema));
  EXPECT_EQ(*host->executor().programRef(pid), before().programRef);
  EXPECT_NE(*host->executor().programRef(pid), held->programRef());
  EXPECT_TRUE(host->upgrade(pid, held));

  const auto report = host->executor().quiescence(pid);
  ASSERT_TRUE(report);
  EXPECT_TRUE(report->quiet());
  const auto unknown = host->executor().quiescence(ActorPid{"x", "y", 9, 9});
  ASSERT_FALSE(unknown);
  EXPECT_EQ(unknown.error().actor, ActorError::UnknownProcess);
}

TEST_F(RefusalUpgrade, TheReconcilerRoadComparesPolicy) {
  settle();
  const auto was = before();
  const auto r   = host->upgrade(pid, version(withPolicyAge(kSource, "12s")));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::PolicyChanged);
  EXPECT_EQ(r.error().route, TransferRoute::UpgradeReconciler);
  EXPECT_EQ(r.error().detail.subject, "at");
  expectUnchanged(was);
}

TEST_F(RefusalUpgrade, ARemovedBoundOrArbitratedPredicateIsRefusedOnBothRoads) {
  settle();
  const auto was = before();
  // Both sources are bound and no reading has made a profile.
  auto held         = host->retainProgram(version(kWithoutPredicate));
  const auto shared = host->upgrade(pid, held);
  ASSERT_FALSE(shared);
  EXPECT_EQ(shared.error().code, TransferCode::PredicateRemoved);
  EXPECT_EQ(shared.error().route, TransferRoute::UpgradeSchema);
  const auto byValue = host->upgrade(pid, version(kWithoutPredicate));
  ASSERT_FALSE(byValue);
  EXPECT_EQ(byValue.error().code, TransferCode::PredicateRemoved);
  EXPECT_EQ(byValue.error().route, TransferRoute::UpgradeReconciler);
  EXPECT_EQ(byValue.error().detail.subject, "at");
  expectUnchanged(was);
  // The binding is intact: a reading is still admitted.
  ASSERT_TRUE(host->executor().observe(pid, reading(kP1, "e1", 1, base)));
  host->pump();
}

TEST(RefusalPlain, APlainUnboundPredicateCanStillBeDropped) {
  const auto upgradeBy = [](bool byValue) {
    auto first = CompiledProgram::fromSource(kPlainPredicate);
    EXPECT_TRUE(first);
    LocalHost host(std::move(*first));
    const auto pid = host.launch("B", "b");
    EXPECT_TRUE(pid);
    host.pump();
    auto second = CompiledProgram::fromSource(kPlainWithout);
    EXPECT_TRUE(second);
    if (byValue) return host.upgrade(*pid, std::move(*second));
    return host.upgrade(*pid, host.retainProgram(std::move(*second)));
  };
  const auto byValue = upgradeBy(true);
  EXPECT_TRUE(byValue) << (byValue ? "" : toString(byValue.error()));
  const auto shared = upgradeBy(false);
  EXPECT_TRUE(shared) << (shared ? "" : toString(shared.error()));
}

TEST_F(RefusalUpgrade, ThePublicEntryPointsAgree) {
  const auto agree = [&](const char *what, auto makeNext) {
    SCOPED_TRACE(what);
    auto next          = makeNext();
    const auto byValue = host->upgrade(pid, std::move(next.first));
    const auto shared =
        host->upgrade(pid, host->retainProgram(std::move(next.second)));
    ASSERT_FALSE(byValue);
    ASSERT_FALSE(shared);
    EXPECT_EQ(byValue.error().code, shared.error().code);
    EXPECT_EQ(byValue.error().detail.subject, shared.error().detail.subject);
    EXPECT_EQ(byValue.error().detail.saved, shared.error().detail.saved);
    EXPECT_EQ(byValue.error().detail.current, shared.error().detail.current);
    EXPECT_EQ(byValue.error().detail.savedCount,
              shared.error().detail.savedCount);
    EXPECT_EQ(byValue.error().route, TransferRoute::UpgradeReconciler);
    EXPECT_EQ(shared.error().route, TransferRoute::UpgradeSchema);
  };
  const auto twin = [&](std::string_view source) {
    return std::pair{version(source), version(source)};
  };
  // Posts are still queued.
  agree("queued", [&] { return twin(kSource); });
  settle();
  agree("policy", [&] { return twin(withPolicyAge(kSource, "12s")); });
  agree("removed", [&] { return twin(kWithoutPredicate); });

  // Populated state is the one difference, and it is named.
  observeOnce();
  settle();
  const auto byValue = host->upgrade(pid, version());
  ASSERT_FALSE(byValue);
  EXPECT_EQ(byValue.error().code, TransferCode::RouteCarriesNoEpistemicState);
  EXPECT_EQ(byValue.error().detail.subject, "d.trust");
  EXPECT_EQ(byValue.error().detail.current, "the shared-pointer overload");
  // The schema road carries it with the default translator (T1c).
  const auto shared = host->upgrade(pid, host->retainProgram(version()));
  EXPECT_TRUE(shared) << (shared ? "" : toString(shared.error()));
}

TEST_F(RefusalUpgrade, ASameReferenceDifferentProgramIsRefusedByName) {
  host.emplace(std::move(*unitProgram(kSource, "R1")));
  const auto launched = host->launch("A", "a");
  ASSERT_TRUE(launched);
  pid = *launched;
  settle();
  const auto held = host->executor().resolveProgram("R1");
  ASSERT_TRUE(held);
  const auto was = before();

  auto r = host->upgrade(
      pid, std::move(*unitProgram(withPolicyAge(kSource, "12s"), "R1")));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::ProgramReferenceReused);
  EXPECT_EQ(r.error().detail.subject, "R1");
  EXPECT_EQ(r.error().actor, ActorError::NotAuthorized);
  EXPECT_EQ(host->executor().resolveProgram("R1"), held);
  expectUnchanged(was);

  // An equal schema is the held program, as `retainProgram` documents.
  r = host->upgrade(pid, std::move(*unitProgram(kSource, "R1")));
  EXPECT_TRUE(r) << (r ? "" : toString(r.error()));
  EXPECT_EQ(host->executor().resolveProgram("R1"), held);
}

TEST_F(RefusalUpgrade, APreflightRefusalLeavesTheRegistriesAsTheyWere) {
  settle();
  observeOnce();
  settle();
  auto next = version();
  const std::string ref(next.programRef());
  const auto r = host->upgrade(pid, std::move(next));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::RouteCarriesNoEpistemicState);
  // Not retained: neither the host's registry nor the executor's has it.
  EXPECT_FALSE(host->executor().resolveProgram(ref));
  EXPECT_FALSE(host->executor().resolveProgram(ref).get());
  auto direct = host->retainProgram(version());
  EXPECT_TRUE(direct);
}

TEST(RefusalPlain, ANonEpistemicAgentWithAnAskStillReconcilesByValue) {
  constexpr std::string_view kV1 = R"vagent(
goal QueryGoal() achieve
plan p1 for QueryGoal() { yield }
agent Boss { var last_ans: Int := 0 }
)vagent";
  auto first                     = CompiledProgram::fromSource(kV1);
  auto second = CompiledProgram::fromSource(std::string(kV1) + "\n");
  ASSERT_TRUE(first && second);
  const auto wall = std::chrono::system_clock::now();
  LocalHost host(std::move(*first));
  const auto owner = *xanadu::identity::Fingerprint::fromString(
      "0123456789abcdef0123456789abcdef01234567");
  host.runtime().trustNode("worker", owner);
  const auto pid = host.launch("Boss", "boss-1", {});
  ASSERT_TRUE(pid);
  host.pump();

  auto *ctx      = host.executor().context(*pid);
  const auto ac  = *host.executor().agentCell(*pid);
  const auto vd  = valueDims(ctx->core);
  const auto dim = registerDimensions(ctx->arena);
  const ActorPid remote{"worker", "remote", 1, 1};
  const auto peers = ctx->arena.makeCell();
  setField(ctx->arena, vd, peers, "worker",
           ctx->arena.makeCell(pidToText(remote)));
  setField(ctx->arena, vd, ac, kFieldPeers, peers);
  const auto intent = ctx->arena.makeCell("intention");
  setField(ctx->arena, vd, intent, kIntentionId,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  setField(ctx->arena, vd, intent, kIntentionGoal,
           ctx->arena.makeCell("QueryGoal"));
  setField(ctx->arena, vd, intent, kIntentionPlan, ctx->arena.makeCell("p1"));
  WireRecord waiting;
  waiting.type = "Waiting";
  waiting.set(std::string(kWaitingKind),
              WireValue::text(std::string(kWaitKindAsk)));
  waiting.set(std::string(kWaitingKey), WireValue::integer(777));
  waiting.set(std::string(kWaitingTo), WireValue::text("worker"));
  setField(ctx->arena, vd, intent, kIntentionWaiting,
           writeValue(ctx->arena, vd, WireValue::record(waiting)));
  appendToRank(ctx->arena, ac, dim.intention, intent);
  const auto entry = ctx->arena.makeCell(std::string(kKindAsk));
  setField(ctx->arena, vd, entry, kOutboxReqId,
           ctx->arena.makeScalarCell(std::int64_t{777}));
  setField(ctx->arena, vd, entry, kOutboxReqTo, ctx->arena.makeCell("worker"));
  setField(ctx->arena, vd, entry, kOutboxReqName, ctx->arena.makeCell("calc"));
  setField(ctx->arena, vd, entry, kOutboxReqDeadline,
           ctx->arena.makeScalarCell(std::int64_t{9999999999}));
  setField(ctx->arena, vd, entry, kOutboxReqIntention,
           ctx->arena.makeScalarCell(std::int64_t{101}));
  appendToRank(ctx->arena, ac, dim.outbox, entry);
  xanadu::identity::ActorMessage sent;
  host.runtime().setSender(
      [&](std::string_view, const xanadu::identity::ActorMessage &message) {
        sent = message;
        return true;
      });
  ASSERT_TRUE(host.runtime().wake(*pid, wall));
  host.pump();
  ASSERT_FALSE(sent.requestId.empty());

  const auto upgraded = host.upgrade(*pid, std::move(*second));
  ASSERT_TRUE(upgraded) << toString(upgraded.error());
  host.pump();

  xanadu::identity::ActorMessage reply;
  reply.type      = xanadu::identity::MessageType::ActorDeliverValue;
  reply.requestId = sent.requestId;
  reply.source    = remote;
  reply.target    = *pid;
  reply.value     = *encodeReply(Reply{WireValue::integer(42), {}});
  ASSERT_TRUE(host.runtime().receive(owner, reply,
                                     zigzag::vortex::mesh::ActorTime{}, wall));
  host.pump();
  EXPECT_FALSE(host.executor().readyIntentions(*pid).empty());
  EXPECT_EQ(host.executor().stat(*pid, "reply_stale"), 0U);
}

} // namespace
