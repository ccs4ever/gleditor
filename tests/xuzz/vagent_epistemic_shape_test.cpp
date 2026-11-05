/**
 * @file vagent_epistemic_shape_test.cpp
 * @brief P19 T1b1: the shape header, the master's encoding and signature, the
 *        one guard that decides whether state is translatable, and the
 *        resume refusals that read them.
 */
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "common/xanadu/vagent/runtime/epistemic_policy.hpp"
#include "common/xanadu/vagent/runtime/epistemic_refusal.hpp"
#include "common/xanadu/vagent/runtime/epistemic_shape.hpp"
#include "common/xanadu/vagent/runtime/parser.hpp"
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
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

constexpr std::string_view kPlain = R"vagent(
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
}
)vagent";

const std::string kP1(64, '1');

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

std::string withoutKey(std::string_view source) {
  std::string out(source);
  out.replace(out.find("key who"), 7, "who");
  return out;
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

CheckedObservation reading(std::string eventId, std::uint64_t sequence,
                           double base) {
  CheckedObservation o;
  o.principal           = kP1;
  o.incarnation         = 1;
  o.eventId             = std::move(eventId);
  o.sequence            = sequence;
  o.predicate           = "at";
  o.tuple               = {WireValue::text("self"), WireValue::real(1.0)};
  o.acquiredAt          = base - 5.0 + 0.01 * static_cast<double>(sequence);
  o.acquiredUncertainty = 0.01;
  o.clockBasis          = "wall";
  o.frame               = "enu";
  o.units               = "m";
  o.variance            = 0.25;
  return o;
}

OwnedGraphDims ownedDims(zigzag::vortex::WorkerContext &ctx) {
  const auto agent = registerDimensions(ctx.arena);
  const auto &core = ctx.core.dims();
  return {.vars      = core.vars,
          .values    = core.values,
          .grab      = core.grab,
          .step      = core.step,
          .clone     = core.clone,
          .clause    = core.clause,
          .source    = agent.source,
          .belief    = agent.belief,
          .trust     = agent.trust,
          .epistemic = agent.epistemic};
}

/// A host with one running agent, optionally with sources and readings.
struct Live {
  std::optional<LocalHost> host;
  ActorPid pid;

  explicit Live(std::string_view source, std::string ref = "ref-1",
                bool readings = false) {
    host.emplace(std::move(*unitProgram(source, std::move(ref))));
    const auto launched = host->launch("A", "a");
    EXPECT_TRUE(launched);
    pid = *launched;
    host->pump();
    if (!readings) return;
    host->executor().setWallClockSynchronised(true);
    EXPECT_TRUE(host->executor().bindSource(
        pid, {"sensor:gps", kP1, 1, "at", std::chrono::hours(1)}));
    host->pump();
    const auto base = wallSeconds();
    for (std::uint64_t i = 1; i <= 3; ++i) {
      EXPECT_TRUE(
          host->executor().observe(pid, reading("e" + std::to_string(i), i, base)));
      host->pump();
    }
  }

  zigzag::vortex::WorkerContext &ctx() { return *host->executor().context(pid); }
  CellRef agent() { return *host->executor().agentCell(pid); }

  /// The cells of the agent's `d.epistemic` rank.
  std::vector<CellRef> epistemicRank() {
    return zigzag::rankAfter(ctx().arena, agent(),
                             registerDimensions(ctx().arena).epistemic) |
           std::ranges::to<std::vector<CellRef>>();
  }
  std::optional<CellRef> header() {
    const auto rank = epistemicRank();
    return rank.empty() ? std::nullopt : std::optional{rank.front()};
  }
  EpistemicStateProbe probe() {
    return probeEpistemicState(ctx().arena, agent(), ownedDims(ctx()));
  }
  void setText(CellRef cell, std::string_view text) {
    const std::array spans{ctx().arena.intern(text)};
    ASSERT_TRUE(ctx().arena.setContent(cell, spans));
  }
  void setHeaderVersion(std::int64_t version) {
    const auto value = fieldValue(ctx().arena, valueDims(ctx().core), *header(),
                                  kShapeVersionField);
    ASSERT_TRUE(value);
    ASSERT_TRUE(ctx().arena.setValueBits(
        *value, xanadu::ValueKind::Int64, static_cast<std::uint64_t>(version)));
  }
  /// The predicate's policy master.
  CellRef master(std::string_view predicate) {
    auto &arena = ctx().arena;
    const auto dims = registerDimensions(arena);
    for (const auto p : zigzag::rankAfter(arena, agent(), dims.belief))
      if (arena.textOf(p) == predicate) return *arena.linked(p, dims.epistemic);
    ADD_FAILURE() << "no predicate " << predicate;
    return 0;
  }
  void injectIntention() {
    appendToRank(ctx().arena, agent(), registerDimensions(ctx().arena).intention,
                 ctx().arena.makeCell("intention"));
  }
  AgentCheckpoint checkpoint() {
    const auto saved = host->executor().checkpoint(pid);
    EXPECT_TRUE(saved) << (saved ? "" : "checkpoint refused");
    return *saved;
  }
  static void registerTranslator(LocalHost &h) {
    h.executor().setOwnedStateTranslator(
        [](const CompiledProgram &, const CompiledProgram &,
           const zigzag::ArenaManifold &, zigzag::CellRef,
           zigzag::vortex::WorkerContext &, zigzag::CellRef,
           std::map<zigzag::CellRef, zigzag::CellRef> &,
           std::uint64_t &) { return std::expected<void, ActorError>{}; });
  }
};

std::size_t versions = 0;
CompiledProgram nextVersion(std::string_view source) {
  ++versions;
  auto program =
      CompiledProgram::fromSource(std::string(source) + std::string(versions, '\n'));
  EXPECT_TRUE(program) << program.error();
  return std::move(*program);
}

// -- the header -------------------------------------------------------------

TEST(EpistemicShapeTest, TheHeaderIsMintedOnlyForAnArbitratingProgram) {
  Live arbitrating(kSource);
  const auto rank = arbitrating.epistemicRank();
  ASSERT_EQ(rank.size(), 1U);
  auto &arena = arbitrating.ctx().arena;
  EXPECT_EQ(arena.textOf(rank.front()), kShapeHeaderName);
  const auto version = fieldValue(arena, valueDims(arbitrating.ctx().core),
                                  rank.front(), kShapeVersionField);
  ASSERT_TRUE(version);
  EXPECT_EQ(arena.asInt64(*version), kEpistemicShapeVersion);
  EXPECT_EQ(kEpistemicShapeVersion, 1);

  Live plain(kPlain);
  EXPECT_TRUE(plain.epistemicRank().empty());
  EXPECT_FALSE(plain.probe().shape);
  EXPECT_FALSE(plain.probe().populated);

  // The program says the same without instantiating anything.
  const auto arb = CompiledProgram::fromSource(kSource);
  const auto non = CompiledProgram::fromSource(kPlain);
  ASSERT_TRUE(arb && non);
  EXPECT_EQ(programShape(arb->upgradeSchema()), kEpistemicShapeVersion);
  EXPECT_FALSE(programShape(non->upgradeSchema()));
}

TEST(EpistemicShapeTest, TheMasterCarriesItsEncodingAndSignature) {
  Live live(kSource);
  auto &arena     = live.ctx().arena;
  const auto vd   = valueDims(live.ctx().core);
  const auto m    = live.master("at");
  const auto enc  = fieldValue(arena, vd, m, "encoding");
  const auto sig  = fieldValue(arena, vd, m, "signature");
  const auto hash = fieldValue(arena, vd, m, "hash");
  ASSERT_TRUE(enc && sig && hash);
  EXPECT_EQ(arena.textOf(*enc), kPolicyEncoding);
  EXPECT_EQ(arena.textOf(*sig), "vagent-sig-1 at(key Str, Float)");
  const auto program = CompiledProgram::fromSource(kSource);
  ASSERT_TRUE(program);
  EXPECT_EQ(arena.textOf(*sig),
            canonicalPredicateSignature(
                "at", program->upgradeSchema().predicates.at("at")));
}

// -- the guard --------------------------------------------------------------

TEST(EpistemicShapeTest, TheGuardIgnoresTheHeaderAndReadsTheRest) {
  const auto none = TranslatorRegistry::of(false);
  const auto one  = TranslatorRegistry::of(true);
  const auto row  = [&](std::optional<std::int64_t> shape, bool populated,
                       const TranslatorRegistry &registry) {
    return hasTranslatableEpistemicState({shape, populated}, registry);
  };
  // No arbitrated predicate, and an arbitrating agent with no source yet:
  // translatable under any registry, since the header is not state.
  EXPECT_TRUE(row(std::nullopt, false, none));
  EXPECT_TRUE(row(1, false, none));
  // Profiles: translatable only when the registry has the saved shape.
  EXPECT_FALSE(row(1, true, none));
  EXPECT_TRUE(row(1, true, one));
  EXPECT_FALSE(row(2, true, one));
  EXPECT_FALSE(row(std::nullopt, true, one));

  Live plain(kPlain);
  EXPECT_TRUE(
      hasTranslatableEpistemicState(plain.probe(), TranslatorRegistry::of(false)));

  Live empty(kSource);
  EXPECT_EQ(empty.probe().shape, 1);
  EXPECT_FALSE(empty.probe().populated);
  EXPECT_TRUE(
      hasTranslatableEpistemicState(empty.probe(), TranslatorRegistry::of(false)));

  Live populated(kSource, "ref-1", true);
  EXPECT_EQ(populated.probe().shape, 1);
  EXPECT_TRUE(populated.probe().populated);
  EXPECT_FALSE(hasTranslatableEpistemicState(populated.probe(),
                                             TranslatorRegistry::of(false)));
  EXPECT_TRUE(hasTranslatableEpistemicState(populated.probe(),
                                            TranslatorRegistry::of(true)));
  // Something other than the header on `d.epistemic` is state too.
  appendToRank(empty.ctx().arena, empty.agent(),
               registerDimensions(empty.ctx().arena).epistemic,
               empty.ctx().arena.makeCell("x"));
  EXPECT_EQ(empty.probe().shape, 1);
  EXPECT_TRUE(empty.probe().populated);
}

TEST(EpistemicShapeTest, AnUnreadableHeaderWithStateRefusesByNumberOnEveryRoad) {
  Live live(kSource, "ref-1", true);
  Live::registerTranslator(*live.host);
  live.host->executor().drainNotifications();

  // Shape 2 against a program that mints shape 1.
  live.setHeaderVersion(2);
  EXPECT_EQ(live.probe().shape, 2);
  auto held = live.host->retainProgram(nextVersion(kSource));
  auto r    = live.host->upgrade(live.pid, held);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::UnknownShapeVersion);
  EXPECT_EQ(r.error().route, TransferRoute::UpgradeSchema);
  EXPECT_EQ(r.error().actor, ActorError::ExecutionFailed);
  EXPECT_EQ(r.error().detail.subject, "d.epistemic");
  EXPECT_EQ(r.error().detail.savedCount, 2U);
  EXPECT_EQ(r.error().detail.currentCount, 1U);

  // The reconciler road reads the same number before it names its own road.
  r = live.host->upgrade(live.pid, nextVersion(kSource));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::UnknownShapeVersion);
  EXPECT_EQ(r.error().route, TransferRoute::UpgradeReconciler);

  // A header that is not one reads as absent: state written before shape 1.
  live.setHeaderVersion(1);
  live.setText(*live.header(), "NotAHeader");
  EXPECT_FALSE(live.probe().shape);
  EXPECT_TRUE(live.probe().populated);
  held = live.host->retainProgram(nextVersion(kSource));
  r    = live.host->upgrade(live.pid, held);
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().code, TransferCode::UnknownShapeVersion);
  EXPECT_FALSE(r.error().detail.savedCount);
  EXPECT_EQ(r.error().detail.currentCount, 1U);
}

