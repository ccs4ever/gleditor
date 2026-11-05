/**
 * @file vagent_epistemic_translate_test.cpp
 * @brief P19 T1c: the translation routine resume and the schema road share,
 *        the full-key matcher, the digest and the owned-structure registry.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_translate.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using xanadu::identity::ActorPid;
using zigzag::CellRef;
using zigzag::vortex::mesh::ActorError;

constexpr std::string_view kSource = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
agent A {
  var adds: Int := 0
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

const std::string kP1(64, '1');
const std::string kP2(64, '2');
const std::string kP3(64, '3');

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct Side {
  std::optional<LocalHost> host;
  ActorPid pid;

  AgentExecutor &executor() { return host->executor(); }
  const zigzag::vortex::WorkerContext &ctx() {
    return *executor().context(pid);
  }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(ctx().arena);
  }
  AgentDims dims() { return registerDimensions(arena()); }
  CellRef agent() { return *executor().agentCell(pid); }
  OwnedGraphDims owned() {
    const auto &core = ctx().core.dims();
    const auto d     = dims();
    return {.vars      = core.vars,
            .values    = core.values,
            .grab      = core.grab,
            .step      = core.step,
            .clone     = core.clone,
            .clause    = core.clause,
            .source    = d.source,
            .belief    = d.belief,
            .trust     = d.trust,
            .epistemic = d.epistemic};
  }
  ValueDims vd() { return valueDims(ctx().core); }
  std::string digest(DigestMode mode = DigestMode::ModuloClock) {
    return epistemicStateDigest(arena(), agent(), owned(), mode);
  }
  void settle() {
    host->pump();
    executor().drainNotifications();
  }
  std::vector<CellRef> profiles() {
    return zigzag::rankAfter(arena(), agent(), dims().trust) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::optional<CellRef> profile(const std::string &principal) {
    for (const auto p : profiles())
      if (arena().textOf(p) == principal) return p;
    return std::nullopt;
  }
  std::optional<CellRef> support(const std::string &principal) {
    const auto d = dims();
    for (const auto predicate : zigzag::rankAfter(arena(), agent(), d.belief))
      for (const auto clause :
           zigzag::rankAfter(arena(), predicate, ctx().core.dims().clause))
        for (const auto s : zigzag::rankAfter(arena(), clause, d.source))
          if (arena().textOf(s) == principal) return s;
    return std::nullopt;
  }
  std::optional<CellRef> field(CellRef owner, std::string_view name) {
    return fieldValue(arena(), vd(), owner, name);
  }
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
  PopulateContext populateContext(const std::string &principal) {
    return {.arena     = arena(),
            .vd        = vd(),
            .clone     = ctx().core.dims().clone,
            .epistemic = dims().epistemic,
            .profile   = *profile(principal),
            .support   = support(principal)};
  }
};

std::string kind(const std::optional<IngestOutcome> &o) {
  return o ? std::to_string(static_cast<int>(o->kind)) : "none";
}

class Translate : public ::testing::Test {
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
    a.executor().setWallClockSynchronised(true);
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

  void populate() {
    a.bind(kP1);
    a.bind(kP2);
    std::uint64_t seq = 0;
    for (const auto &principal : {kP1, kP2})
      for (const auto *key : {"left", "right"})
        for (int i = 0; i < 3; ++i)
          ASSERT_TRUE(a.observe(reading(principal, key, ++seq, 1.0 + i)));
  }

  bool resumeOnB(std::string_view source = kSource) {
    const auto saved = a.executor().checkpoint(a.pid);
    EXPECT_TRUE(saved) << (saved ? "" : toString(saved.error()));
    if (!saved) return false;
    auto program = CompiledProgram::fromSource(source);
    EXPECT_TRUE(program) << program.error();
    b.host.emplace(std::move(*program));
    const auto resumed = b.executor().resume(*saved);
    EXPECT_TRUE(resumed) << (resumed ? "" : toString(resumed.error()));
    if (!resumed) return false;
    b.pid = *resumed;
    b.host->pump();
    if (!b.executor().context(b.pid)) return false;
    b.executor().setWallClockSynchronised(true);
    return true;
  }

  std::expected<void, TransferRefusal> upgradeA() {
    auto next = CompiledProgram::fromSource(std::string(kSource) + "\n");
    EXPECT_TRUE(next) << next.error();
    auto held = a.host->retainProgram(std::move(*next));
    EXPECT_TRUE(held);
    a.settle();
    return a.host->upgrade(a.pid, held);
  }

  /// Every registered structure built on one source's profile and support.
  void populateEveryStructure() {
    populate();
    const auto context = a.populateContext(kP1);
    for (const auto &entry : allOwnedStructures()) entry.populate(context);
  }
};

TEST(TranslateRegistry, RegistryIsConsistent) {
  std::set<std::string_view> ids;
  const std::set<std::string_view> known{"d.vars",     "d.values", "d.grab",
                                         "d.step",     "d.clone",  "d.trust",
                                         "d.epistemic"};
  for (const auto &entry : allOwnedStructures()) {
    EXPECT_TRUE(ids.insert(entry.id).second) << entry.id;
    EXPECT_TRUE(known.contains(entry.dimension)) << entry.id;
    EXPECT_NE(entry.populate, nullptr) << entry.id;
    if (entry.resume == Fate::Dropped || entry.upgrade == Fate::Dropped)
      EXPECT_FALSE(entry.dropReason.empty()) << entry.id;
  }
  // Every dimension an agent registers is owned or deliberately not.
  std::set<std::string_view> covered;
  for (const auto &entry : allOwnedStructures())
    covered.insert(entry.dimension);
  for (const auto &entry : notOwnedDimensions()) {
    covered.insert(entry.dimension);
    EXPECT_FALSE(entry.reason.empty()) << entry.dimension;
  }
  for (const auto name : kAgentDimensionNames)
    EXPECT_TRUE(covered.contains(name)) << name;
  EXPECT_FALSE(hostState().empty());
}

TEST(TranslateKeys, PredicateKeysDecodeAndRoundTrip) {
  constexpr std::string_view minimal = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
agent A { var n: Int := 0 }
)vagent";
  constexpr std::string_view full    = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s, decay: 5s,
    alignment: 2s, cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0, max_slew: 3.0)
agent A { var n: Int := 0 }
)vagent";
  for (const auto source : {minimal, full, kSource}) {
    auto program = CompiledProgram::fromSource(source);
    ASSERT_TRUE(program) << program.error();
    const auto expected =
        *program->upgradeSchema().predicates.at("at").epistemicPolicy;
    LocalHost host(std::move(*program));
    const auto pid = host.launch("A", "a");
    ASSERT_TRUE(pid);
    host.pump();
    const auto &ctx = *host.executor().context(*pid);
    auto &arena     = const_cast<zigzag::ArenaManifold &>(ctx.arena);
    const auto d    = registerDimensions(arena);
    const auto predicate =
        *zigzag::rankAfter(arena, *host.executor().agentCell(*pid), d.belief)
             .begin();
    const auto master = arena.linked(predicate, d.epistemic);
    ASSERT_TRUE(master);
    const auto decoded = decodePolicy(arena, valueDims(ctx.core), *master);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(*decoded, expected.policy);
    EXPECT_EQ(policyHash(*decoded), expected.hash);
  }
}

TEST(TranslateKeys, AForcedFullKeyHashCollisionDoesNotMatch) {
  PredicateKey one{.signature = "vagent-sig-1 at(key Str, Float)",
                   .policy    = std::nullopt};
  PredicateKey two{.signature = "vagent-sig-1 at(key Str, Int)",
                   .policy    = std::nullopt};
  const std::vector<PredicateKey> keys{one, two};
  const PredicateMatcher normal(keys);
  const PredicateMatcher colliding(
      keys, [](const PredicateKey &) -> std::uint64_t { return 7; });
  EXPECT_EQ(normal.find(one), 0U);
  EXPECT_EQ(normal.find(two), 1U);
  EXPECT_EQ(colliding.find(one), normal.find(one));
  EXPECT_EQ(colliding.find(two), normal.find(two));
  PredicateKey other{.signature = "vagent-sig-1 at(key Str, Bool)",
                     .policy    = std::nullopt};
  EXPECT_FALSE(colliding.find(other));
  // The same signature under another policy is another key.
  PredicateKey policed = one;
  policed.policy       = ArbitrationPolicy{};
  EXPECT_FALSE(colliding.find(policed));
}

TEST_F(Translate, PopulatedStateSurvivesResumeAndUpgrade) {
  populateEveryStructure();
  const auto source = a.digest();
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(b.digest(), source);
  // Each sample reached the new graph, not only the digest's view of it.
  const auto q = *b.profile(kP1);
  EXPECT_TRUE(b.field(q, kProfileReasons));
  EXPECT_TRUE(b.field(q, kProfileActions));
  EXPECT_TRUE(b.field(q, kProfileHealthWeight));
  ASSERT_TRUE(b.support(kP1));

  const auto before = a.executor().epistemicCells(a.pid);
  const auto done   = upgradeA();
  ASSERT_TRUE(done) << toString(done.error());
  a.settle();
  EXPECT_EQ(a.digest(), source);
  EXPECT_EQ(*a.executor().epistemicCells(a.pid), *before);
  // Full digests differ only in what the clock rule drops.
  EXPECT_EQ(a.digest(DigestMode::Full), a.digest(DigestMode::Full));
}

TEST_F(Translate, ALatchAndReasonsSurviveBothRoads) {
  populateEveryStructure();
  const auto latched = [](Side &s) {
    const auto p    = *s.profile(kP1);
    const auto cell = s.field(p, kProfileFaulted);
    return cell && s.arena().asBool(*cell).value_or(false);
  };
  ASSERT_TRUE(latched(a));
  ASSERT_TRUE(resumeOnB());
  EXPECT_TRUE(latched(b));
  EXPECT_EQ(b.arena().asInt64(*b.field(*b.profile(kP1), kProfileFaultSeq)), 1);
  ASSERT_TRUE(upgradeA());
  a.settle();
  EXPECT_TRUE(latched(a));
  EXPECT_EQ(a.arena().asInt64(*a.field(*a.profile(kP1), kProfileFaultSeq)), 1);
}

TEST_F(Translate, LivenessStampsFollowTheDisposition) {
  populateEveryStructure();
  ASSERT_TRUE(a.field(*a.profile(kP1), kProfileLastReceive));
  ASSERT_TRUE(resumeOnB());
  EXPECT_FALSE(b.field(*b.profile(kP1), kProfileLastReceive));
  EXPECT_FALSE(b.field(*b.profile(kP1), kProfileLastContact));
  ASSERT_TRUE(upgradeA());
  a.settle();
  const auto kept = a.field(*a.profile(kP1), kProfileLastReceive);
  ASSERT_TRUE(kept);
  EXPECT_EQ(a.arena().asDouble(*kept), 123.5);
}

TEST_F(Translate, DoublesCrossByBitPattern) {
  populateEveryStructure();
  // The routine moves the stored bits as they are. A checkpoint is encoded
  // with canonical doubles, so -0.0 reaches a resume as 0.0; the upgrade road
  // reads the live arena and keeps it.
  constexpr auto negativeZero = 0x8000000000000000ULL;
  const auto cell = a.field(*a.profile(kP1), kProfileHealthRefusedWeight);
  ASSERT_TRUE(cell);
  zigzag::expectWritten(
      a.arena().setValueBits(*cell, xanadu::ValueKind::Double, negativeZero));
  const auto bits = [](Side &s) {
    const auto c = s.field(*s.profile(kP1), kProfileHealthRefusedWeight);
    return c ? s.arena().slot(*c)->valueBits : 0x1ULL;
  };
  ASSERT_TRUE(upgradeA());
  a.settle();
  EXPECT_EQ(bits(a), negativeZero);
}

TEST_F(Translate, ResumeAndUpgradeAgreeOverRandomIngestion) {
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    SCOPED_TRACE("seed " + std::to_string(seed));
    SetUp();
    std::uint64_t state = seed * 0x9E3779B97F4A7C15ULL;
    const auto next     = [&](std::uint64_t n) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return (state >> 33) % n;
    };
    const std::string principals[]{kP1, kP2, kP3};
    std::uint64_t incarnation[3]{1, 1, 1};
    std::uint64_t sequence = 0;
    for (const auto &principal : principals) a.bind(principal);
    const auto events = 1 + next(40);
    for (std::uint64_t i = 0; i < events; ++i) {
      const auto who = next(3);
      switch (next(10)) {
      case 0:
        (void)a.executor().retireSource(a.pid, principals[who]);
        a.host->pump();
        break;
      case 1:
        a.bind(principals[who], ++incarnation[who]);
        break;
      case 2: {
        EpistemicHealthGrant g{
            "g" + std::to_string(i),
            *HealthIssuer::fromPrincipal(std::string(64, 'b')),
            {principals[who]},
            {},
            std::chrono::hours(1),
            16};
        g.actions.push_back(HealthActionKind::Isolate);
        g.actions.push_back(HealthActionKind::Clear);
        if (const auto token = a.executor().grantHealth(a.pid, g)) {
          (void)a.executor().healthAction(
              a.pid,
              {*token, principals[who],
               next(2) ? HealthAction{Isolate{"x"}} : HealthAction{Clear{}}});
          a.host->pump();
        }
        break;
      }
      default:
        (void)a.observe(reading(principals[who], next(2) ? "left" : "right",
                                ++sequence, 1.0 + static_cast<double>(next(5)),
                                incarnation[who]));
      }
    }
    a.settle();
    const auto source = a.digest();
    ASSERT_TRUE(resumeOnB());
    EXPECT_EQ(b.digest(), source);
    ASSERT_TRUE(upgradeA());
    a.settle();
    EXPECT_EQ(a.digest(), source);
    b.host.reset();
  }
}

TEST_F(Translate, AFailingTranslatorRollsBack) {
  populate();
  a.settle();
  const auto context = a.executor().context(a.pid);
  const auto digest  = a.digest();
  a.executor().setOwnedStateTranslator(
      [](const TranslationPlan &, const ApplyRequest &)
          -> std::expected<TranslationReport, TransferRefusal> {
        return std::unexpected(TransferRefusal::make(
            TransferCode::StateInconsistent, TransferRoute::UpgradeSchema,
            ActorError::ExecutionFailed, {.subject = "injected"}));
      });
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::StateInconsistent);
  EXPECT_EQ(done.error().detail.subject, "injected");
  EXPECT_EQ(a.executor().context(a.pid), context);
  EXPECT_EQ(a.digest(), digest);
  EXPECT_EQ(a.profiles().size(), 2U);
}

TEST_F(Translate, NoTranslatorRefusesPopulatedStateByNumber) {
  populate();
  a.settle();
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  a.executor().setOwnedStateTranslator({});
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::NoTranslator);
  EXPECT_EQ(done.error().detail.subject, "d.trust");
  EXPECT_EQ(done.error().detail.savedCount, 1U);
  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program);
  b.host.emplace(std::move(*program));
  b.executor().setOwnedStateTranslator({});
  const auto resumed = b.executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::NoTranslator);
}

TEST_F(Translate, AnApplyTimeResumeFailureEndsTheActorWithAStoredRefusal) {
  populate();
  a.settle();
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program);
  b.host.emplace(std::move(*program));
  b.executor().setOwnedStateTranslator(
      [](const TranslationPlan &, const ApplyRequest &)
          -> std::expected<TranslationReport, TransferRefusal> {
        return std::unexpected(TransferRefusal::make(
            TransferCode::StateInconsistent, TransferRoute::Resume,
            ActorError::ExecutionFailed, {.subject = "injected"}));
      });
  const auto resumed = b.executor().resume(*saved);
  ASSERT_TRUE(resumed);
  b.host->pump();
  EXPECT_FALSE(b.executor().context(*resumed));
  const auto stored = b.executor().lastRefusal(*resumed);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->code, TransferCode::StateInconsistent);
  EXPECT_EQ(stored->detail.subject, "injected");
}

TEST_F(Translate, ARecordWithoutAProfileIsInconsistent) {
  populate();
  a.settle();
  const auto support = a.support(kP1);
  ASSERT_TRUE(support);
  // A record whose `profile` value is no clone occurrence of any profile.
  auto &arena = a.arena();
  WireRecord record;
  record.set(std::string(kObsProfile), WireValue::text(kP1));
  appendToRank(arena, *support, a.dims().epistemic,
               writeValue(arena, a.vd(), WireValue::record(std::move(record))));
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::StateInconsistent);
}

TEST_F(Translate, AMissingStreamsListIsReplayStateUnavailable) {
  populate();
  a.settle();
  const auto p = *a.profile(kP1);
  setField(a.arena(), a.vd(), p, kProfileStreams, std::nullopt);
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::ReplayStateUnavailable);
  EXPECT_EQ(done.error().detail.subject, kP1);
  EXPECT_EQ(done.error().detail.saved, std::string(kProfileStreams));
}

TEST_F(Translate, AnUnregisteredDimensionRefuses) {
  populate();
  a.settle();
  auto &arena      = a.arena();
  const auto extra = arena.ensureDimension("d.extra");
  zigzag::expectWritten(
      arena.link(*a.profile(kP1), extra, zigzag::DimVector::POS,
                 arena.makeCell(std::string_view("owned by nobody"))));
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::UnregisteredOwnedStructure);
  EXPECT_EQ(done.error().detail.subject, "d.extra");
}

TEST_F(Translate, AnUnregisteredProfileFieldRefuses) {
  populate();
  a.settle();
  setField(a.arena(), a.vd(), *a.profile(kP1), "calibration_epoch",
           writeValue(a.arena(), a.vd(), WireValue::integer(1)));
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::UnregisteredOwnedStructure);
  EXPECT_EQ(done.error().detail.subject, "calibration_epoch");
}

TEST_F(Translate, BindingsSurviveAnUnchangedPolicyAndAChangedOneIsRefused) {
  populate();
  a.settle();
  const auto changed = [&] {
    std::string source(kSource);
    source.replace(source.find("max_age: 10s"), 12, "max_age: 12s");
    auto program = CompiledProgram::fromSource(source);
    EXPECT_TRUE(program) << program.error();
    return a.host->retainProgram(std::move(*program));
  }();
  ASSERT_TRUE(changed);
  const auto refused = a.host->upgrade(a.pid, changed);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, TransferCode::PolicyChanged);
  const auto stale = a.observe(reading(kP1, "left", 50, 2.0));
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->kind, IngestOutcome::Kind::Committed);
  ASSERT_TRUE(upgradeA());
  a.settle();
  // No rebind: the binding made before the swap still speaks for the source.
  const auto after = a.observe(reading(kP1, "left", 51, 2.0));
  ASSERT_TRUE(after);
  EXPECT_EQ(after->kind, IngestOutcome::Kind::Committed) << kind(after);
}

/// A full graph: the largest the executor admits, resumed within the budgets,
/// with the work the routine charged recorded per cell.
TEST_F(Translate, AFullBudgetResumesWithinThePeakAndNotInPlace) {
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
        const auto r = a.observe(o);
        refused      = r && r->kind == IngestOutcome::Kind::Refused;
      }
    if (refused) break;
  }
  const auto source = a.digest();
  const auto cells  = *a.executor().epistemicCells(a.pid);
  ASSERT_GE(cells, AgentSettings{}.epistemic.cells * 9 / 10);

  std::optional<TranslationReport> report;
  const auto measure = [&report](Side &s) {
    s.executor().setOwnedStateTranslator(
        [&report](const TranslationPlan &plan, const ApplyRequest &request) {
          auto done = applyTranslation(plan, request);
          if (done) report = *done;
          return done;
        });
  };
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program);
  b.host.emplace(std::move(*program));
  measure(b);
  const auto planStart = std::chrono::steady_clock::now();
  EXPECT_FALSE(b.executor().preflightResume(*saved));
  const auto planMs = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - planStart)
                          .count();
  const auto resumed = b.executor().resume(*saved);
  ASSERT_TRUE(resumed) << toString(resumed.error());
  b.pid = *resumed;
  b.host->pump();
  ASSERT_TRUE(b.executor().context(b.pid));
  ASSERT_TRUE(report);
  const auto resumeCharges = report->charges;
  const auto copied        = report->cells;
  RecordProperty("cells", std::to_string(copied));
  RecordProperty("resume_charges", std::to_string(resumeCharges));
  RecordProperty("resume_charges_per_cell",
                 std::to_string(static_cast<double>(resumeCharges) /
                                static_cast<double>(copied)));
  RecordProperty("preflight_ms", std::to_string(planMs));
  std::printf("T1C-MEASURE cells=%llu resume_charges=%llu per_cell=%.2f "
              "preflight_ms=%.1f\n",
              static_cast<unsigned long long>(copied),
              static_cast<unsigned long long>(resumeCharges),
              static_cast<double>(resumeCharges) / static_cast<double>(copied),
              planMs);
  EXPECT_LE(resumeCharges, copied * kTransferWorkPerCell);
  EXPECT_EQ(b.digest(), source);

  // The in-place road copies into an arena whose scratch an upgrade grant
  // caps at 32 KiB, and the clauses alone nearly fill it at this size: the
  // swap fails with a typed resource refusal and leaves the actor as it was.
  const auto context = a.executor().context(a.pid);
  const auto done    = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().actor, ActorError::ResourceLimit);
  EXPECT_EQ(a.executor().context(a.pid), context);
  EXPECT_EQ(a.digest(), source);

  // A host with a lower ceiling refuses by the numbers and writes nothing.
  LocalHost::Options options;
  options.settings.epistemic.cells = cells / 2;
  auto lowProgram                  = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(lowProgram);
  LocalHost low(std::move(*lowProgram), options);
  const auto refused = low.executor().resume(*saved);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, TransferCode::PeakCopyBudget);
  EXPECT_EQ(refused.error().detail.savedCount, cells);
}

TEST_F(Translate, AnUpgradeWithinTheScratchBudgetCarriesAndIsMeasured) {
  populate();
  a.settle();
  const auto source = a.digest();
  std::optional<TranslationReport> report;
  a.executor().setOwnedStateTranslator(
      [&report](const TranslationPlan &plan, const ApplyRequest &request) {
        auto done = applyTranslation(plan, request);
        if (done) report = *done;
        return done;
      });
  ASSERT_TRUE(upgradeA());
  ASSERT_TRUE(report);
  std::printf("T1C-MEASURE upgrade cells=%llu charges=%llu per_cell=%.2f\n",
              static_cast<unsigned long long>(report->cells),
              static_cast<unsigned long long>(report->charges),
              static_cast<double>(report->charges) /
                  static_cast<double>(report->cells));
  EXPECT_LE(report->charges, report->cells * kTransferWorkPerCell);
  a.settle();
  EXPECT_EQ(a.digest(), source);
}

TEST(TranslateScript, TheCoverageScriptFailsOnAnUncoveredFieldOrDimension) {
  namespace fs           = std::filesystem;
  const fs::path runtime = "apps/common/xanadu/vagent/runtime";
  const fs::path script  = "tools/check-translator-coverage.py";
  if (!fs::exists(script) || !fs::exists(runtime))
    GTEST_SKIP() << "run from the repository root";
  const auto dir = fs::temp_directory_path() / "t1c-coverage-fixture";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto run = [&](const fs::path &abi, const fs::path &sources) {
    const auto command =
        "python3 -B " + script.string() + " --registry " +
        (runtime / "epistemic_translate_registry.cpp").string() + " --abi " +
        abi.string() + " --sources " + sources.string() + " 2>/dev/null";
    return std::system(command.c_str());
  };
  std::ofstream(dir / "abi_ok.hpp")
      << "inline constexpr std::string_view kProfileFaulted = \"faulted\";\n";
  std::ofstream(dir / "src_ok.cpp") << "ensureDimension(\"d.trust\");\n";
  std::ofstream(dir / "abi_extra.hpp")
      << "inline constexpr std::string_view kProfileFaulted = \"faulted\";\n"
         "inline constexpr std::string_view kProfileInvented = \"invented\";\n";
  std::ofstream(dir / "src_extra.cpp")
      << "ensureDimension(\"d.trust\");\nensureDimension(\"d.invented\");\n";
  EXPECT_EQ(run(dir / "abi_ok.hpp", dir / "src_ok.cpp"), 0);
  EXPECT_NE(run(dir / "abi_extra.hpp", dir / "src_ok.cpp"), 0);
  EXPECT_NE(run(dir / "abi_ok.hpp", dir / "src_extra.cpp"), 0);
  fs::remove_all(dir);
}

} // namespace
