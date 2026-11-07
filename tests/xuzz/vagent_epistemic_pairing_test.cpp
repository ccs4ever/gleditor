/**
 * @file vagent_epistemic_pairing_test.cpp
 * @brief P19 I3-3b1 part B: the host's clock mappings, the challenge rules of
 *        design 4.2 and the adapter's step 6 for every kind.
 *
 * Time is simulated: one clock the test moves stands for the host's wall and
 * steady clocks, and a free device reads its own clock as the host's steady
 * time times (1 + 50 ppm) plus an offset, so a pairing's interval has
 * something real to contain. The loopback measurement alone uses real clocks.
 */
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gleditor/logging.hpp>

#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "epistemic_invariants.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;
namespace id = xanadu::identity;
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
predicate fuzzy(key id: Str, n: Int)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s,
    max_uncertainty: 150ms)
predicate tight(key id: Str, n: Int)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s,
    max_uncertainty: 100ms)
predicate snug(key id: Str, n: Int)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s,
    max_uncertainty: 101ms)
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

constexpr std::array<std::string_view, 1> kPredicates{"at"};
constexpr std::int64_t kNanosPerMilli  = 1'000'000;
constexpr std::int64_t kNanosPerSecond = 1'000'000'000;
constexpr double kSimDriftPpm          = 50.0;
/// Where a simulated free device starts its clock, in seconds.
constexpr std::int64_t kSimDeviceBias = 7 * kNanosPerSecond;

/// The warnings `vagent.ingress` logs while a scope lives.
class LogCapture {
public:
  LogCapture()
      : sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(out_)),
        logger_(gleditor::logging::category("vagent.ingress")),
        previous_(logger_->level()) {
    logger_->sinks().push_back(sink_);
    logger_->set_level(spdlog::level::warn);
  }
  ~LogCapture() {
    logger_->sinks().pop_back();
    logger_->set_level(previous_);
  }
  LogCapture(const LogCapture &)            = delete;
  LogCapture &operator=(const LogCapture &) = delete;
  [[nodiscard]] std::string text() {
    logger_->flush();
    return out_.str();
  }
  [[nodiscard]] std::size_t count(std::string_view needle) {
    const auto log = text();
    std::size_t n  = 0;
    for (auto at = log.find(needle); at != std::string::npos;
         at      = log.find(needle, at + needle.size()))
      ++n;
    return n;
  }

private:
  std::ostringstream out_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  spdlog::level::level_enum previous_;
};

