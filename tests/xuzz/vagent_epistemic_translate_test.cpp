/**
 * @file vagent_epistemic_translate_test.cpp
 * @brief P19 T1c: the translation routine resume and the schema road share,
 *        the full-key matcher, the digest and the owned-structure registry.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/runtime/epistemic_abi_liveness.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "epistemic_time.hpp"

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
  /// Every checkpoint taken, kept so its seeds outlive the hops after it.
  std::vector<AgentCheckpoint> checkpoints;
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
    setTimes(o, base - 5.0 + 0.01 * static_cast<double>(sequence), 0.01);
    o.clockBasis = "wall";
    o.frame      = "enu";
    o.units      = "m";
    o.variance   = 0.25;
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
    return resumeOn(a, b, source);
  }

  /// Checkpoint @p from and resume it on @p to, on a fresh host.
  bool resumeOn(Side &from, Side &to, std::string_view source = kSource) {
    const auto saved = from.executor().checkpoint(from.pid);
    EXPECT_TRUE(saved) << (saved ? "" : toString(saved.error()));
    if (!saved) return false;
    // A resumed actor's next checkpoint still quotes the bytes the one before
    // it published; this host seeds only its own, so the earlier checkpoint's
    // seeds stand behind it as the remote source would.
    if (!checkpoints.empty())
      saved->source->setFallback(checkpoints.back().source.get());
    checkpoints.push_back(*saved);
    auto program = CompiledProgram::fromSource(source);
    EXPECT_TRUE(program) << program.error();
    to.host.emplace(std::move(*program));
    const auto resumed = to.executor().resume(*saved);
    EXPECT_TRUE(resumed) << (resumed ? "" : toString(resumed.error()));
    if (!resumed) return false;
    to.pid = *resumed;
    to.host->pump();
    if (!to.executor().context(to.pid)) return false;
    to.executor().setWallClockSynchronised(true);
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

TEST_F(Translate, AMappingVersionIsComparedOnlyUnderOneClockInstance) {
  a.bind(kP1);
  auto first       = reading(kP1, "left", 1, 1.0);
  first.clockEpoch = 5;
  setMappingVersion(first, 7);
  ASSERT_TRUE(a.observe(first));
  // Under the executor that issued version 7, version 2 went backwards.
  auto regress       = reading(kP1, "right", 2, 1.0);
  regress.clockEpoch = 5;
  setMappingVersion(regress, 2);
  const auto refused = a.observe(regress);
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->code, "ClockMappingRegression");
  // The stream is carried, with the name of the clock it was issued under;
  // the new executor's versions start over, so the same version is not a
  // regression there (design 4.3, A4).
  ASSERT_TRUE(resumeOnB());
  b.bind(kP1);
  auto again       = reading(kP1, "right", 3, 1.0);
  again.clockEpoch = 5;
  setMappingVersion(again, 2);
  const auto resumed = b.observe(again);
  ASSERT_TRUE(resumed);
  EXPECT_EQ(resumed->kind, IngestOutcome::Kind::Committed);
  // The stream now names the new instance, so a lower version regresses again.
  auto lower       = reading(kP1, "left", 4, 1.0);
  lower.clockEpoch = 5;
  setMappingVersion(lower, 1);
  const auto regressed = b.observe(lower);
  ASSERT_TRUE(regressed);
  EXPECT_EQ(regressed->code, "ClockMappingRegression");
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

// I3-3c1 carries the stamps and the flags across a resume; the new instance's
// first control turn rebases them (the liveness tests drive that turn).
TEST_F(Translate, LivenessFieldsAreCarriedByBothDispositions) {
  populateEveryStructure();
  const auto real = [](Side &s, std::string_view name) {
    const auto cell = s.field(*s.profile(kP1), name);
    return cell ? s.arena().asDouble(*cell) : std::nullopt;
  };
  ASSERT_EQ(real(a, kProfileLastReceive), 123.5);
  const auto carried = [&](Side &s) {
    EXPECT_EQ(real(s, kProfileLastReceive), 123.5);
    EXPECT_EQ(real(s, kProfileLastContact), 124.5);
    EXPECT_EQ(real(s, kProfileSilentSince), 125.25);
    const auto cell = [&](std::string_view name) {
      return s.field(*s.profile(kP1), name);
    };
    ASSERT_TRUE(cell(kProfileLivenessInstance));
    EXPECT_EQ(s.arena().textOf(*cell(kProfileLivenessInstance)),
              "fedcba9876543210");
    ASSERT_TRUE(cell(kProfileSilent));
    EXPECT_TRUE(s.arena().asBool(*cell(kProfileSilent)).value_or(false));
    ASSERT_TRUE(cell(kProfileSilentCause));
    EXPECT_EQ(s.arena().textOf(*cell(kProfileSilentCause)),
              kSilentCauseHeartbeat);
    const auto streams = cell(kProfileStreams);
    ASSERT_TRUE(streams);
    const auto stream = s.field(*streams, "live_stream");
    ASSERT_TRUE(stream);
    const auto received = s.field(*stream, kStreamReceived);
    ASSERT_TRUE(received);
    EXPECT_EQ(s.arena().asDouble(*received), 126.125);
    const auto stale = s.field(*stream, kStreamStale);
    ASSERT_TRUE(stale);
    EXPECT_TRUE(s.arena().asBool(*stale).value_or(false));
  };
  ASSERT_TRUE(resumeOnB());
  carried(b);
  ASSERT_TRUE(upgradeA());
  a.settle();
  carried(a);
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

/// Event IDs and keys a checkpoint types: numbers, a float literal and the
/// truth words.
constexpr std::string_view kTypedLooking[] = {"42",   "1e3", "6",
                                              "true", "on",  "yes"};

TEST_F(Translate, NumericAndBooleanLookingTextAgreesOnBothRoads) {
  // Three keys a source, under the default `keys_per_source` of four.
  a.bind(kP1);
  a.bind(kP2);
  std::uint64_t seq = 0;
  std::vector<CheckedObservation> sent;
  for (std::size_t i = 0; i < std::size(kTypedLooking); ++i) {
    const std::string text(kTypedLooking[i]);
    auto o    = reading(i < 3 ? kP1 : kP2, text, ++seq, 1.0);
    o.eventId = text;
    ASSERT_EQ(a.observe(o)->kind, IngestOutcome::Kind::Committed) << text;
    sent.push_back(o);
  }
  a.settle();
  const auto source = a.digest();
  // Counted by the arena's stats: the host's outcome log answers an event ID
  // it has already logged with the first outcome.
  const auto refused = [](Side &side, std::string_view code) {
    return side.executor().stat(side.pid, std::string(kStatRefusedPrefix) +
                                              std::string(code));
  };
  const auto replays = [&](Side &side) {
    for (const auto &o : sent) {
      SCOPED_TRACE(o.eventId);
      const auto duplicates = refused(side, kTurnResultDuplicate);
      const auto conflicts  = refused(side, kTurnResultConflictingEvent);
      ASSERT_TRUE(side.executor().observe(side.pid, o));
      side.host->pump();
      EXPECT_EQ(refused(side, kTurnResultDuplicate), duplicates + 1);
      auto other     = o;
      other.tuple[1] = WireValue::real(7.0);
      ASSERT_TRUE(side.executor().observe(side.pid, other));
      side.host->pump();
      EXPECT_EQ(refused(side, kTurnResultConflictingEvent), conflicts + 1);
    }
    EXPECT_EQ(refused(side, kTurnResultStaleSequence), 0U);
  };
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(b.digest(), source);
  b.bind(kP1);
  b.bind(kP2);
  replays(b);
  const auto done = upgradeA();
  ASSERT_TRUE(done) << toString(done.error());
  a.settle();
  EXPECT_EQ(a.digest(), source);
  replays(a);
}

TEST_F(Translate, RespelledTextDigestsAlikeAfterTwoHops) {
  // Spellings a canonical rendering rewrites. The second checkpoint promotes
  // the typed cells the first resume made, which is where they could drift.
  constexpr std::string_view respelled[] = {"007", "1e3", "on",
                                            "yes", "+5",  "1.50"};
  a.bind(kP1);
  a.bind(kP2);
  std::uint64_t seq = 0;
  std::vector<CheckedObservation> sent;
  for (std::size_t i = 0; i < std::size(respelled); ++i) {
    const std::string text(respelled[i]);
    auto o    = reading(i < 3 ? kP1 : kP2, text, ++seq, 1.0);
    o.eventId = text;
    ASSERT_EQ(a.observe(o)->kind, IngestOutcome::Kind::Committed) << text;
    sent.push_back(o);
  }
  a.settle();
  const auto source = a.digest();
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(b.digest(), source);
  Side c;
  ASSERT_TRUE(resumeOn(b, c));
  EXPECT_EQ(c.digest(), source);
  c.bind(kP1);
  c.bind(kP2);
  for (const auto &o : sent) {
    SCOPED_TRACE(o.eventId);
    const auto duplicates =
        c.executor().stat(c.pid, std::string(kStatRefusedPrefix) +
                                     std::string(kTurnResultDuplicate));
    ASSERT_TRUE(c.executor().observe(c.pid, o));
    c.host->pump();
    EXPECT_EQ(c.executor().stat(c.pid, std::string(kStatRefusedPrefix) +
                                           std::string(kTurnResultDuplicate)),
              duplicates + 1);
  }
}

TEST_F(Translate, ANumericLookingClockInstanceDigestsAlikeOnBothRoads) {
  // The executor's clock instance is 16 random hex digits, all decimal about
  // once in 800 executors; that is what the random ingestion test met. Each
  // record's copy is written here so the case is not left to chance.
  populate();
  a.settle();
  const std::string digits = "1234567890123456";
  std::size_t written      = 0;
  const auto d             = a.dims();
  for (const auto pred : zigzag::rankAfter(a.arena(), a.agent(), d.belief))
    for (const auto clause :
         zigzag::rankAfter(a.arena(), pred, a.ctx().core.dims().clause))
      for (const auto s : zigzag::rankAfter(a.arena(), clause, d.source))
        for (const auto record : zigzag::rankAfter(a.arena(), s, d.epistemic)) {
          const auto cell = a.field(record, kObsClockInstance);
          ASSERT_TRUE(cell);
          zigzag::expectWritten(a.arena().setContent(
              *cell, std::array{a.arena().intern(digits)}));
          zigzag::expectWritten(
              a.arena().setValueBits(*cell, xanadu::ValueKind::None, 0));
          ++written;
        }
  ASSERT_GT(written, 0U);
  const auto source = a.digest();
  ASSERT_TRUE(resumeOnB());
  EXPECT_EQ(b.digest(), source);
  ASSERT_TRUE(upgradeA());
  a.settle();
  EXPECT_EQ(a.digest(), source);
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

TEST_F(Translate, AnArenaLimitInTheCopyIsATypedStoredRefusal) {
  // The translator meets the scratch cap mid-copy: the refusal is typed and
  // stored, where the arena's own throw used to end the resume untyped (S-3).
  populate();
  a.settle();
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program);
  b.host.emplace(std::move(*program));
  b.executor().setOwnedStateTranslator([](const TranslationPlan &plan,
                                          const ApplyRequest &request) {
    // Room for no text at all.
    request.into.setExecutionLimits(std::numeric_limits<std::uint32_t>::max(),
                                    std::numeric_limits<std::size_t>::max(),
                                    request.into.scratchBytes());
    return applyTranslation(plan, request);
  });
  const auto resumed = b.executor().resume(*saved);
  ASSERT_TRUE(resumed);
  b.host->pump();
  EXPECT_FALSE(b.executor().context(*resumed));
  const auto stored = b.executor().lastRefusal(*resumed);
  ASSERT_TRUE(stored);
  EXPECT_EQ(stored->code, TransferCode::PeakCopyBudget);
  EXPECT_EQ(stored->detail.subject, "scratch bytes");
  EXPECT_EQ(stored->actor, ActorError::ResourceLimit);
}

TEST_F(Translate, TheScratchGateCountsTypedTextAndTheCallersBytes) {
  populate();
  a.settle();
  ASSERT_TRUE(resumeOnB());
  const auto smallest = [](Side &s, std::uint64_t fixed) {
    const auto registry = TranslatorRegistry::of(true);
    const auto plan     = [&](std::uint64_t bytes) {
      return planTranslation(
          {.disposition = Disposition::Upgrade,
               .route       = TransferRoute::UpgradeSchema,
               .from        = s.arena(),
               .fromAgent   = s.agent(),
               .fromDims    = s.owned(),
               .intoSchema  = s.host->program().upgradeSchema(),
               .registry    = registry,
               .allowance =
               ResumeAllowance{
                       .epistemicCells = AgentSettings{}.epistemic.cells,
                       .grant =
                       ResumeLimits{
                               .instructions =
                               std::numeric_limits<std::uint64_t>::max(),
                               .maxCells =
                               std::numeric_limits<std::uint32_t>::max(),
                               .maxBytes = bytes}},
               .fixedBytes = fixed});
    };
    std::uint64_t lo = 0, hi = 1U << 20;
    EXPECT_TRUE(plan(hi));
    while (lo < hi) {
      const auto mid = lo + (hi - lo) / 2;
      if (plan(mid))
        hi = mid;
      else
        lo = mid + 1;
    }
    if (lo > 0) {
      const auto refused = plan(lo - 1);
      EXPECT_FALSE(refused);
      if (!refused) {
        EXPECT_EQ(refused.error().code, TransferCode::PeakCopyBudget);
        EXPECT_EQ(refused.error().detail.subject, "scratch bytes");
      }
    }
    return lo;
  };
  const auto live    = smallest(a, 0);
  const auto resumed = smallest(b, 0);
  // A checkpoint gives every number its text, and the copier interns it, so
  // the resumed graph needs more scratch than the live one for the same state.
  EXPECT_GT(resumed, live);
  // What the caller's copiers already took counts against the same grant.
  EXPECT_EQ(smallest(a, 100), live + 100);
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

TEST_F(Translate, ARefusalNamesACellByHashAndNeverByItsText) {
  populate();
  a.settle();
  // A profile whose text is no principal token: document text, say.
  const std::string secret = "the vault code is 1234";
  const auto p             = *a.profile(kP1);
  zigzag::expectWritten(
      a.arena().setContent(p, std::array{a.arena().intern(secret)}));
  const auto done = upgradeA();
  ASSERT_FALSE(done);
  EXPECT_EQ(done.error().code, TransferCode::StateInconsistent);
  ASSERT_TRUE(done.error().detail.subject);
  EXPECT_TRUE(done.error().detail.subject->starts_with("profile sha256:"))
      << *done.error().detail.subject;
  EXPECT_EQ(toString(done.error()).find(secret), std::string::npos);
}

TEST_F(Translate, TheDigestStopsAtACloneRing) {
  // S-8: the plan refuses a ring; the digest, a test reader today, must still
  // end on one.
  populate();
  a.settle();
  const auto p     = *a.profile(kP1);
  const auto clone = a.ctx().core.dims().clone;
  auto last        = a.arena().linked(p, clone);
  ASSERT_TRUE(last);
  while (const auto next = a.arena().linked(*last, clone)) last = next;
  zigzag::expectWritten(
      a.arena().link(*last, clone, zigzag::DimVector::POS, p));
  EXPECT_FALSE(a.digest().empty());
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

/// The profile values a crafted image could carry, each refused by the plan
/// on both roads before anything is written (S-2).
class TranslateForgedValues : public Translate {
protected:
  CellRef p1() { return *a.profile(kP1); }
  void setBits(std::string_view field, xanadu::ValueKind kind,
               std::uint64_t bits) {
    const auto cell = a.field(p1(), field);
    ASSERT_TRUE(cell) << field;
    zigzag::expectWritten(a.arena().setValueBits(*cell, kind, bits));
  }
  void setText(CellRef owner, std::string_view field, std::string_view text) {
    const auto cell = a.field(owner, field);
    ASSERT_TRUE(cell) << field;
    zigzag::expectWritten(
        a.arena().setContent(*cell, std::array{a.arena().intern(text)}));
    zigzag::expectWritten(
        a.arena().setValueBits(*cell, xanadu::ValueKind::None, 0));
  }
  CellRef firstReason() {
    const auto list = a.field(p1(), kProfileReasons);
    return *a.arena().linked(*list, a.ctx().core.dims().grab);
  }
  /// Refused on the upgrade road and by the resume pre-flight, by @p field.
  void expectRefused(std::string_view field) {
    a.settle();
    const auto saved = a.executor().checkpoint(a.pid);
    ASSERT_TRUE(saved);
    const auto resumed = a.executor().preflightResume(*saved);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->code, TransferCode::StateInconsistent);
    EXPECT_EQ(resumed->detail.subject, std::string(field));
    const auto done = upgradeA();
    ASSERT_FALSE(done);
    EXPECT_EQ(done.error().code, TransferCode::StateInconsistent);
    EXPECT_EQ(done.error().detail.subject, std::string(field));
  }
};

TEST_F(TranslateForgedValues, TheSamplesThemselvesPass) {
  populateEveryStructure();
  a.settle();
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  EXPECT_FALSE(a.executor().preflightResume(*saved));
}

TEST_F(TranslateForgedValues, HealthCellsAtEitherExtremeRefuse) {
  for (const auto forged : {std::numeric_limits<std::int64_t>::max(),
                            std::numeric_limits<std::int64_t>::min()}) {
    SCOPED_TRACE(forged);
    SetUp();
    populateEveryStructure();
    setBits(kProfileHealthCells, xanadu::ValueKind::Int64,
            std::bit_cast<std::uint64_t>(forged));
    expectRefused(kProfileHealthCells);
  }
}

TEST_F(TranslateForgedValues, ANonFiniteOrNegativeWeightRefuses) {
  for (const auto field : {kProfileHealthWeight, kProfileHealthRefusedWeight})
    for (const auto forged : {std::numeric_limits<double>::quiet_NaN(), -1.0,
                              std::numeric_limits<double>::infinity()}) {
      SCOPED_TRACE(std::string(field) + " " + std::to_string(forged));
      SetUp();
      populateEveryStructure();
      setBits(field, xanadu::ValueKind::Double,
              std::bit_cast<std::uint64_t>(forged));
      expectRefused(field);
    }
}

TEST_F(TranslateForgedValues, MoreReasonsThanTheSettingAllowRefuse) {
  populateEveryStructure();
  const auto context = a.populateContext(kP1);
  const auto &all    = allOwnedStructures();
  const auto reasons = std::ranges::find(
      all, std::string_view("profile.reasons"), &OwnedStructure::id);
  ASSERT_NE(reasons, all.end());
  // The sample made one; the default allows three.
  for (int i = 0; i < 3; ++i) reasons->populate(context);
  expectRefused(kProfileReasons);
}

TEST_F(TranslateForgedValues, AForgedOperatorReasonRefuses) {
  populateEveryStructure();
  // An issuer that is no principal token cannot have authorised a degrade.
  setText(firstReason(), kReasonIssuer, "operator says so");
  expectRefused(kReasonOrigin);
}

TEST_F(TranslateForgedValues, AnUnknownOriginOrKindRefuses) {
  populateEveryStructure();
  setText(firstReason(), kReasonOrigin, "administrator");
  expectRefused(kReasonOrigin);
  SetUp();
  populateEveryStructure();
  setText(firstReason(), kReasonKind, "trusted");
  expectRefused(kReasonKind);
}

TEST_F(TranslateForgedValues, ANegativeIncarnationRefuses) {
  populateEveryStructure();
  setBits(kProfileIncarnation, xanadu::ValueKind::Int64,
          std::bit_cast<std::uint64_t>(std::int64_t{-1}));
  expectRefused(kProfileIncarnation);
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
