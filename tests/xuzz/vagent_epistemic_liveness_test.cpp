/**
 * @file vagent_epistemic_liveness_test.cpp
 * @brief P19 I3-3c1: heartbeats, silence, staleness and the timer wake
 *        (design 5.1 to 5.4), through the signed adapter.
 *
 * Time is simulated and its two parts move apart: `suspend` moves the wall
 * clock and not the steady one. A free device reads its own clock as the
 * host's steady time times (1 + 50 ppm) plus a bias. Every figure comes from
 * the emitted routines and the executor's timer, never from a stand-in.
 */
#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/epistemic_abi_liveness.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/health_grants.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "epistemic_invariants.hpp"
#include "epistemic_time.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;
namespace id = xanadu::identity;
using std::chrono::milliseconds;
using xanadu::identity::ActorPid;

constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate seen(key id: Str, n: Int)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "actor_mapped", max_age: 10s,
    max_uncertainty: 1s)
event Quit { }
agent A {
  on Quit { terminate }
}
)vagent";

constexpr std::int64_t kNanosPerMilli  = 1'000'000;
constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
constexpr double kSimDriftPpm          = 50.0;
constexpr std::int64_t kSimDeviceBias  = 7 * kNanosPerSecond;
/// The default `heartbeat_floor_ms`.
constexpr std::int64_t kFloorMs = kDefaultHeartbeatFloorMs;

/// The host's two clocks, moved only by the test and apart from each other.
struct SimClock {
  std::int64_t steady{};
  std::int64_t wall{};
  explicit SimClock(std::int64_t steadyBias = 0)
      : steady(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count() +
               steadyBias),
        wall(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) {}
  void advance(std::int64_t nanoseconds) {
    steady += nanoseconds;
    wall += nanoseconds;
  }
  void suspend(std::int64_t nanoseconds) { wall += nanoseconds; }
  [[nodiscard]] double offset() const {
    return static_cast<double>(wall - steady) / 1e9;
  }
  AgentClock agentClock() {
    return AgentClock{.wall =
                          [this] {
                            return std::chrono::system_clock::time_point(
                                std::chrono::system_clock::duration(wall));
                          },
                      .steady =
                          [this] {
                            return std::chrono::steady_clock::time_point(
                                std::chrono::steady_clock::duration(steady));
                          }};
  }
};

id::StandardCryptoEngine newKey() {
  auto engine = id::StandardCryptoEngine::generate();
  EXPECT_TRUE(engine.has_value());
  return std::move(*engine);
}

/// One simulated host with one agent, and readers of its arena.
struct Side {
  SimClock sim;
  std::unique_ptr<LocalHost> host;
  ActorPid pid;

  explicit Side(std::int64_t steadyBias = 0) : sim(steadyBias) {}

  void launch(AgentSettings settings = {}, bool synchronised = true) {
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.clock    = sim.agentClock();
    options.settings = std::move(settings);
    host = std::make_unique<LocalHost>(std::move(*program), options);
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    executor().setWallClockSynchronised(synchronised);
  }
  bool resume(const AgentCheckpoint &saved, AgentSettings settings = {}) {
    auto program = CompiledProgram::fromSource(kProgram);
    EXPECT_TRUE(program) << program.error();
    LocalHost::Options options;
    options.clock    = sim.agentClock();
    options.settings = std::move(settings);
    host = std::make_unique<LocalHost>(std::move(*program), options);
    const auto resumed = executor().resume(saved);
    EXPECT_TRUE(resumed) << (resumed ? "" : toString(resumed.error()));
    if (!resumed) return false;
    pid = *resumed;
    host->pump();
    executor().setWallClockSynchronised(true);
    return true;
  }
  AgentExecutor &executor() { return host->executor(); }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(executor().context(pid)->arena);
  }
  ValueDims vd() { return valueDims(executor().context(pid)->core); }

  /// One wake: the timer, then the turns it queued.
  void wake() {
    executor().tick();
    host->pump();
  }
  void advance(std::int64_t ms) { sim.advance(ms * kNanosPerMilli); }
  /// Advance by @p ms and wake.
  void pass(std::int64_t ms) {
    advance(ms);
    wake();
  }
  [[nodiscard]] double steadySeconds() const {
    return static_cast<double>(sim.steady) / 1e9;
  }

  std::optional<zigzag::CellRef> profile(const std::string &principal) {
    const auto dims  = registerDimensions(arena());
    const auto agent = *executor().agentCell(pid);
    for (const auto p : zigzag::rankAfter(arena(), agent, dims.trust))
      if (arena().textOf(p) == principal) return p;
    return std::nullopt;
  }
  std::optional<zigzag::CellRef> field(zigzag::CellRef owner,
                                       std::string_view name) {
    return fieldValue(arena(), vd(), owner, name);
  }
  std::optional<double> real(const std::string &principal,
                             std::string_view name) {
    const auto p = profile(principal);
    const auto c = p ? field(*p, name) : std::nullopt;
    return c ? arena().asDouble(*c) : std::nullopt;
  }
  std::optional<bool> flag(const std::string &principal,
                           std::string_view name) {
    const auto p = profile(principal);
    const auto c = p ? field(*p, name) : std::nullopt;
    return c ? arena().asBool(*c) : std::nullopt;
  }
  std::optional<std::string> text(const std::string &principal,
                                  std::string_view name) {
    const auto p = profile(principal);
    const auto c = p ? field(*p, name) : std::nullopt;
    return c ? std::optional{arena().textOf(*c)} : std::nullopt;
  }
  std::optional<zigzag::CellRef> stream(const std::string &principal,
                                        std::string_view predicate) {
    const auto p = profile(principal);
    const auto s = p ? field(*p, kProfileStreams) : std::nullopt;
    return s ? field(*s, predicate) : std::nullopt;
  }
  std::optional<double> streamReal(const std::string &principal,
                                   std::string_view predicate,
                                   std::string_view name) {
    const auto s = stream(principal, predicate);
    const auto c = s ? field(*s, name) : std::nullopt;
    return c ? arena().asDouble(*c) : std::nullopt;
  }
  bool streamStale(const std::string &principal, std::string_view predicate) {
    const auto s = stream(principal, predicate);
    const auto c = s ? field(*s, kStreamStale) : std::nullopt;
    return c && arena().asBool(*c).value_or(false);
  }
  std::optional<SourceHealth> health(const std::string &principal) {
    return executor().sourceHealth(pid, principal);
  }
  [[nodiscard]] std::size_t slots() { return arena().cellCount(); }
};