/// The host's two clocks, moved only by the test.
struct SimClock {
  std::int64_t steady{};
  std::int64_t wall{};
  SimClock()
      : steady(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count()),
        wall(std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) {}
  void advance(std::int64_t nanoseconds) {
    steady += nanoseconds;
    wall += nanoseconds;
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

class VagentEpistemicPairing : public ::testing::Test {
protected:
  /// Loopback runs on the real clocks and a device whose clock is the host's.
  bool simulated = true;
  SimClock sim;
  std::unique_ptr<LocalHost> host;
  ActorPid pid;
  xanadu::MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  std::unique_ptr<EpistemicAdapter> adapter;
  id::StandardCryptoEngine device = newKey();
  std::string principal           = issuerOf(device.publicKey());
  std::vector<ClockChallenge> challenges;
  std::int64_t sequence = 0;

  void SetUp() override {
    registry.load();
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    LocalHost::Options options;
    if (simulated) options.clock = sim.agentClock();
    host = std::make_unique<LocalHost>(std::move(*program), options);
    const auto launched = host->launch("A", "a");
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    host->executor().setWallClockSynchronised(true);
    adapter = makeAdapter();
  }

  std::unique_ptr<EpistemicAdapter> makeAdapter(AdapterLimits limits = {}) {
    auto made = std::make_unique<EpistemicAdapter>(host->executor(), limits,
                                                   registry, masters);
    made->setChallengeSink(
        [this](const ClockChallenge &c) { challenges.push_back(c); });
    return made;
  }

  // --- time -----------------------------------------------------------------

  void advanceMs(std::int64_t ms) { sim.advance(ms * kNanosPerMilli); }
  std::int64_t steadyNs() { return sim.steady; }
  std::int64_t wallNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               host->executor().hostTime().wall.time_since_epoch())
        .count();
  }
  std::int64_t realSteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               host->executor().hostTime().steady.time_since_epoch())
        .count();
  }
  /// A free device's reading of its own clock now.
  std::int64_t deviceNow() {
    const auto host = realSteadyNs();
    if (!simulated) return host;
    return static_cast<std::int64_t>(std::llround(
               static_cast<double>(host) * (1.0 + kSimDriftPpm * 1e-6))) +
           kSimDeviceBias;
  }

  // --- binding and messages -------------------------------------------------

  SourceBinding binding(std::vector<std::string> predicates,
                        std::string alias = "sensor:gps") const {
    SourceBinding b;
    b.alias      = std::move(alias);
    b.principal  = principal;
    b.lifetime   = std::chrono::hours(24);
    b.predicates = std::move(predicates);
    return b;
  }
  void bind(SourceBinding b) {
    auto result = adapter->bind(pid, std::move(b));
    ASSERT_TRUE(result) << epistemicRefusalName(result.error());
    host->pump();
  }
  std::string audience() {
    const auto a = host->executor().audience(pid);
    EXPECT_TRUE(a);
    return a.value_or("");
  }
  std::string policyOf(std::string_view predicate) {
    const auto bound =
        host->executor().boundPredicate(pid, principal, std::string(predicate));
    EXPECT_TRUE(bound) << predicate;
    return bound ? policyHash(bound->policy) : std::string(64, '0');
  }

  SignedHeader header(SignedKind kind, SignedClockBasis basis,
                      const std::string &issuer, std::int64_t incarnation = 1,
                      std::int64_t epoch = 1) {
    SignedHeader h;
    h.kind        = kind;
    h.issuer      = issuer;
    h.incarnation = incarnation;
    h.sequence    = ++sequence;
    h.audience    = audience();
    h.epoch       = epoch;
    h.basis       = basis;
    h.sent = basis == SignedClockBasis::Synchronised ? wallNs() : deviceNow();
    return h;
  }

  std::string seal(const SignedHeader &h, const SignedPayload &p,
                   const id::StandardCryptoEngine &signer) const {
    auto envelope = signEnvelope(signer, encodeSignedBody(h, p));
    EXPECT_TRUE(envelope.has_value());
    return encodeSignedEnvelope(*envelope);
  }
  std::string seal(const SignedHeader &h, const SignedPayload &p) const {
    return seal(h, p, device);
  }

  /// A reading of @p predicate (`at`, `seen`, `fuzzy`, ...) acquired at
  /// @p acquired on the device's clock. `at` carries a Float, the rest an Int.
  ObservationPayload payload(const std::string &predicate,
                             const std::string &eventId, std::int64_t acquired,
                             std::int64_t uncertainty = 0) {
    ObservationPayload o;
    o.eventId             = eventId;
    o.predicate           = predicate;
    o.policy              = policyOf(predicate);
    o.acquired            = acquired;
    o.acquiredUncertainty = uncertainty;
    if (predicate == "at") {
      o.tuple    = {std::string("self"), encodeSignedFloat(1.5)};
      o.frame    = "enu";
      o.units    = "m";
      o.variance = 0.25;
    } else {
      o.tuple = {std::string("self"), std::int64_t{3}};
    }
    return o;
  }

  /// A free device's reading, acquired @p ago before it signs.
  std::string freeReading(const std::string &predicate,
                          const std::string &eventId,
                          std::int64_t ago         = 200 * kNanosPerMilli,
                          std::int64_t incarnation = 1,
                          std::int64_t epoch       = 1) {
    auto h = header(SignedKind::Observation, SignedClockBasis::Free, principal,
                    incarnation, epoch);
    return seal(h, payload(predicate, eventId, h.sent - ago));
  }
  /// A reading from a `synchronised` device: its clock is the host's wall.
  std::string syncReading(const std::string &predicate,
                          const std::string &eventId,
                          std::int64_t uncertainty      = 0,
                          std::int64_t clockUncertainty = kNanosPerMilli) {
    auto h = header(SignedKind::Observation, SignedClockBasis::Synchronised,
                    principal);
    h.clockUncertainty = clockUncertainty;
    return seal(h, payload(predicate, eventId, h.sent - 200 * kNanosPerMilli,
                           uncertainty));
  }
  /// The device's reply, signed a moment after the nonce reached it. The
  /// simulated clock moves 1 ms first: a round trip of zero would leave a
  /// mapping with no uncertainty for rounding alone to refuse.
  std::string reply(const ClockChallenge &c, std::int64_t incarnation = 1,
                    std::int64_t epoch = 1) {
    if (simulated) advanceMs(1);
    auto h = header(SignedKind::ClockPair, SignedClockBasis::Free, principal,
                    incarnation, epoch);
    return seal(h, ClockPairPayload{c.nonce});
  }

  std::expected<TransportReceipt, EpistemicRefusal>
  ingest(const std::string &bytes, std::optional<std::string> peer = {}) {
    auto result =
        adapter->ingest(bytes, TransportPeer{peer.value_or(principal)});
    host->pump();
    return result;
  }

  std::optional<IngestOutcome> outcome(std::string eventId,
                                       std::uint64_t incarnation = 1) {
    return host->executor().ingestOutcome(pid, principal, incarnation, eventId);
  }
  static std::string code(const std::optional<IngestOutcome> &o) {
    if (!o) return "absent";
    if (o->kind == IngestOutcome::Kind::Committed) return "Committed";
    return o->code.value_or("pending");
  }

  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(
        host->executor().context(pid)->arena);
  }
  /// The value cell of @p field in the record ingested as @p eventId.
  std::optional<zigzag::CellRef> recordCell(std::string_view eventId,
                                            std::string_view field) {
    const auto &ctx  = *host->executor().context(pid);
    auto &a          = arena();
    const auto dims  = registerDimensions(a);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *host->executor().agentCell(pid);
    for (const auto predicate : zigzag::rankAfter(a, agent, dims.belief))
      for (const auto clause :
           zigzag::rankAfter(a, predicate, ctx.core.dims().clause))
        for (const auto support : zigzag::rankAfter(a, clause, dims.source))
          for (const auto record :
               zigzag::rankAfter(a, support, dims.epistemic)) {
            const auto idv = fieldValue(a, vd, record, kObsEventId);
            if (!idv || a.textOf(*idv) != eventId) continue;
            return fieldValue(a, vd, record, field);
          }
    return std::nullopt;
  }
  double realField(std::string_view eventId, std::string_view field) {
    const auto cell = recordCell(eventId, field);
    const auto v    = cell ? arena().asDouble(*cell) : std::nullopt;
    EXPECT_TRUE(v) << eventId << " " << field;
    return v.value_or(0.0);
  }
  std::int64_t intField(std::string_view eventId, std::string_view field) {
    const auto cell = recordCell(eventId, field);
    const auto v    = cell ? arena().asInt64(*cell) : std::nullopt;
    EXPECT_TRUE(v) << eventId << " " << field;
    return v.value_or(-1);
  }

  /// Pair the free device: the first reading is refused, its challenge is
  /// answered at once. Returns the challenge.
  ClockChallenge pairNow(const std::string &predicate = "seen",
                         const std::string &eventId   = "first",
                         std::int64_t epoch           = 1) {
    const auto before = challenges.size();
    const auto first =
        ingest(freeReading(predicate, eventId, 200 * kNanosPerMilli, 1, epoch));
    EXPECT_FALSE(first);
    EXPECT_EQ(challenges.size(), before + 1);
    const auto c = challenges.back();
    EXPECT_TRUE(ingest(reply(c, 1, epoch)));
    return c;
  }
};