TEST(EpistemicShapeTest, ResumeRefusesAnUnknownShapeBeforeAPid) {
  Live a(kSource, "ref-1", true);
  a.setHeaderVersion(7);
  const auto saved = a.checkpoint();

  LocalHost b(std::move(*unitProgram(kSource, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::UnknownShapeVersion);
  EXPECT_EQ(resumed.error().route, TransferRoute::Resume);
  EXPECT_EQ(resumed.error().detail.savedCount, 7U);
  EXPECT_EQ(resumed.error().detail.currentCount, 1U);
  EXPECT_EQ(b.runtime().actorCount(), 0U);
  EXPECT_EQ(b.pump(), 0U);

  // State written before the header existed has no number to read.
  Live c(kSource, "ref-1", true);
  c.setText(*c.header(), "NotAHeader");
  const auto old = c.checkpoint();
  LocalHost d(std::move(*unitProgram(kSource, "ref-1")));
  const auto refused = d.executor().resume(old);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().code, TransferCode::UnknownShapeVersion);
  EXPECT_FALSE(refused.error().detail.savedCount);
  EXPECT_EQ(refused.error().detail.currentCount, 1U);
}

// -- no profile yet ---------------------------------------------------------

TEST(EpistemicShapeTest, AnArbitratingAgentWithNoProfileUpgradesAndResumes) {
  Live live(kSource);
  ASSERT_EQ(live.probe().shape, 1);
  const auto saved = live.checkpoint();
  // The header does not make the agent populated: no translator is needed on
  // either road, and a resume carries it.
  auto held = live.host->retainProgram(nextVersion(kSource));
  const auto shared = live.host->upgrade(live.pid, held);
  EXPECT_TRUE(shared) << (shared ? "" : toString(shared.error()));
  EXPECT_TRUE(live.host->upgrade(live.pid, nextVersion(kSource)));
  live.host->pump();

  LocalHost b(std::move(*unitProgram(kSource, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_TRUE(resumed) << toString(resumed.error());
}

TEST(EpistemicShapeTest, ANonArbitratingAgentUpgradesUnchangedAfterT1) {
  Live live(kPlain);
  EXPECT_TRUE(live.epistemicRank().empty());
  const auto shared =
      live.host->upgrade(live.pid, live.host->retainProgram(nextVersion(kPlain)));
  EXPECT_TRUE(shared) << (shared ? "" : toString(shared.error()));
  const auto byValue = live.host->upgrade(live.pid, nextVersion(kPlain));
  EXPECT_TRUE(byValue) << (byValue ? "" : toString(byValue.error()));
  EXPECT_TRUE(live.epistemicRank().empty());
}

// -- compared, never copied -------------------------------------------------

TEST(EpistemicShapeTest, TheSavedHeaderIsComparedAndNotCopied) {
  Live fresh(kSource);
  const auto programsOwn = *fresh.header();

  Live a(kSource, "ref-1", true);
  const auto saved = a.checkpoint();
  LocalHost b(std::move(*unitProgram(kSource, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_TRUE(resumed) << toString(resumed.error());
  b.pump();
  {
    auto &ctx   = *b.executor().context(*resumed);
    const auto agent = *b.executor().agentCell(*resumed);
    const auto rank  = zigzag::rankAfter(ctx.arena, agent,
                                         registerDimensions(ctx.arena).epistemic) |
                      std::ranges::to<std::vector<CellRef>>();
    ASSERT_EQ(rank.size(), 1U);
    // The program's own instantiation made it: the same cell a fresh launch
    // gets, not a clone of the saved one.
    EXPECT_EQ(rank.front(), programsOwn);
    EXPECT_EQ(ctx.arena.textOf(rank.front()), kShapeHeaderName);
  }

  // The in-place upgrade does the same.
  Live::registerTranslator(*a.host);
  a.host->executor().drainNotifications();
  const auto upgraded =
      a.host->upgrade(a.pid, a.host->retainProgram(nextVersion(kSource)));
  ASSERT_TRUE(upgraded) << toString(upgraded.error());
  a.host->pump();
  ASSERT_EQ(a.epistemicRank().size(), 1U);
  EXPECT_EQ(a.header(), programsOwn);
  EXPECT_EQ(a.probe().shape, 1);
}

// -- resume reads what the master now holds ---------------------------------

TEST(EpistemicShapeTest, ResumeOfAChangedPolicyTagNamesTheTag) {
  Live a(kSource, "ref-1", true);
  const auto m = a.master("at");
  const auto encoding =
      fieldValue(a.ctx().arena, valueDims(a.ctx().core), m, "encoding");
  ASSERT_TRUE(encoding);
  a.setText(*encoding, "vagent-policy-1");
  const auto saved = a.checkpoint();

  LocalHost b(std::move(*unitProgram(kSource, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_FALSE(resumed);
  const auto &refusal = resumed.error();
  EXPECT_EQ(refusal.code, TransferCode::PolicyEncodingChanged);
  EXPECT_EQ(refusal.route, TransferRoute::Resume);
  EXPECT_EQ(refusal.actor, ActorError::ExecutionFailed);
  EXPECT_EQ(refusal.detail.subject, "at");
  EXPECT_EQ(refusal.detail.saved, "vagent-policy-1");
  EXPECT_EQ(refusal.detail.current, kPolicyEncoding);
  EXPECT_EQ(b.runtime().actorCount(), 0U);
  EXPECT_EQ(b.pump(), 0U);
}

TEST(EpistemicShapeTest, ResumeOfAChangedSignatureIsPredicateIncompatible) {
  Live a(kSource, "ref-1", true);
  const auto saved = a.checkpoint();

  // The policy is the same; only the key position differs.
  LocalHost b(std::move(*unitProgram(withoutKey(kSource), "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_FALSE(resumed);
  const auto &refusal = resumed.error();
  EXPECT_EQ(refusal.code, TransferCode::PredicateIncompatible);
  EXPECT_EQ(refusal.route, TransferRoute::Resume);
  EXPECT_EQ(refusal.detail.subject, "at");
  EXPECT_EQ(refusal.detail.saved, "vagent-sig-1 at(key Str, Float)");
  EXPECT_EQ(refusal.detail.current, "vagent-sig-1 at(Str, Float)");
  EXPECT_EQ(b.runtime().actorCount(), 0U);
  EXPECT_EQ(b.pump(), 0U);
}

// -- intentions -------------------------------------------------------------

TEST(EpistemicShapeTest, AnActiveIntentionRefusesOnEveryRoadForAnArbitratingAgent) {
  Live live(kSource);
  live.injectIntention();

  const auto saved = live.checkpoint();
  LocalHost b(std::move(*unitProgram(kSource, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::ActiveIntention);
  EXPECT_EQ(resumed.error().route, TransferRoute::Resume);
  EXPECT_EQ(resumed.error().detail.savedCount, 1U);
  EXPECT_FALSE(resumed.error().detail.limit);
  EXPECT_EQ(b.runtime().actorCount(), 0U);

  const auto shared = live.host->upgrade(
      live.pid, live.host->retainProgram(nextVersion(kSource)));
  ASSERT_FALSE(shared);
  EXPECT_EQ(shared.error().code, TransferCode::ActiveIntention);
  EXPECT_EQ(shared.error().route, TransferRoute::UpgradeSchema);

  const auto byValue = live.host->upgrade(live.pid, nextVersion(kSource));
  ASSERT_FALSE(byValue);
  EXPECT_EQ(byValue.error().code, TransferCode::ActiveIntention);
  EXPECT_EQ(byValue.error().route, TransferRoute::UpgradeReconciler);
  EXPECT_EQ(byValue.error().detail.savedCount, 1U);
}

TEST(EpistemicShapeTest, ANonArbitratingAgentStillResumesAndReconcilesWithAnIntention) {
  Live live(kPlain);
  live.injectIntention();
  const auto saved = live.checkpoint();
  const auto byValue = live.host->upgrade(live.pid, nextVersion(kPlain));
  EXPECT_TRUE(byValue) << (byValue ? "" : toString(byValue.error()));

  LocalHost b(std::move(*unitProgram(kPlain, "ref-1")));
  const auto resumed = b.executor().resume(saved);
  EXPECT_TRUE(resumed) << (resumed ? "" : toString(resumed.error()));
}

} // namespace