class VagentEpistemicLiveness : public ::testing::Test {
protected:
  Side a;
  xanadu::MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  std::unique_ptr<EpistemicAdapter> adapter;
  id::StandardCryptoEngine syncKey = newKey();
  id::StandardCryptoEngine freeKey = newKey();
  std::string syncId               = issuerOf(syncKey.publicKey());
  std::string freeId               = issuerOf(freeKey.publicKey());
  const std::string relayId        = std::string(64, '7');
  std::vector<ClockChallenge> challenges;
  std::int64_t sequence = 0;

  void SetUp() override {
    registry.load();
    a.launch();
    adapter = makeAdapter(a);
  }
  std::unique_ptr<EpistemicAdapter> makeAdapter(Side &side,
                                                AdapterLimits limits = {}) {
    auto made = std::make_unique<EpistemicAdapter>(side.executor(), limits,
                                                   registry, masters);
    made->setChallengeSink(
        [this](const ClockChallenge &c) { challenges.push_back(c); });
    return made;
  }

  // --- bindings
  // ---------------------------------------------------------------

  SourceBinding syncBinding(std::optional<std::int64_t> heartbeatMs = 1000,
                            std::optional<std::int64_t> dataMs = 2000) const {
    SourceBinding b;
    b.alias      = "sensor:sync";
    b.principal  = syncId;
    b.lifetime   = std::chrono::hours(24);
    b.predicates = {"at"};
    if (heartbeatMs) b.heartbeat = milliseconds(*heartbeatMs);
    if (dataMs) b.data["at"] = milliseconds(*dataMs);
    return b;
  }
  SourceBinding freeBinding(std::int64_t heartbeatMs = 1000) const {
    SourceBinding b;
    b.alias      = "sensor:free";
    b.principal  = freeId;
    b.lifetime   = std::chrono::hours(24);
    b.predicates = {"seen"};
    b.heartbeat  = milliseconds(heartbeatMs);
    return b;
  }
  void bind(Side &side, EpistemicAdapter &through, SourceBinding b) {
    const auto bound = through.bind(side.pid, std::move(b));
    ASSERT_TRUE(bound) << epistemicRefusalName(bound.error());
    side.host->pump();
  }
  void bindSync(std::optional<std::int64_t> hb   = 1000,
                std::optional<std::int64_t> data = 2000) {
    bind(a, *adapter, syncBinding(hb, data));
  }

  // --- messages
  // ---------------------------------------------------------------

  std::int64_t deviceNow(Side &side) {
    return static_cast<std::int64_t>(
               std::llround(static_cast<double>(side.sim.steady) *
                            (1.0 + kSimDriftPpm * 1e-6))) +
           kSimDeviceBias;
  }
  std::string audience(Side &side) {
    const auto v = side.executor().audience(side.pid);
    EXPECT_TRUE(v);
    return v.value_or("");
  }
  SignedHeader header(Side &side, SignedKind kind, SignedClockBasis basis,
                      const std::string &issuer) {
    SignedHeader h;
    h.kind        = kind;
    h.issuer      = issuer;
    h.incarnation = 1;
    h.sequence    = ++sequence;
    h.audience    = audience(side);
    h.epoch       = 1;
    h.basis       = basis;
    h.sent        = basis == SignedClockBasis::Synchronised ? side.sim.wall
                                                            : deviceNow(side);
    return h;
  }
  std::string seal(const SignedHeader &h, const SignedPayload &p,
                   const id::StandardCryptoEngine &signer) const {
    auto envelope = signEnvelope(signer, encodeSignedBody(h, p));
    EXPECT_TRUE(envelope.has_value());
    return encodeSignedEnvelope(*envelope);
  }
  /// A synchronised device's heartbeat, sent @p lateMs before now and
  /// claiming @p uncertainty of clock error.
  std::string heartbeat(Side &side, std::int64_t lateMs = 0,
                        std::int64_t uncertainty = kNanosPerMilli) {
    auto h = header(side, SignedKind::Heartbeat, SignedClockBasis::Synchronised,
                    syncId);
    h.sent -= lateMs * kNanosPerMilli;
    h.clockUncertainty = uncertainty;
    h.expires          = side.sim.wall + 5 * kNanosPerSecond;
    return seal(h, HeartbeatPayload{}, syncKey);
  }
  std::string freeHeartbeat(Side &side) {
    auto h =
        header(side, SignedKind::Heartbeat, SignedClockBasis::Free, freeId);
    h.expires = h.sent + 5 * kNanosPerSecond;
    return seal(h, HeartbeatPayload{}, freeKey);
  }
  std::string syncReading(Side &side, const std::string &eventId) {
    auto h             = header(side, SignedKind::Observation,
                                SignedClockBasis::Synchronised, syncId);
    h.clockUncertainty = kNanosPerMilli;
    ObservationPayload o;
    o.eventId        = eventId;
    o.predicate      = "at";
    const auto bound = side.executor().boundPredicate(side.pid, syncId, "at");
    o.policy         = bound ? policyHash(bound->policy) : std::string(64, '0');
    o.acquired       = h.sent - 200 * kNanosPerMilli;
    o.tuple          = {std::string("self"), encodeSignedFloat(1.5)};
    o.frame          = "enu";
    o.units          = "m";
    o.variance       = 0.25;
    return seal(h, o, syncKey);
  }
  std::string freeReading(Side &side, const std::string &eventId) {
    auto h =
        header(side, SignedKind::Observation, SignedClockBasis::Free, freeId);
    ObservationPayload o;
    o.eventId        = eventId;
    o.predicate      = "seen";
    const auto bound = side.executor().boundPredicate(side.pid, freeId, "seen");
    o.policy         = bound ? policyHash(bound->policy) : std::string(64, '0');
    o.acquired       = h.sent - 200 * kNanosPerMilli;
    o.tuple          = {std::string("self"), std::int64_t{3}};
    return seal(h, o, freeKey);
  }
  std::string reply(Side &side, const ClockChallenge &c) {
    side.sim.advance(kNanosPerMilli);
    auto h =
        header(side, SignedKind::ClockPair, SignedClockBasis::Free, freeId);
    return seal(h, ClockPairPayload{c.nonce}, freeKey);
  }
  std::expected<TransportReceipt, EpistemicRefusal>
  ingest(Side &side, EpistemicAdapter &through, const std::string &bytes,
         const std::string &peer) {
    auto result = through.ingest(bytes, TransportPeer{peer});
    side.host->pump();
    return result;
  }
  std::expected<TransportReceipt, EpistemicRefusal>
  ingest(const std::string &bytes) {
    return ingest(a, *adapter, bytes, syncId);
  }
  /// A grant naming @p principal at the executor directly, with a heartbeat.
  static EpistemicSourceGrant grant(const std::string &principal,
                                    std::string alias,
                                    std::optional<std::int64_t> heartbeatMs,
                                    std::optional<std::int64_t> dataMs = {}) {
    EpistemicSourceGrant g{std::move(alias), principal, 1, "at",
                           std::chrono::hours(1)};
    if (heartbeatMs) g.heartbeat = milliseconds(*heartbeatMs);
    if (dataMs) g.data = milliseconds(*dataMs);
    return g;
  }