// --- pairing
// ------------------------------------------------------------------

TEST_F(VagentEpistemicPairing, AFreeDevicesFirstReadingStartsAPairing) {
  bind(binding({"seen"}));
  const auto bytes = freeReading("seen", "e1");
  const auto first = ingest(bytes);
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), EpistemicRefusal::UnknownClockMapping);
  EXPECT_EQ(outcomeClassOf("UnknownClockMapping"), OutcomeClass::Retryable);
  // Logged: the executor holds an outcome for it.
  EXPECT_EQ(code(outcome("e1")), "UnknownClockMapping");
  ASSERT_EQ(challenges.size(), 1U);
  EXPECT_EQ(challenges[0].issuer, principal);
  EXPECT_EQ(challenges[0].incarnation, 1);
  EXPECT_EQ(challenges[0].audience, audience());
  EXPECT_EQ(challenges[0].pid, pid);

  // The device answers; the same bytes then commit.
  const auto paired = ingest(reply(challenges[0]));
  ASSERT_TRUE(paired);
  EXPECT_TRUE(paired->eventId.empty());
  const auto checks = adapter->stats().signatureChecks;
  const auto resent = ingest(bytes);
  ASSERT_TRUE(resent) << epistemicRefusalName(resent.error());
  EXPECT_EQ(adapter->stats().signatureChecks, checks + 1);
  EXPECT_EQ(code(outcome("e1")), "Committed");
  EXPECT_EQ(intField("e1", kObsClockMappingVersion), 1);
  EXPECT_EQ(challenges.size(), 1U);
}

TEST_F(VagentEpistemicPairing, ARelayReplayingMessagesCannotStarveAPairing) {
  // S-3a. A settled message first (a synchronised epoch commits), then a
  // free epoch that cannot map.
  bind(binding({"seen"}));
  const auto settled = syncReading("seen", "settled");
  ASSERT_TRUE(ingest(settled));
  ASSERT_EQ(code(outcome("settled")), "Committed");
  const auto waiting =
      freeReading("seen", "waiting", 200 * kNanosPerMilli, 1, 2);
  ASSERT_FALSE(ingest(waiting));
  ASSERT_EQ(challenges.size(), 1U);
  const auto victim = challenges[0];

  const auto checks = adapter->stats().signatureChecks;
  for (int second = 0; second < 3; ++second) {
    advanceMs(500);
    // Refused before its signature: it is settled, so it cannot trigger.
    const auto old = ingest(settled, std::string(64, '9'));
    ASSERT_FALSE(old);
    EXPECT_EQ(old.error(), EpistemicRefusal::Duplicate);
    // An unsettled replay passes steps 1 to 5 and triggers, and is ignored:
    // the pending challenge is never replaced inside its timeout.
    const auto again = ingest(waiting, std::string(64, '9'));
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error(), EpistemicRefusal::UnknownClockMapping);
  }
  EXPECT_GT(adapter->stats().signatureChecks, checks);
  EXPECT_EQ(challenges.size(), 1U);
  // The device's reply, to the nonce it was first given, still pairs.
  ASSERT_TRUE(ingest(reply(victim, 1, 2)));
  ASSERT_TRUE(ingest(waiting));
  EXPECT_EQ(code(outcome("waiting")), "Committed");
}

TEST_F(VagentEpistemicPairing, AnotherSourceEchoingTheNonceIsRefused) {
  // S-3b. The attacker is bound too, under its own key.
  const auto rival = newKey();
  auto second      = binding({"seen"}, "sensor:rival");
  second.principal = issuerOf(rival.publicKey());
  bind(binding({"seen"}));
  bind(second);
  ASSERT_FALSE(ingest(freeReading("seen", "e1")));
  ASSERT_EQ(challenges.size(), 1U);
  const auto victim = challenges[0];

  auto h =
      header(SignedKind::ClockPair, SignedClockBasis::Free, second.principal);
  const auto echoed  = seal(h, ClockPairPayload{victim.nonce}, rival);
  const auto refused = adapter->ingest(echoed, TransportPeer{second.principal});
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::ClockPairUnsolicited);
  EXPECT_EQ(outcomeClassOf("ClockPairUnsolicited"), OutcomeClass::Terminal);
  // The victim's own reply still pairs.
  ASSERT_TRUE(ingest(reply(victim)));
  ASSERT_TRUE(ingest(freeReading("seen", "e2")));
  EXPECT_EQ(code(outcome("e2")), "Committed");
}

TEST_F(VagentEpistemicPairing, ALateAnUnknownAndAConsumedReplyAreRefused) {
  bind(binding({"seen"}));
  // Late: past the timeout.
  ASSERT_FALSE(ingest(freeReading("seen", "e1")));
  const auto late      = challenges.back();
  const auto lateReply = reply(late);
  advanceMs(kDefaultClockPairTimeoutMs + 500);
  auto refused = ingest(lateReply);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::ClockPairUnsolicited);

  // An unknown nonce does not consume the real challenge.
  ASSERT_FALSE(ingest(freeReading("seen", "e2")));
  ASSERT_EQ(challenges.size(), 2U);
  auto wrong  = challenges.back();
  wrong.nonce = {};
  refused     = ingest(reply(wrong));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::ClockPairUnsolicited);
  const auto good = reply(challenges.back());
  ASSERT_TRUE(ingest(good));
  // Consumed: the same bytes again, and a fresh reply with the same nonce.
  refused = ingest(good);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::ClockPairUnsolicited);
  refused = ingest(reply(challenges.back()));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::ClockPairUnsolicited);
  // A reply nobody asked for, from a device with no challenge at all.
  EXPECT_EQ(challenges.size(), 2U);
}

TEST_F(VagentEpistemicPairing, TriggersAreRateLimitedPerIssuer) {
  // A timeout shorter than the interval, so an expired challenge is not what
  // holds the next one back.
  AdapterLimits limits;
  limits.clockPairTimeoutMs = 100;
  adapter                   = makeAdapter(limits);
  bind(binding({"seen"}));
  ASSERT_FALSE(ingest(freeReading("seen", "e1", 200 * kNanosPerMilli, 1, 1)));
  ASSERT_EQ(challenges.size(), 1U);
  advanceMs(300);
  ASSERT_FALSE(ingest(freeReading("seen", "e2", 200 * kNanosPerMilli, 1, 2)));
  EXPECT_EQ(challenges.size(), 1U);
  advanceMs(800);
  ASSERT_FALSE(ingest(freeReading("seen", "e3", 200 * kNanosPerMilli, 1, 2)));
  EXPECT_EQ(challenges.size(), 2U);
  EXPECT_NE(challenges[0].nonce, challenges[1].nonce);
}

TEST_F(VagentEpistemicPairing,
       APendingChallengeIsNeverReplacedInsideItsTimeout) {
  bind(binding({"seen"}));
  ASSERT_FALSE(ingest(freeReading("seen", "e1", 200 * kNanosPerMilli, 1, 1)));
  advanceMs(1500);
  ASSERT_FALSE(ingest(freeReading("seen", "e2", 200 * kNanosPerMilli, 1, 2)));
  EXPECT_EQ(challenges.size(), 1U);
}