  bool silent(Side &side, const std::string &principal) {
    const auto h = side.health(principal);
    return h && h->silent;
  }
  std::string act(Side &side, HealthAction action, const std::string &who) {
    static int grants = 0;
    EpistemicHealthGrant g{"live-" + std::to_string(++grants),
                           *HealthIssuer::fromPrincipal(std::string(64, 'b')),
                           {who},
                           {},
                           std::chrono::hours(1),
                           16};
    for (const auto kind :
         {HealthActionKind::Isolate, HealthActionKind::Degrade,
          HealthActionKind::Clear, HealthActionKind::Reset})
      g.actions.push_back(kind);
    const auto token = side.executor().grantHealth(side.pid, g);
    if (!token) return "no grant";
    const auto receipt = side.executor().healthAction(
        side.pid, {*token, who, std::move(action)});
    if (!receipt) return std::string(epistemicRefusalName(receipt.error()));
    side.host->pump();
    const auto view = side.executor().healthOutcome(side.pid, *receipt);
    if (view.state != HealthOutcomeView::State::Known || view.items.empty())
      return "NoChange";
    const auto &outcome = view.items.front().outcome;
    return outcome.kind == IngestOutcome::Kind::Committed
               ? "Committed"
               : outcome.code.value_or("?");
  }
};

// --- the state machine -------------------------------------------------------