TEST_F(VagentEpistemicPairing, AnEntropyFailureStartsNoPairing) {
  bind(binding({"seen"}));
  adapter->setNonceSource([] { return std::optional<ClockNonce>{}; });
  const auto first = ingest(freeReading("seen", "e1"));
  ASSERT_FALSE(first);
  // The triggering reading still gets its own outcome.
  EXPECT_EQ(first.error(), EpistemicRefusal::UnknownClockMapping);
  EXPECT_EQ(code(outcome("e1")), "UnknownClockMapping");
  EXPECT_TRUE(challenges.empty());
  // The failure is not rate limited: the next trigger pairs.
  adapter->setNonceSource({});
  ASSERT_FALSE(ingest(freeReading("seen", "e2")));
  EXPECT_EQ(challenges.size(), 1U);
}

TEST_F(VagentEpistemicPairing, NoSinkMeansNoPairing) {
  bind(binding({"seen"}));
  adapter->setChallengeSink({});
  ASSERT_FALSE(ingest(freeReading("seen", "e1")));
  EXPECT_TRUE(challenges.empty());
}

TEST_F(VagentEpistemicPairing, TheSinkMayCallBackIntoTheAdapter) {
  bind(binding({"seen"}));
  // Re-entrancy: the reply is ingested from inside the sink.
  std::optional<std::expected<TransportReceipt, EpistemicRefusal>> inner;
  adapter->setChallengeSink([&](const ClockChallenge &c) {
    inner = adapter->ingest(reply(c), TransportPeer{principal});
  });
  ASSERT_FALSE(ingest(freeReading("seen", "e1")));
  ASSERT_TRUE(inner);
  EXPECT_TRUE(*inner);
  ASSERT_TRUE(ingest(freeReading("seen", "e2")));
  EXPECT_EQ(code(outcome("e2")), "Committed");
}

// --- the interval and re-pairing ---------------------------------------------