TEST_F(VagentEpistemicLiveness,
       AnUnheardSourceGoesSilentWhenItsFirstDeadlinePasses) {
  bindSync(1000, std::nullopt);
  // A source never heard has no profile yet, and nothing to read.
  EXPECT_FALSE(a.health(syncId));
  a.pass(900);
  EXPECT_FALSE(a.health(syncId));
  a.pass(200);
  const auto after = a.health(syncId);
  ASSERT_TRUE(after);
  EXPECT_TRUE(after->silent);
  EXPECT_EQ(after->silentCause, std::string(kSilentCauseHeartbeat));
  // Silence is read beside the state and never into it (design 7).
  EXPECT_EQ(after->state, HealthState::Nominal);
  EXPECT_EQ(after->weight, 0.0);
  EXPECT_EQ(after->refusalFraction, 0.0);
  EXPECT_TRUE(after->reasons.empty());
  EXPECT_FALSE(after->livenessPresent);
  EXPECT_EQ(after->incarnation, 1U);
  EXPECT_FALSE(after->faulted);
  // It has no deadline now, so nothing fires again.
  EXPECT_EQ(a.executor().livenessEntries(), 0U);
  const auto items = a.executor().livenessItemsPosted();
  a.pass(10'000);
  EXPECT_EQ(a.executor().livenessItemsPosted(), items);
}

TEST_F(VagentEpistemicLiveness,
       LiveSilentLiveWritesTheArenaOnlyOnTheTransition) {
  bindSync(1000, std::nullopt);
  // LIVE: ten heartbeats within the deadline write nothing at all.
  ASSERT_TRUE(ingest(heartbeat(a)));
  const auto slots = a.slots();
  const auto cells = *a.executor().epistemicCells(a.pid);
  for (int i = 0; i < 10; ++i) {
    a.advance(300);
    ASSERT_TRUE(ingest(heartbeat(a))) << i;
    a.wake();
  }
  EXPECT_EQ(a.slots(), slots);
  EXPECT_EQ(*a.executor().epistemicCells(a.pid), cells);
  EXPECT_EQ(a.executor().livenessItemsPosted(), 0U);
  EXPECT_FALSE(silent(a, syncId));
  // SILENT once the deadline passes with nothing heard.
  a.pass(1100);
  EXPECT_TRUE(silent(a, syncId));
  EXPECT_EQ(a.executor().livenessItemsPosted(), 1U);
  EXPECT_EQ(a.text(syncId, kProfileSilentCause), kSilentCauseHeartbeat);
  EXPECT_TRUE(a.real(syncId, kProfileSilentSince));
  // The next fresh heartbeat ends it: one item, one transition.
  ASSERT_TRUE(ingest(heartbeat(a)));
  EXPECT_FALSE(silent(a, syncId));
  EXPECT_EQ(a.executor().livenessItemsPosted(), 2U);
  // And LIVE writes nothing again.
  const auto later = a.slots();
  for (int i = 0; i < 5; ++i) {
    a.advance(200);
    ASSERT_TRUE(ingest(heartbeat(a)));
  }
  EXPECT_EQ(a.slots(), later);
  EXPECT_EQ(a.executor().livenessItemsPosted(), 2U);
}

TEST_F(VagentEpistemicLiveness,
       ARelayedFreshHeartbeatRefreshesAndANonFreshOneDoesNot) {
  bindSync(1000, std::nullopt);
  a.pass(1100);
  ASSERT_TRUE(silent(a, syncId));
  // Signed 6 s ago, delivered now: past the path bound (5 s) plus the
  // 0.1 s of wall sync, so it is not fresh and revives nothing (S-2).
  const auto withheld = ingest(a, *adapter, heartbeat(a, 6000), syncId);
  ASSERT_FALSE(withheld);
  EXPECT_EQ(withheld.error(), EpistemicRefusal::LateDelivery);
  EXPECT_TRUE(silent(a, syncId));
  // It did not raise the heartbeat floor either: the next one still verifies.
  // A relayed copy of a fresh heartbeat refreshes liveness all the same.
  const auto relayed = ingest(a, *adapter, heartbeat(a), relayId);
  ASSERT_TRUE(relayed);
  EXPECT_TRUE(relayed->eventId.empty());
  EXPECT_FALSE(silent(a, syncId));
  // The source is timed from the heartbeat, not from the bind.
  a.pass(900);
  EXPECT_FALSE(silent(a, syncId));
  a.pass(200);
  EXPECT_TRUE(silent(a, syncId));
}

TEST_F(VagentEpistemicLiveness,
       AHugeClockUncertaintyDoesNotWidenTheFreshnessWindow) {
  bindSync(1000, std::nullopt);
  a.pass(1100);
  ASSERT_TRUE(silent(a, syncId));
  // Claimed uncertainty is clamped at the path bound, so an hour of it buys
  // at most max_path_lateness (5 s) more than the bound itself.
  const auto late =
      ingest(a, *adapter, heartbeat(a, 11'000, 3600 * kNanosPerSecond), syncId);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), EpistemicRefusal::LateDelivery);
  EXPECT_TRUE(silent(a, syncId));
  // Within the clamped window it is fresh, and claiming little does not
  // shrink it below the bound.
  const auto widened =
      ingest(a, *adapter, heartbeat(a, 7000, 3600 * kNanosPerSecond), syncId);
  EXPECT_TRUE(widened);
  EXPECT_FALSE(silent(a, syncId));
  a.pass(1100);
  ASSERT_TRUE(silent(a, syncId));
  const auto modest = ingest(a, *adapter, heartbeat(a, 7000), syncId);
  ASSERT_FALSE(modest);
  EXPECT_EQ(modest.error(), EpistemicRefusal::LateDelivery);
  EXPECT_TRUE(silent(a, syncId));
}

TEST_F(VagentEpistemicLiveness,
       AStreamGoesStaleAfterItsDataIntervalAndTheNextCommitClearsIt) {
  bindSync(10'000, 1000);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  ASSERT_EQ(a.executor().ingestOutcome(a.pid, syncId, 1, "e1")->kind,
            IngestOutcome::Kind::Committed);
  const auto received = a.streamReal(syncId, "at", kStreamReceived);
  ASSERT_TRUE(received);
  EXPECT_EQ(a.real(syncId, kProfileLastReceive), received);
  EXPECT_EQ(a.text(syncId, kProfileLivenessInstance),
            a.executor().clockInstance());
  a.pass(900);
  EXPECT_FALSE(a.streamStale(syncId, "at"));
  a.pass(200);
  EXPECT_TRUE(a.streamStale(syncId, "at"));
  const auto stale = a.health(syncId);
  ASSERT_TRUE(stale);
  EXPECT_EQ(stale->staleStreams, 1U);
  // A stale stream is not a silent source, and it moves nothing else.
  EXPECT_FALSE(stale->silent);
  EXPECT_EQ(stale->state, HealthState::Nominal);
  EXPECT_TRUE(stale->reasons.empty());
  ASSERT_TRUE(ingest(syncReading(a, "e2")));
  EXPECT_FALSE(a.streamStale(syncId, "at"));
  EXPECT_EQ(a.health(syncId)->staleStreams, 0U);
  EXPECT_GT(*a.streamReal(syncId, "at", kStreamReceived), *received);
}

TEST_F(VagentEpistemicLiveness, ACommitRacingTheTimerWins) {
  bindSync(1000, 1000);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  const auto armed = a.real(syncId, kProfileLastReceive);
  ASSERT_TRUE(armed);
  a.advance(950);
  // The commit re-arms the source and its stream from its own receipt.
  ASSERT_TRUE(ingest(syncReading(a, "e2")));
  a.pass(100);
  EXPECT_FALSE(silent(a, syncId));
  EXPECT_FALSE(a.streamStale(syncId, "at"));
  EXPECT_EQ(a.executor().livenessItemsPosted(), 0U);
  // A stale entry delivered anyway, armed from the older stamp or from none,
  // finds a newer stamp in the arena and sets nothing.
  LivenessDue silence;
  silence.kind        = LivenessDue::Kind::Silent;
  silence.principal   = syncId;
  silence.stamp       = armed;
  LivenessDue unheard = silence;
  unheard.stamp       = std::nullopt;
  LivenessDue staleness;
  staleness.kind      = LivenessDue::Kind::Stale;
  staleness.principal = syncId;
  staleness.predicate = "at";
  staleness.stamp     = armed;
  a.executor().postLivenessItem(a.pid, {silence, unheard, staleness});
  a.host->pump();
  EXPECT_FALSE(silent(a, syncId));
  EXPECT_FALSE(a.streamStale(syncId, "at"));
  // The same entries armed from the current stamp do set their flags.
  silence.stamp   = a.real(syncId, kProfileLastReceive);
  staleness.stamp = a.streamReal(syncId, "at", kStreamReceived);
  a.executor().postLivenessItem(a.pid, {silence, staleness});
  a.host->pump();
  EXPECT_TRUE(silent(a, syncId));
  EXPECT_TRUE(a.streamStale(syncId, "at"));
}

TEST_F(VagentEpistemicLiveness, TheFlagsMoveNothingAndAResetClearsThemAll) {
  bindSync(1000, 2000);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  const auto baseline = a.health(syncId);
  ASSERT_TRUE(baseline);
  const auto weights = [&] {
    return std::pair{a.real(syncId, kProfileHealthRefusedWeight),
                     a.real(syncId, kProfileHealthWeight)};
  };
  const auto rn = weights();
  a.pass(2500);
  const auto flagged = a.health(syncId);
  ASSERT_TRUE(flagged);
  ASSERT_TRUE(flagged->silent);
  ASSERT_EQ(flagged->staleStreams, 1U);
  // No R or N, no reason, no change of the displayed state.
  EXPECT_EQ(weights(), rn);
  EXPECT_EQ(flagged->state, baseline->state);
  EXPECT_EQ(flagged->weight, baseline->weight);
  EXPECT_EQ(flagged->refusalFraction, baseline->refusalFraction);
  EXPECT_EQ(flagged->reasons.size(), baseline->reasons.size());
  EXPECT_EQ(flagged->reasonCode, baseline->reasonCode);
  EXPECT_FALSE(flagged->faulted);

  ASSERT_EQ(act(a, Reset{0}, syncId), "Committed");
  const auto reset = a.health(syncId);
  ASSERT_TRUE(reset);
  EXPECT_FALSE(reset->silent);
  EXPECT_EQ(reset->staleStreams, 0U);
  EXPECT_FALSE(reset->livenessPresent);
  EXPECT_FALSE(a.real(syncId, kProfileLastReceive));
  EXPECT_FALSE(a.real(syncId, kProfileLastContact));
  // UNHEARD again, with its deadline restarted from the reset.
  a.pass(900);
  EXPECT_FALSE(silent(a, syncId));
  a.pass(200);
  EXPECT_TRUE(silent(a, syncId));
  EXPECT_EQ(a.text(syncId, kProfileSilentCause), kSilentCauseHeartbeat);
}

TEST_F(VagentEpistemicLiveness, ALatchedSourceArmsNothing) {
  bindSync(1000, 1000);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  EXPECT_EQ(a.executor().livenessEntries(), 2U);
  ASSERT_EQ(act(a, Isolate{"stuck"}, syncId), "Committed");
  EXPECT_EQ(a.executor().livenessEntries(), 0U);
  a.pass(30'000);
  const auto latched = a.health(syncId);
  ASSERT_TRUE(latched);
  EXPECT_TRUE(latched->faulted);
  EXPECT_FALSE(latched->silent);
  EXPECT_EQ(latched->staleStreams, 0U);
  // Heartbeats refresh nothing of a latched source either.
  ASSERT_TRUE(ingest(heartbeat(a)));
  EXPECT_EQ(a.executor().livenessEntries(), 0U);
  // A bind that finds the arena's latch arms nothing.
  bindSync(1000, 1000);
  EXPECT_EQ(a.executor().livenessEntries(), 0U);
  a.pass(30'000);
  EXPECT_FALSE(silent(a, syncId));
  // A reset restarts the deadlines from the reset: the profile's, and the
  // stream's from the `received` the reset kept.
  ASSERT_EQ(act(a, Reset{0}, syncId), "Committed");
  EXPECT_EQ(a.executor().livenessEntries(), 2U);
}

// --- the floor and the bounds ------------------------------------------------

TEST_F(VagentEpistemicLiveness, ADurationBelowTheFloorRefusesAtBind) {
  const auto refuses = [&](SourceBinding b) {
    const auto bound = adapter->bind(a.pid, std::move(b));
    EXPECT_FALSE(bound);
    return !bound && bound.error() == EpistemicRefusal::InvalidBinding;
  };
  EXPECT_TRUE(refuses(syncBinding(1000, 1)));
  EXPECT_TRUE(refuses(syncBinding(1000, kFloorMs - 1)));
  EXPECT_TRUE(refuses(syncBinding(kFloorMs - 1, 1000)));
  auto shortLife     = syncBinding();
  shortLife.lifetime = milliseconds(kFloorMs - 1);
  EXPECT_TRUE(refuses(shortLife));
  auto stray            = syncBinding();
  stray.data["nothing"] = milliseconds(1000);
  EXPECT_TRUE(refuses(stray));
  EXPECT_FALSE(a.executor().boundPredicate(a.pid, syncId, "at"));
  // Exactly the floor is allowed.
  auto edge     = syncBinding(kFloorMs, kFloorMs);
  edge.lifetime = milliseconds(kFloorMs);
  EXPECT_TRUE(adapter->bind(a.pid, edge));
  // The executor refuses the same, so a direct caller cannot bypass the
  // adapter.
  auto &executor = a.executor();
  for (const auto &g :
       {grant(freeId, "x", kFloorMs - 1), grant(freeId, "x", 1000, 1),
        grant(freeId, "x", 1000, kFloorMs - 1)}) {
    const auto bound = executor.bindSource(a.pid, g);
    ASSERT_FALSE(bound);
    EXPECT_EQ(bound.error(), EpistemicRefusal::InvalidBinding);
  }
  auto brief       = grant(freeId, "x", 1000);
  brief.lifetime   = milliseconds(kFloorMs - 1);
  const auto bound = executor.bindSource(a.pid, brief);
  ASSERT_FALSE(bound);
  EXPECT_EQ(bound.error(), EpistemicRefusal::InvalidBinding);
  // The setting is ranged too.
  AdapterLimits limits;
  limits.heartbeatFloorMs = kMinHeartbeatFloorMs - 1;
  EXPECT_FALSE(limits.validate());
  limits                 = AdapterLimits{};
  limits.sequenceGapNote = 0;
  EXPECT_FALSE(limits.validate());
  EpistemicLimits ep;
  ep.heartbeatFloorMs = kMaxHeartbeatFloorMs + 1;
  EXPECT_FALSE(ep.validate());
}

TEST_F(VagentEpistemicLiveness,
       TheHeapStaysWithinSourcesTimesStreamsUnderManyHeartbeats) {
  // Four sources with a heartbeat and one stream each.
  std::vector<std::string> sources;
  for (int i = 0; i < 4; ++i) {
    char name[65];
    std::snprintf(name, sizeof name, "c%063x", i);
    sources.emplace_back(name);
    ASSERT_TRUE(a.executor().bindSource(
        a.pid, grant(sources.back(), "s" + std::to_string(i), 1000, 5000)));
  }
  a.host->pump();
  EXPECT_EQ(a.executor().livenessEntries(), sources.size());
  // One real commit each gives each a stream entry.
  std::uint64_t seq = 0;
  for (const auto &principal : sources) {
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = 1;
    o.eventId     = "e" + std::to_string(++seq);
    o.sequence    = seq;
    o.predicate   = "at";
    o.tuple       = {WireValue::text("self"), WireValue::real(1.0)};
    setTimes(o,
             static_cast<double>(a.sim.wall) /
                     static_cast<double>(kNanosPerSecond) -
                 1.0,
             0.01);
    o.clockBasis = "wall";
    o.frame      = "enu";
    o.units      = "m";
    o.variance   = 0.25;
    ASSERT_TRUE(a.executor().observe(a.pid, o)) << principal;
  }
  a.host->pump();
  const auto bound = sources.size() * 2;
  EXPECT_EQ(a.executor().livenessEntries(), bound);
  // 100,000 fresh heartbeats re-arm in place and add nothing.
  for (int i = 0; i < 100'000; ++i) {
    a.advance(0);
    a.executor().noteFresh(
        a.pid, sources[static_cast<std::size_t>(i) % 4],
        std::chrono::steady_clock::time_point(
            std::chrono::steady_clock::duration(a.sim.steady + i)));
  }
  EXPECT_EQ(a.executor().livenessEntries(), bound);
  EXPECT_EQ(a.executor().livenessItemsPosted(), 0U);
}

// Sources heard once, so each has a profile and a stamp; @p count of them,
// each timed by a heartbeat of 200 ms.
std::vector<std::string> heardSources(Side &host, std::size_t count,
                                      std::uint64_t first) {
  std::vector<std::string> sources;
  std::uint64_t seq = 0;
  for (std::size_t i = 0; i < count; ++i) {
    char name[65];
    std::snprintf(name, sizeof name, "a%063llx",
                  static_cast<unsigned long long>(first + i));
    sources.emplace_back(name);
    EpistemicSourceGrant g{"s" + std::to_string(i), sources.back(), 1, "at",
                           std::chrono::hours(1)};
    g.heartbeat = milliseconds(200);
    EXPECT_TRUE(host.executor().bindSource(host.pid, g));
  }
  host.host->pump();
  for (const auto &principal : sources) {
    CheckedObservation o;
    o.principal   = principal;
    o.incarnation = 1;
    o.eventId     = "e" + std::to_string(++seq);
    o.sequence    = seq;
    o.predicate   = "at";
    o.tuple       = {WireValue::text("self"), WireValue::real(1.0)};
    setTimes(o,
             static_cast<double>(host.sim.wall) /
                     static_cast<double>(kNanosPerSecond) -
                 1.0,
             0.01);
    o.clockBasis = "wall";
    o.frame      = "enu";
    o.units      = "m";
    o.variance   = 0.25;
    EXPECT_TRUE(host.executor().observe(host.pid, o)) << principal;
    host.host->pump();
  }
  return sources;
}

TEST_F(VagentEpistemicLiveness, ManySourcesDueInOneWakeProduceOneItem) {
  // Profiles with histories for this many sources need a larger ceiling than
  // the default.
  AgentSettings settings;
  settings.epistemic.cells = 400'000;
  Side big;
  big.launch(settings);
  const auto sources = heardSources(big, 64, 0x1000);
  ASSERT_EQ(big.executor().livenessEntries(), 64U);
  big.advance(250);
  big.executor().tick();
  EXPECT_EQ(big.executor().livenessItemsPosted(), 1U);
  EXPECT_EQ(big.executor().livenessEntries(), 0U);
  big.host->pump();
  std::size_t quiet = 0;
  for (const auto &principal : sources)
    if (silent(big, principal)) ++quiet;
  EXPECT_EQ(quiet, 64U);
  // A wake with nothing due posts nothing.
  big.wake();
  EXPECT_EQ(big.executor().livenessItemsPosted(), 1U);
}

// A turn's scratch is bounded by its grant, and each silence writes about 330
// bytes into it, so a wake that finds the most `max_sources` allows (128) posts
// two items rather than one that would be undone whole.
TEST_F(VagentEpistemicLiveness, ALargerWakeIsSplitOnlyByTheTurnsScratch) {
  AgentSettings settings;
  settings.epistemic.cells = 400'000;
  Side big;
  big.launch(settings);
  const auto sources = heardSources(big, 128, 0x2000);
  ASSERT_EQ(big.executor().livenessEntries(), 128U);
  big.advance(250);
  big.executor().tick();
  EXPECT_EQ(big.executor().livenessItemsPosted(), 2U);
  big.host->pump();
  std::size_t quiet = 0;
  for (const auto &principal : sources)
    if (silent(big, principal)) ++quiet;
  EXPECT_EQ(quiet, 128U);
}

// A silence of a source never heard makes its profile in the turn, which costs
// scratch the turn's grant bounds: the wake then posts more than one item.
TEST_F(VagentEpistemicLiveness,
       SourcesNeverHeardCostMoreScratchAndSplitSooner) {
  std::vector<std::string> sources;
  for (int i = 0; i < 128; ++i) {
    char name[65];
    std::snprintf(name, sizeof name, "b%063x", i);
    sources.emplace_back(name);
    ASSERT_TRUE(a.executor().bindSource(
        a.pid, grant(sources.back(), "u" + std::to_string(i), 200)));
  }
  a.host->pump();
  a.advance(250);
  a.executor().tick();
  const auto items = a.executor().livenessItemsPosted();
  EXPECT_GE(items, 16U);
  EXPECT_LE(items, 20U);
  a.host->pump();
  std::size_t quiet = 0;
  for (const auto &principal : sources)
    if (silent(a, principal)) ++quiet;
  EXPECT_EQ(quiet, 128U);
}

TEST_F(VagentEpistemicLiveness, TheEarliestDeadlineIncludesTheHeapsHead) {
  bindSync(1000, std::nullopt);
  const auto next = a.executor().earliestDeadline();
  ASSERT_TRUE(next);
  const auto wall = std::chrono::system_clock::time_point(
      std::chrono::system_clock::duration(a.sim.wall));
  // The lease renews far later than a one-second heartbeat.
  EXPECT_LE(*next, wall + milliseconds(1000));
  EXPECT_GE(*next, wall + milliseconds(990));
  // Simulated time stops at it and fires it.
  a.executor().advanceSimulatedTime(milliseconds(1200));
  EXPECT_TRUE(silent(a, syncId));
}

// --- sequence gaps
// -------------------------------------------------------------

TEST_F(VagentEpistemicLiveness, SequenceGapsAreCountedPerIssuerAcrossKinds) {
  AdapterLimits limits;
  limits.sequenceGapNote = 10;
  auto noted             = makeAdapter(a, limits);
  bind(a, *noted, syncBinding());
  bind(a, *noted, freeBinding());
  ASSERT_TRUE(ingest(a, *noted, heartbeat(a), syncId));
  EXPECT_EQ(noted->sequenceGaps(syncId), 0U);
  // The next sequence the device uses is 40 above the last: one gap, though
  // it is the first observation of its stream.
  sequence += 40;
  ASSERT_TRUE(ingest(a, *noted, syncReading(a, "e1"), syncId));
  EXPECT_EQ(noted->sequenceGaps(syncId), 1U);
  // A step of exactly the note is not above it.
  sequence += 9;
  ASSERT_TRUE(ingest(a, *noted, heartbeat(a), syncId));
  EXPECT_EQ(noted->sequenceGaps(syncId), 1U);
  sequence += 10;
  a.advance(10);
  ASSERT_TRUE(ingest(a, *noted, heartbeat(a), syncId));
  EXPECT_EQ(noted->sequenceGaps(syncId), 2U);
  // The other issuer is counted on its own.
  EXPECT_EQ(noted->sequenceGaps(freeId), 0U);
  EXPECT_EQ(noted->sequenceGaps(std::string(64, '0')), 0U);
}

// --- the clock cause
// ----------------------------------------------------------

TEST_F(VagentEpistemicLiveness,
       AMappingDroppedByAJumpThatMissesTheDeadlineSilencesWithTheClockCause) {
  bind(a, *adapter, freeBinding(5000));
  const auto first = ingest(a, *adapter, freeHeartbeat(a), freeId);
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), EpistemicRefusal::UnknownClockMapping);
  ASSERT_EQ(challenges.size(), 1U);
  ASSERT_TRUE(ingest(a, *adapter, reply(a, challenges.back()), freeId));
  ASSERT_TRUE(ingest(a, *adapter, freeHeartbeat(a), freeId));
  EXPECT_FALSE(silent(a, freeId));
  EXPECT_FALSE(adapter->clockPending(a.pid, freeId));
  // A suspend: the wall clock runs, the steady one stops. The next message
  // drops the paired mapping, is refused, and starts a pairing.
  a.sim.suspend(60 * kNanosPerSecond);
  a.wake();
  // A challenge a second after the last one: the interval of 4.2.
  a.advance(1100);
  const auto jumped = ingest(a, *adapter, freeHeartbeat(a), freeId);
  ASSERT_FALSE(jumped);
  EXPECT_EQ(jumped.error(), EpistemicRefusal::UnknownClockMapping);
  ASSERT_TRUE(adapter->clockPending(a.pid, freeId));
  ASSERT_EQ(challenges.size(), 2U);
  // Still LIVE: the heartbeat deadline is on the steady clock, which stood
  // still, and the refused message refreshed nothing.
  EXPECT_FALSE(silent(a, freeId));
  // The pairing does not complete before the deadline: silent, `clock`.
  a.pass(4500);
  const auto health = a.health(freeId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->silent);
  EXPECT_EQ(health->silentCause, std::string(kSilentCauseClock));
  EXPECT_EQ(health->state, HealthState::Nominal);
  EXPECT_TRUE(health->reasons.empty());
  // The device answers late: a challenge older than its timeout is refused,
  // so the source's next message starts another, and that pairing completes.
  const auto stale = ingest(a, *adapter, reply(a, challenges.back()), freeId);
  EXPECT_FALSE(stale);
  EXPECT_TRUE(silent(a, freeId));
  const auto again = ingest(a, *adapter, freeHeartbeat(a), freeId);
  ASSERT_FALSE(again);
  EXPECT_EQ(again.error(), EpistemicRefusal::UnknownClockMapping);
  ASSERT_EQ(challenges.size(), 3U);
  ASSERT_TRUE(ingest(a, *adapter, reply(a, challenges.back()), freeId));
  // The completed pairing ends the pending state; the silence waits for the
  // next fresh message, which clears it.
  EXPECT_FALSE(adapter->clockPending(a.pid, freeId));
  EXPECT_TRUE(silent(a, freeId));
  ASSERT_TRUE(ingest(a, *adapter, freeHeartbeat(a), freeId));
  EXPECT_FALSE(silent(a, freeId));
  // The next lapse is an ordinary one.
  a.pass(5100);
  EXPECT_EQ(a.health(freeId)->silentCause,
            std::optional<std::string>(std::string(kSilentCauseHeartbeat)));
}

// --- resume
// --------------------------------------------------------------------

/// The new host: its steady epoch is 1,000 s apart and its wall is later.
struct Resumed {
  Side side;
  Resumed() : side(-1000 * kNanosPerSecond) {}
};

TEST_F(VagentEpistemicLiveness,
       ResumeRebasesTheStampsConservativelyAndArmsFromThem) {
  bindSync(1000, 5000);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  const auto oldInstance = a.executor().clockInstance();
  const double t         = *a.real(syncId, kProfileLastReceive);
  const double tStream   = *a.streamReal(syncId, "at", kStreamReceived);
  const double oldOffset = a.sim.offset();
  a.advance(400);
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  Resumed r;
  Side &b    = r.side;
  b.sim.wall = a.sim.wall + 4 * kNanosPerSecond;
  ASSERT_TRUE(b.resume(*saved));
  // Carried before anything runs.
  EXPECT_EQ(b.real(syncId, kProfileLastReceive), t);
  EXPECT_EQ(b.text(syncId, kProfileLivenessInstance), oldInstance);
  auto second = makeAdapter(b);
  bind(b, *second, syncBinding(1000, 5000));
  ASSERT_NE(b.executor().clockInstance(), oldInstance);
  EXPECT_EQ(b.text(syncId, kProfileLivenessInstance),
            b.executor().clockInstance());

  // t + delta - u, the end that makes the stamp older.
  const auto rebased = b.executor().requestClockRebase(b.pid, oldInstance, t);
  ASSERT_TRUE(rebased);
  b.host->pump();
  const auto answer = b.executor().clockRebaseResult(*rebased);
  ASSERT_TRUE(answer);
  ASSERT_EQ(answer->status, AgentExecutor::ClockRebase::Status::Rebased);
  const double expected = t + answer->delta - answer->uncertainty;
  EXPECT_NEAR(*b.real(syncId, kProfileLastReceive), expected, 1e-6);
  EXPECT_NEAR(*b.real(syncId, kProfileLastContact), expected, 1e-6);
  EXPECT_NEAR(*b.streamReal(syncId, "at", kStreamReceived),
              tStream + answer->delta - answer->uncertainty, 1e-6);
  EXPECT_NEAR(answer->delta, oldOffset - b.sim.offset(), 1e-6);
  EXPECT_LT(*b.real(syncId, kProfileLastReceive), t + answer->delta);
  EXPECT_FALSE(silent(b, syncId));

  // The first deadline is the rebased stamp plus the expectation, never the
  // bind time: on this host the stamp is some 4.4 s old, so it is overdue.
  const double deadline  = expected + 1.0;
  const double nowSteady = b.steadySeconds();
  ASSERT_LT(deadline, nowSteady);
  const auto head = b.executor().earliestDeadline();
  ASSERT_TRUE(head);
  const auto wallNow = std::chrono::system_clock::time_point(
      std::chrono::system_clock::duration(b.sim.wall));
  EXPECT_NEAR(std::chrono::duration<double>(*head - wallNow).count(),
              deadline - nowSteady, 1e-3);
  b.wake();
  EXPECT_TRUE(silent(b, syncId));
  EXPECT_EQ(b.text(syncId, kProfileSilentCause), kSilentCauseHeartbeat);
}

TEST_F(VagentEpistemicLiveness,
       ResumeKeepsALiveSourceLiveWhileItsRebasedStampIsYoung) {
  bindSync(1000, std::nullopt);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  a.advance(50);
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved);
  Resumed r;
  Side &b    = r.side;
  b.sim.wall = a.sim.wall;
  ASSERT_TRUE(b.resume(*saved));
  auto second = makeAdapter(b);
  bind(b, *second, syncBinding(1000, std::nullopt));
  EXPECT_FALSE(silent(b, syncId));
  b.pass(500);
  EXPECT_FALSE(silent(b, syncId));
  b.pass(600);
  EXPECT_TRUE(silent(b, syncId));
}

TEST_F(VagentEpistemicLiveness,
       AnEvictedSegmentSetsSilentWithTheRebaseCauseAtOnce) {
  AgentSettings settings;
  settings.epistemic.clockLineage = 2;
  Side u;
  u.launch(settings);
  auto through = makeAdapter(u);
  bind(u, *through, syncBinding(1000, std::nullopt));
  ASSERT_TRUE(ingest(u, *through, syncReading(u, "e1"), syncId));
  const double t = *u.real(syncId, kProfileLastReceive);
  // A suspend appends the second segment; the resume's third evicts the first,
  // which holds the stamp.
  u.advance(500);
  u.sim.suspend(30 * kNanosPerSecond);
  u.wake();
  ASSERT_EQ(*u.real(syncId, kProfileLastReceive), t);
  const auto saved = u.executor().checkpoint(u.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  Resumed r;
  Side &b    = r.side;
  b.sim.wall = u.sim.wall;
  ASSERT_TRUE(b.resume(*saved, settings));
  auto second = makeAdapter(b);
  bind(b, *second, syncBinding(1000, std::nullopt));
  const auto health = b.health(syncId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->silent);
  EXPECT_EQ(health->silentCause, std::string(kSilentCauseRebase));
  // No grace period, no reset, and no stamp left to mistake for a time.
  EXPECT_FALSE(health->livenessPresent);
  EXPECT_EQ(health->state, HealthState::Nominal);
  EXPECT_EQ(b.text(syncId, kProfileLivenessInstance),
            b.executor().clockInstance());
  EXPECT_FALSE(b.streamReal(syncId, "at", kStreamReceived));
  // Nothing arms for it; a fresh heartbeat ends the silence.
  EXPECT_EQ(b.executor().livenessEntries(), 0U);
  b.pass(30'000);
  ASSERT_TRUE(ingest(b, *second, heartbeat(b), syncId));
  EXPECT_FALSE(silent(b, syncId));
}

TEST_F(VagentEpistemicLiveness,
       AnUnanchoredSegmentSetsSilentWithTheRebaseCause) {
  Side u;
  u.launch(AgentSettings{}, false);
  auto through = makeAdapter(u);
  bind(u, *through, freeBinding(1000));
  const auto first = ingest(u, *through, freeHeartbeat(u), freeId);
  ASSERT_FALSE(first);
  ASSERT_FALSE(challenges.empty());
  ASSERT_TRUE(ingest(u, *through, reply(u, challenges.back()), freeId));
  ASSERT_TRUE(ingest(u, *through, freeReading(u, "f1"), freeId));
  ASSERT_TRUE(u.real(freeId, kProfileLastReceive));
  const auto saved = u.executor().checkpoint(u.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  Resumed r;
  Side &b    = r.side;
  b.sim.wall = u.sim.wall;
  ASSERT_TRUE(b.resume(*saved));
  auto second = makeAdapter(b);
  bind(b, *second, freeBinding(1000));
  const auto health = b.health(freeId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->silent);
  EXPECT_EQ(health->silentCause, std::string(kSilentCauseRebase));
}

TEST_F(VagentEpistemicLiveness,
       AShapeOneProfileWithoutAnInstanceTakesTheRebasePath) {
  bindSync(1000, std::nullopt);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  // Shape-1 state: stamps with no `liveness_instance`.
  const auto p = *a.profile(syncId);
  setField(a.arena(), a.vd(), p, kProfileLivenessInstance, std::nullopt);
  ASSERT_FALSE(a.text(syncId, kProfileLivenessInstance));
  ASSERT_TRUE(a.real(syncId, kProfileLastReceive));
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  Resumed r;
  Side &b    = r.side;
  b.sim.wall = a.sim.wall;
  ASSERT_TRUE(b.resume(*saved));
  auto second = makeAdapter(b);
  bind(b, *second, syncBinding(1000, std::nullopt));
  const auto health = b.health(syncId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->silent);
  EXPECT_EQ(health->silentCause, std::string(kSilentCauseRebase));
  EXPECT_EQ(b.text(syncId, kProfileLivenessInstance),
            b.executor().clockInstance());
}

TEST_F(VagentEpistemicLiveness, AFailedRebaseDoesNotResetALatch) {
  bindSync(1000, std::nullopt);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  ASSERT_EQ(act(a, Isolate{"stuck"}, syncId), "Committed");
  const auto p = *a.profile(syncId);
  setField(a.arena(), a.vd(), p, kProfileLivenessInstance, std::nullopt);
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  Resumed r;
  Side &b    = r.side;
  b.sim.wall = a.sim.wall;
  ASSERT_TRUE(b.resume(*saved));
  auto second = makeAdapter(b);
  bind(b, *second, syncBinding(1000, std::nullopt));
  const auto health = b.health(syncId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->faulted);
  EXPECT_EQ(health->state, HealthState::Faulted);
  EXPECT_TRUE(health->silent);
  EXPECT_EQ(health->silentCause, std::string(kSilentCauseRebase));
  // A latched source arms nothing on the new host either.
  EXPECT_EQ(b.executor().livenessEntries(), 0U);
}

// --- cost
// ---------------------------------------------------------------------

TEST_F(VagentEpistemicLiveness, TheCellsLivenessAddsToAProfileAndAStream) {
  bindSync(1000, 2000);
  const auto profileOnly = *a.executor().epistemicCells(a.pid);
  ASSERT_TRUE(ingest(syncReading(a, "e1")));
  const auto first = *a.executor().epistemicCells(a.pid);
  ASSERT_TRUE(ingest(syncReading(a, "e2")));
  const auto second = *a.executor().epistemicCells(a.pid);
  std::printf("MEASURE liveness: profile alone %zu cells, first commit +%zu, "
              "second +%zu\n",
              profileOnly, first - profileOnly, second - first);
  // The flags themselves cost nothing to read and add three cells to a
  // profile when set (silent, silent_since, silent_cause), once.
  a.pass(3000);
  const auto flagged = *a.executor().epistemicCells(a.pid);
  std::printf("MEASURE liveness: a silent profile and a stale stream +%zu\n",
              flagged - second);
  EXPECT_GT(flagged, second);
  a.pass(3000);
  EXPECT_EQ(*a.executor().epistemicCells(a.pid), flagged);
}

} // namespace