TEST_F(VagentEpistemicPairing, ThePairingIntervalContainsTheTrueOffset) {
  auto b     = binding({"seen"});
  b.driftPpm = 10'000;
  bind(b);
  ASSERT_FALSE(ingest(freeReading("seen", "first")));
  const auto c = challenges.back();
  // The device receives the nonce 0.4 ms after t1 and the host reads the
  // reply 0.6 ms later.
  sim.advance(400'000);
  auto h = header(SignedKind::ClockPair, SignedClockBasis::Free, principal);
  const auto signedAt      = h.sent;
  const auto hostAtSigning = steadyNs();
  sim.advance(600'000);
  ASSERT_TRUE(ingest(seal(h, ClockPairPayload{c.nonce})));

  // A reading acquired at the anchor reads the pairing's own uncertainty:
  // U = u_M(anchor) = u0 on the steady timeline.
  auto o = header(SignedKind::Observation, SignedClockBasis::Free, principal);
  ASSERT_TRUE(ingest(seal(o, payload("seen", "anchor", signedAt))));
  const double mapped = realField("anchor", kObsAcquiredAt);
  const double u      = realField("anchor", kObsAcquiredUncertainty);
  EXPECT_NEAR(u, 0.0005, 1e-9);
  // The device acquired it when the host's steady clock read hostAtSigning.
  const double truth = static_cast<double>(hostAtSigning) / 1e9;
  EXPECT_LE(std::abs(mapped - truth), u);

  // Sixty seconds on, the device clock has drifted 3 ms from the host's, well
  // inside the widened interval, and the mapping asks to be renewed.
  advanceMs(60'000);
  auto later =
      header(SignedKind::Observation, SignedClockBasis::Free, principal);
  const auto acquired = later.sent;
  ASSERT_TRUE(ingest(seal(later, payload("seen", "later", acquired))));
  EXPECT_EQ(intField("later", kObsClockMappingVersion), 1);
  const double mappedLater = realField("later", kObsAcquiredAt);
  const double uLater      = realField("later", kObsAcquiredUncertainty);
  EXPECT_NEAR(uLater,
              0.0005 + 0.01 * (static_cast<double>(acquired - signedAt) / 1e9),
              1e-6);
  EXPECT_LE(std::abs(mappedLater - static_cast<double>(steadyNs()) / 1e9),
            uLater);
  // u_M reached half of max_uncertainty (1 s): a second challenge.
  ASSERT_EQ(challenges.size(), 2U);
  EXPECT_NE(challenges[0].nonce, challenges[1].nonce);
  ASSERT_TRUE(ingest(reply(challenges[1])));
  ASSERT_TRUE(ingest(
      seal(header(SignedKind::Observation, SignedClockBasis::Free, principal),
           payload("seen", "renewed", deviceNow()))));
  EXPECT_EQ(intField("renewed", kObsClockMappingVersion), 2);
  // The renewed mapping is tight again, so no third challenge.
  EXPECT_EQ(challenges.size(), 2U);
}

TEST_F(VagentEpistemicPairing, AWallMappingCarriesTheHostsOwnSyncUncertainty) {
  auto b     = binding({"at"});
  b.driftPpm = 0;
  bind(b);
  ASSERT_FALSE(ingest(freeReading("at", "first")));
  const auto c = challenges.back();
  ASSERT_TRUE(ingest(reply(c)));
  auto h = header(SignedKind::Observation, SignedClockBasis::Free, principal);
  ASSERT_TRUE(
      ingest(seal(h, payload("at", "wall", h.sent - 100 * kNanosPerMilli))));
  EXPECT_EQ(code(outcome("wall")), "Committed");
  // The device's anchor is 100 ms back: u0 = 0.5 ms + 100 ms of wall sync.
  EXPECT_NEAR(realField("wall", kObsAcquiredUncertainty), 0.1005, 1e-6);
}

// --- loopback ---------------------------------------------------------------

class VagentEpistemicPairingLoopback : public VagentEpistemicPairing {
public:
  VagentEpistemicPairingLoopback() { simulated = false; }
};

TEST_F(VagentEpistemicPairingLoopback,
       TheInitialUncertaintyIsUnderAMillisecond) {
  bind(binding({"seen"}));
  // The device answers from inside the sink, the closest a test comes to a
  // transport with no delay: the interval is a signature, a verify and a
  // lookup.
  std::int64_t anchor = 0;
  adapter->setChallengeSink([&](const ClockChallenge &c) {
    auto h = header(SignedKind::ClockPair, SignedClockBasis::Free, principal);
    anchor = h.sent;
    ASSERT_TRUE(adapter->ingest(seal(h, ClockPairPayload{c.nonce}),
                                TransportPeer{principal}));
  });
  ASSERT_FALSE(ingest(freeReading("seen", "first")));
  ASSERT_NE(anchor, 0);
  auto h = header(SignedKind::Observation, SignedClockBasis::Free, principal);
  ASSERT_TRUE(ingest(seal(h, payload("seen", "anchor", anchor))));
  const double u0 = realField("anchor", kObsAcquiredUncertainty);
  std::printf("[measure] pairing u0 on loopback: %.0f us\n", u0 * 1e6);
  EXPECT_GT(u0, 0.0);
  EXPECT_LT(u0, 1e-3);
}

// --- the bind-time check -----------------------------------------------------

TEST_F(VagentEpistemicPairing,
       AWallBoundAtOrBelowTheSyncUncertaintyRefusesAtBind) {
  // wall_sync_uncertainty_ms is 100: `tight` allows exactly that.
  const auto refused = adapter->bind(pid, binding({"at", "tight"}));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::InvalidBinding);
  host->pump();
  for (const auto *predicate : {"at", "tight"})
    EXPECT_FALSE(host->executor().boundPredicate(pid, principal, predicate))
        << predicate;
  // One millisecond of room binds, and so does an actor-mapped predicate.
  EXPECT_TRUE(adapter->bind(pid, binding({"snug", "seen"})));
}

// --- the persisting UncertainClockMapping
// --------------------------------------

TEST_F(VagentEpistemicPairing, APersistingUncertainMappingLogsOncePerStreak) {
  bind(binding({"fuzzy"}));
  LogCapture log;
  const auto window = host->executor().settings().epistemic.commonCauseWindowMs;
  // 0.1 s signed + 0.1 s of wall sync is over fuzzy's 0.15 s.
  const auto attempt = [&](int n) {
    const auto r = ingest(
        syncReading("fuzzy", "u" + std::to_string(n), 100 * kNanosPerMilli, 0));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error(), EpistemicRefusal::UncertainClockMapping);
  };
  attempt(0);
  EXPECT_EQ(log.count("UncertainClockMapping"), 0U);
  advanceMs(window / 2);
  attempt(1);
  EXPECT_EQ(log.count("UncertainClockMapping"), 0U);
  advanceMs(window);
  attempt(2);
  ASSERT_EQ(log.count("UncertainClockMapping"), 1U);
  const auto text = log.text();
  EXPECT_NE(text.find("sensor:gps"), std::string::npos);
  EXPECT_NE(text.find(principal), std::string::npos);
  attempt(3);
  EXPECT_EQ(log.count("UncertainClockMapping"), 1U);
}

// --- step 6 for heartbeats and reports ---------------------------------------

TEST_F(VagentEpistemicPairing, HeartbeatsAreHeldToThePathBound) {
  bind(binding({"seen"}));
  const auto heartbeat = [&](std::int64_t lateMs, std::int64_t expiresInMs) {
    auto h = header(SignedKind::Heartbeat, SignedClockBasis::Synchronised,
                    principal);
    h.sent -= lateMs * kNanosPerMilli;
    h.clockUncertainty = kNanosPerMilli;
    h.expires          = wallNs() + expiresInMs * kNanosPerMilli;
    return ingest(seal(h, HeartbeatPayload{}));
  };
  // Fresh: dispatch is c1's, so it reaches UnsupportedKind.
  auto fresh = heartbeat(50, 5000);
  ASSERT_FALSE(fresh);
  EXPECT_EQ(fresh.error(), EpistemicRefusal::UnsupportedKind);
  // Inside the bound plus the mapping's own uncertainty.
  fresh = heartbeat(5000, 5000);
  ASSERT_FALSE(fresh);
  EXPECT_EQ(fresh.error(), EpistemicRefusal::UnsupportedKind);
  // Beyond max_path_lateness_ms (5 s) plus u_d and the 0.1 s of wall sync.
  auto late = heartbeat(6000, 5000);
  ASSERT_FALSE(late);
  EXPECT_EQ(late.error(), EpistemicRefusal::LateDelivery);
  // Its own `expires` has passed.
  auto expired = heartbeat(50, -1000);
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error(), EpistemicRefusal::MessageExpired);
  // Sent in the future beyond every bound.
  auto future = heartbeat(-5000, 9000);
  ASSERT_FALSE(future);
  EXPECT_EQ(future.error(), EpistemicRefusal::ClockMappingMismatch);
}

TEST_F(VagentEpistemicPairing,
       ADevicesOwnUncertaintyCannotWidenTheFreshnessBound) {
  bind(binding({"seen"}));
  auto h =
      header(SignedKind::Heartbeat, SignedClockBasis::Synchronised, principal);
  h.sent -= 20'000 * kNanosPerMilli;
  // An hour of claimed uncertainty buys at most max_path_lateness more.
  h.clockUncertainty = 3600 * kNanosPerSecond;
  h.expires          = wallNs() + 5 * kNanosPerSecond;
  const auto r       = ingest(seal(h, HeartbeatPayload{}));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), EpistemicRefusal::LateDelivery);
}

TEST_F(VagentEpistemicPairing, AFreeDevicesHeartbeatNeedsAPairingToo) {
  bind(binding({"seen"}));
  auto h    = header(SignedKind::Heartbeat, SignedClockBasis::Free, principal);
  h.expires = h.sent + 5 * kNanosPerSecond;
  auto r    = ingest(seal(h, HeartbeatPayload{}));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), EpistemicRefusal::UnknownClockMapping);
  ASSERT_EQ(challenges.size(), 1U);
  ASSERT_TRUE(ingest(reply(challenges[0])));
  h         = header(SignedKind::Heartbeat, SignedClockBasis::Free, principal);
  h.expires = h.sent + 5 * kNanosPerSecond;
  r         = ingest(seal(h, HeartbeatPayload{}));
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), EpistemicRefusal::UnsupportedKind);
}

// --- incarnations -----------------------------------------------------------

TEST_F(VagentEpistemicPairing, ASignedAdvanceFromAFreeDeviceNeedsAMapping) {
  auto b              = binding({"seen"});
  b.incarnationPolicy = IncarnationPolicy::Signed;
  bind(b);
  const auto bound = [&] {
    return host->executor().boundPredicate(pid, principal, "seen")->incarnation;
  };
  // Incarnation 2 from a free device with no mapping: no advance.
  const auto first = ingest(freeReading("seen", "n1", 200 * kNanosPerMilli, 2));
  ASSERT_FALSE(first);
  EXPECT_EQ(first.error(), EpistemicRefusal::UnboundIncarnation);
  EXPECT_EQ(bound(), 1U);
  // The attempt started a pairing for the incarnation it would advance to.
  ASSERT_EQ(challenges.size(), 1U);
  EXPECT_EQ(challenges[0].incarnation, 2);
  ASSERT_TRUE(ingest(reply(challenges[0], 2)));
  EXPECT_EQ(bound(), 1U);
  // A new reading, now mapped, is a timely message and advances.
  ASSERT_TRUE(ingest(freeReading("seen", "n2", 200 * kNanosPerMilli, 2)));
  EXPECT_EQ(bound(), 2U);
  EXPECT_EQ(code(outcome("n2", 2)), "Committed");
}

// --- the pass-credit bound ---------------------------------------------------

TEST_F(VagentEpistemicPairing, ASlowStreamIsCreditedThroughTheAdapter) {
  bind(binding({"at"}));
  const auto reading = [&](std::int64_t seq) {
    auto h     = header(SignedKind::Observation, SignedClockBasis::Synchronised,
                        principal);
    h.sequence = seq;
    h.clockUncertainty = kNanosPerMilli;
    return seal(h, payload("at", "e" + std::to_string(seq),
                           h.sent - 200 * kNanosPerMilli, 10 * kNanosPerMilli));
  };
  ASSERT_TRUE(ingest(reading(1)));
  ASSERT_EQ(code(outcome("e1")), "Committed");
  // Offered without a pump, so the per-source queue of eight stays full.
  for (std::int64_t seq = 2; seq <= 9; ++seq)
    ASSERT_TRUE(adapter->ingest(reading(seq), TransportPeer{principal})) << seq;
  // A stride of 1,080 is past mark + health_sequence_gap; only the issuer's
  // highest verified sequence covers it.
  const auto dropped = adapter->ingest(reading(1081), TransportPeer{principal});
  ASSERT_FALSE(dropped);
  EXPECT_EQ(dropped.error(), EpistemicRefusal::IngressBackpressure);
  host->pump();
  const EpistemicView view(host->executor(), pid, kPredicates);
  const auto all = view.weights();
  ASSERT_TRUE(all.contains(principal));
  const auto b = healthDecay(32);
  double n     = 0.0;
  for (int i = 0; i < 10; ++i) n = b * n + 1.0;
  EXPECT_NEAR(all.at(principal).n, n, 1e-9);
}

// --- settings ---------------------------------------------------------------

TEST(VagentEpistemicPairingSettings, TheSixSettingsShipWithTheirDefaults) {
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  ASSERT_TRUE(model.isValid()) << model.validationError();
  const auto limits = AdapterLimits::fromModel(model);
  EXPECT_EQ(limits, AdapterLimits{});
  EXPECT_EQ(limits.clockDriftPpm, 100);
  EXPECT_EQ(limits.clockRepairFraction, 0.5);
  EXPECT_EQ(limits.clockPairIntervalMs, 1000);
  EXPECT_EQ(limits.clockPairTimeoutMs, 2000);
  EXPECT_EQ(limits.wallSyncUncertaintyMs, 100);
  EXPECT_EQ(limits.maxPathLatenessMs, 5000);

  auto ver = xanadu::setSetting(
      store, store.primaryCurrentVersion(),
      xanadu::settings::kVagentEpistemicWallSyncUncertaintyMs,
      std::int64_t{250});
  store.repointCurrentVersion(ver);
  ver = xanadu::setSetting(
      store, store.primaryCurrentVersion(),
      xanadu::settings::kVagentEpistemicClockRepairFraction, 0.25);
  store.repointCurrentVersion(ver);
  const auto changed =
      AdapterLimits::fromModel(xanadu::SystemStoreModel::fromStore(store));
  EXPECT_EQ(changed.wallSyncUncertaintyMs, 250);
  EXPECT_EQ(changed.clockRepairFraction, 0.25);

  // One value out of range keeps every default.
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentEpistemicClockPairTimeoutMs,
                           std::int64_t{1});
  store.repointCurrentVersion(ver);
  EXPECT_EQ(
      AdapterLimits::fromModel(xanadu::SystemStoreModel::fromStore(store)),
      AdapterLimits{});
}

TEST(VagentEpistemicPairingSettings, EachRangeIsChecked) {
  const auto bad = [](auto edit) {
    AdapterLimits l;
    edit(l);
    return l.validate();
  };
  EXPECT_EQ(bad([](auto &l) { l.clockDriftPpm = 10'001; }).error(),
            AdapterLimitError::InvalidClockDrift);
  EXPECT_EQ(bad([](auto &l) { l.clockRepairFraction = 0.04; }).error(),
            AdapterLimitError::InvalidClockRepairFraction);
  EXPECT_EQ(bad([](auto &l) { l.clockRepairFraction = std::nan(""); }).error(),
            AdapterLimitError::InvalidClockRepairFraction);
  EXPECT_EQ(bad([](auto &l) { l.clockPairIntervalMs = 99; }).error(),
            AdapterLimitError::InvalidClockPairInterval);
  EXPECT_EQ(bad([](auto &l) { l.clockPairTimeoutMs = 60'001; }).error(),
            AdapterLimitError::InvalidClockPairTimeout);
  EXPECT_EQ(bad([](auto &l) { l.wallSyncUncertaintyMs = -1; }).error(),
            AdapterLimitError::InvalidWallSyncUncertainty);
  EXPECT_EQ(bad([](auto &l) { l.maxPathLatenessMs = 9; }).error(),
            AdapterLimitError::InvalidMaxPathLateness);
}

} // namespace
