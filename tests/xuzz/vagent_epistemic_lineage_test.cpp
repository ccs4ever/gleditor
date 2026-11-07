/**
 * @file vagent_epistemic_lineage_test.cpp
 * @brief P19 I3-3b2: the clock lineage of segments (design 4.4), the host's
 *        detection of a jump in the wall-minus-steady offset and the
 *        invalidation of the live steady-basis mappings (design 4.5).
 *
 * Time is simulated and its two parts move apart: `suspend` moves the wall
 * clock and not the steady one, as a suspend of the host does. A free device
 * reads its own clock as the host's steady time times (1 + 50 ppm) plus a
 * bias, so a pairing has something real to contain. `rtClockRebase` is run
 * through the executor's test surface, which runs the emitted routine in a
 * turn that is rolled back, so every figure below comes from the IR.
 */
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
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
#include "common/xanadu/vagent/runtime/epistemic_abi_clock.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"
#include "common/xanadu/vagent/runtime/epistemic_health.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"
#include "epistemic_invariants.hpp"

namespace {

using namespace xanadu::vagent::runtime;
using namespace xanadu::vagent::runtime::epistemic_test;
namespace id = xanadu::identity;
using xanadu::identity::ActorPid;
using Status = AgentExecutor::ClockRebase::Status;

// Only steady-basis predicates, so nothing but the lineage and the mappings
// can see the jump (N1).
constexpr auto kProgram = R"vagent(
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
/// The default `wall_sync_uncertainty_ms` in seconds, twice: one anchor each.
constexpr double kTwoSigmas = 0.2;

/// The host's two clocks, moved only by the test and apart from each other.
struct SimClock {
  std::int64_t steady{};
  std::int64_t wall{};
  /// @p steadyBias shifts this clock's steady epoch, so a second host's
  /// steady times are not the first's.
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
  /// A suspend of @p nanoseconds: the wall clock runs, the steady one stops.
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

/// The INFO lines `vagent.ingress` logs while a scope lives.
class LogCapture {
public:
  LogCapture()
      : sink_(std::make_shared<spdlog::sinks::ostream_sink_mt>(out_)),
        logger_(gleditor::logging::category("vagent.ingress")),
        previous_(logger_->level()) {
    logger_->sinks().push_back(sink_);
    logger_->set_level(spdlog::level::info);
  }
  ~LogCapture() {
    logger_->sinks().pop_back();
    logger_->set_level(previous_);
  }
  LogCapture(const LogCapture &)            = delete;
  LogCapture &operator=(const LogCapture &) = delete;
  [[nodiscard]] std::size_t count(std::string_view needle) {
    logger_->flush();
    const auto log = out_.str();
    std::size_t n  = 0;
    for (auto at = log.find(needle); at != std::string::npos;
         at      = log.find(needle, at + needle.size()))
      ++n;
    return n;
  }
  [[nodiscard]] std::string text() {
    logger_->flush();
    return out_.str();
  }

private:
  std::ostringstream out_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  spdlog::level::level_enum previous_;
};

/// One segment of the lineage as the arena holds it.
struct Segment {
  std::string instance;
  double start{};
  double anchorSteady{};
  std::optional<double> wall;
  std::optional<double> sync;
  bool operator==(const Segment &) const = default;
};

/// One simulated host: its clock, its executor and one agent with `seen`.
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
  /// Resumed from @p saved on this host's own clocks.
  bool resume(const AgentCheckpoint &saved, bool synchronised = true,
              AgentSettings settings = {}) {
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
    executor().setWallClockSynchronised(synchronised);
    return true;
  }
  AgentExecutor &executor() { return host->executor(); }
  zigzag::ArenaManifold &arena() {
    return const_cast<zigzag::ArenaManifold &>(executor().context(pid)->arena);
  }

  [[nodiscard]] std::vector<Segment> segments() {
    const auto &ctx  = *executor().context(pid);
    auto &a          = arena();
    const auto dims  = registerDimensions(a);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *executor().agentCell(pid);
    std::vector<Segment> out;
    for (const auto cell : zigzag::rankAfter(a, agent, dims.epistemic)) {
      if (a.textOf(cell) != kLineageRecordName) continue;
      const auto list = fieldValue(a, vd, cell, kLineageSegments);
      if (!list) return out;
      for (auto at = a.linked(*list, vd.grab); at;
           at      = a.linked(*at, vd.step)) {
        const auto text = [&](std::string_view f) {
          const auto v = fieldValue(a, vd, *at, f);
          return v ? a.textOf(*v) : std::string{};
        };
        const auto real = [&](std::string_view f) -> std::optional<double> {
          const auto v = fieldValue(a, vd, *at, f);
          return v ? a.asDouble(*v) : std::nullopt;
        };
        out.push_back({text(kSegInstance), real(kSegStartSteady).value_or(-1),
                       real(kSegAnchorSteady).value_or(-1),
                       real(kSegAnchorWall), real(kSegAnchorSync)});
      }
    }
    return out;
  }
  [[nodiscard]] std::size_t lineageRecords() {
    const auto dims  = registerDimensions(arena());
    const auto agent = *executor().agentCell(pid);
    std::size_t n    = 0;
    for (const auto cell : zigzag::rankAfter(arena(), agent, dims.epistemic))
      if (arena().textOf(cell) == kLineageRecordName) ++n;
    return n;
  }
  [[nodiscard]] double steadySeconds() const {
    return static_cast<double>(sim.steady) / 1e9;
  }

  /// Run `rtClockRebase(instance, t)` and read its answer.
  std::optional<AgentExecutor::ClockRebase> rebase(std::string_view instance,
                                                   double t) {
    const auto request = executor().requestClockRebase(pid, instance, t);
    EXPECT_TRUE(request);
    if (!request) return std::nullopt;
    host->pump();
    return executor().clockRebaseResult(*request);
  }
};

class VagentEpistemicLineage : public ::testing::Test {
protected:
  Side a;
  xanadu::MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  std::unique_ptr<EpistemicAdapter> adapter;
  /// A free (paired) device and a synchronised one, two sources of one agent.
  id::StandardCryptoEngine freeKey = newKey();
  id::StandardCryptoEngine syncKey = newKey();
  std::string freeId               = issuerOf(freeKey.publicKey());
  std::string syncId               = issuerOf(syncKey.publicKey());
  std::vector<ClockChallenge> challenges;
  std::int64_t sequence = 0;

  void SetUp() override {
    registry.load();
    a.launch();
    adapter = makeAdapter(a);
  }

  std::unique_ptr<EpistemicAdapter> makeAdapter(Side &side) {
    auto made = std::make_unique<EpistemicAdapter>(
        side.executor(), AdapterLimits{}, registry, masters);
    made->setChallengeSink(
        [this](const ClockChallenge &c) { challenges.push_back(c); });
    return made;
  }

  SourceBinding binding(const std::string &principal, std::string alias) const {
    SourceBinding b;
    b.alias      = std::move(alias);
    b.principal  = principal;
    b.lifetime   = std::chrono::hours(24);
    b.predicates = {"seen"};
    return b;
  }
  void bindBoth(Side &side, EpistemicAdapter &through) {
    ASSERT_TRUE(through.bind(side.pid, binding(freeId, "sensor:free")));
    ASSERT_TRUE(through.bind(side.pid, binding(syncId, "sensor:sync")));
    side.host->pump();
  }

  // --- time -----------------------------------------------------------------

  std::int64_t wallNs(Side &side) { return side.sim.wall; }
  std::int64_t deviceNow(Side &side) {
    return static_cast<std::int64_t>(
               std::llround(static_cast<double>(side.sim.steady) *
                            (1.0 + kSimDriftPpm * 1e-6))) +
           kSimDeviceBias;
  }

  // --- messages -------------------------------------------------------------

  std::string audience(Side &side) {
    const auto v = side.executor().audience(side.pid);
    EXPECT_TRUE(v);
    return v.value_or("");
  }
  std::string policyOf(Side &side, const std::string &principal) {
    const auto bound =
        side.executor().boundPredicate(side.pid, principal, "seen");
    EXPECT_TRUE(bound);
    return bound ? policyHash(bound->policy) : std::string(64, '0');
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
    h.sent        = basis == SignedClockBasis::Synchronised ? wallNs(side)
                                                            : deviceNow(side);
    return h;
  }
  std::string seal(const SignedHeader &h, const SignedPayload &p,
                   const id::StandardCryptoEngine &signer) const {
    auto envelope = signEnvelope(signer, encodeSignedBody(h, p));
    EXPECT_TRUE(envelope.has_value());
    return encodeSignedEnvelope(*envelope);
  }
  ObservationPayload payload(Side &side, const std::string &principal,
                             const std::string &eventId,
                             std::int64_t acquired) {
    ObservationPayload o;
    o.eventId   = eventId;
    o.predicate = "seen";
    o.policy    = policyOf(side, principal);
    o.acquired  = acquired;
    o.tuple     = {std::string("self"), std::int64_t{3}};
    return o;
  }
  /// A free device's reading, acquired 200 ms before it signs.
  std::string freeReading(Side &side, const std::string &eventId) {
    auto h =
        header(side, SignedKind::Observation, SignedClockBasis::Free, freeId);
    return seal(h,
                payload(side, freeId, eventId, h.sent - 200 * kNanosPerMilli),
                freeKey);
  }
  std::string syncReading(Side &side, const std::string &eventId) {
    auto h             = header(side, SignedKind::Observation,
                                SignedClockBasis::Synchronised, syncId);
    h.clockUncertainty = kNanosPerMilli;
    return seal(h,
                payload(side, syncId, eventId, h.sent - 200 * kNanosPerMilli),
                syncKey);
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
  ingestFree(const std::string &bytes) {
    return ingest(a, *adapter, bytes, freeId);
  }
  std::expected<TransportReceipt, EpistemicRefusal>
  ingestSync(const std::string &bytes) {
    return ingest(a, *adapter, bytes, syncId);
  }
  /// The free device's first reading is refused, and its challenge answered.
  void pairFree(const std::string &eventId) {
    const auto before = challenges.size();
    const auto first  = ingestFree(freeReading(a, eventId));
    ASSERT_FALSE(first);
    EXPECT_EQ(first.error(), EpistemicRefusal::UnknownClockMapping);
    ASSERT_EQ(challenges.size(), before + 1);
    ASSERT_TRUE(ingestFree(reply(a, challenges.back())));
  }

  // --- reading the arena ----------------------------------------------------

  std::string code(Side &side, const std::string &principal,
                   const std::string &eventId) {
    const auto o =
        side.executor().ingestOutcome(side.pid, principal, 1, eventId);
    if (!o) return "absent";
    if (o->kind == IngestOutcome::Kind::Committed) return "Committed";
    return o->code.value_or("pending");
  }
  std::optional<zigzag::CellRef>
  recordField(Side &side, std::string_view eventId, std::string_view field) {
    const auto &ctx  = *side.executor().context(side.pid);
    auto &arena      = side.arena();
    const auto dims  = registerDimensions(arena);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *side.executor().agentCell(side.pid);
    for (const auto predicate : zigzag::rankAfter(arena, agent, dims.belief))
      for (const auto clause :
           zigzag::rankAfter(arena, predicate, ctx.core.dims().clause))
        for (const auto support : zigzag::rankAfter(arena, clause, dims.source))
          for (const auto record :
               zigzag::rankAfter(arena, support, dims.epistemic)) {
            const auto idv = fieldValue(arena, vd, record, kObsEventId);
            if (!idv || arena.textOf(*idv) != eventId) continue;
            return fieldValue(arena, vd, record, field);
          }
    return std::nullopt;
  }
  double realField(Side &side, std::string_view eventId,
                   std::string_view field) {
    const auto cell = recordField(side, eventId, field);
    const auto v    = cell ? side.arena().asDouble(*cell) : std::nullopt;
    EXPECT_TRUE(v) << eventId << " " << field;
    return v.value_or(0.0);
  }
  std::int64_t intField(Side &side, std::string_view eventId,
                        std::string_view field) {
    const auto cell = recordField(side, eventId, field);
    const auto v    = cell ? side.arena().asInt64(*cell) : std::nullopt;
    EXPECT_TRUE(v) << eventId << " " << field;
    return v.value_or(-1);
  }
  /// The profile's raw `R` and `N`, as the bits the arena holds.
  std::pair<std::uint64_t, std::uint64_t>
  weights(Side &side, const std::string &principal) {
    const auto &ctx  = *side.executor().context(side.pid);
    auto &arena      = side.arena();
    const auto dims  = registerDimensions(arena);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *side.executor().agentCell(side.pid);
    for (const auto profile : zigzag::rankAfter(arena, agent, dims.trust)) {
      if (arena.textOf(profile) != principal) continue;
      const auto bits = [&](std::string_view field) {
        const auto v = fieldValue(arena, vd, profile, field);
        const auto d = v ? arena.asDouble(*v) : std::nullopt;
        return std::bit_cast<std::uint64_t>(d.value_or(0.0));
      };
      return {bits(kProfileHealthRefusedWeight), bits(kProfileHealthWeight)};
    }
    return {0, 0};
  }
  /// A stored record's time, uncertainty and mapping version, as bits.
  struct Stamp {
    std::uint64_t acquiredAt{};
    std::uint64_t uncertainty{};
    std::int64_t version{};
    bool operator==(const Stamp &) const = default;
  };
  Stamp stamp(Side &side, const std::string &eventId) {
    return {
        std::bit_cast<std::uint64_t>(realField(side, eventId, kObsAcquiredAt)),
        std::bit_cast<std::uint64_t>(
            realField(side, eventId, kObsAcquiredUncertainty)),
        intField(side, eventId, kObsClockMappingVersion)};
  }
};

// --- the segment rule --------------------------------------------------------

TEST_F(VagentEpistemicLineage, TheFirstBindStartsTheInstancesFirstSegment) {
  EXPECT_EQ(a.lineageRecords(), 0U);
  bindBoth(a, *adapter);
  ASSERT_EQ(a.lineageRecords(), 1U);
  const auto segments = a.segments();
  ASSERT_EQ(segments.size(), 1U);
  EXPECT_EQ(segments[0].instance, a.executor().clockInstance());
  // An instance's first segment starts with its steady clock.
  EXPECT_EQ(segments[0].start, 0.0);
  ASSERT_TRUE(segments[0].wall);
  EXPECT_NEAR(*segments[0].wall - segments[0].anchorSteady, a.sim.offset(),
              1e-6);
  EXPECT_EQ(segments[0].sync, 0.1);
  // A second bind of the same instance adds no segment and no record.
  ASSERT_TRUE(adapter->bind(a.pid, binding(freeId, "sensor:free")));
  a.host->pump();
  EXPECT_EQ(a.lineageRecords(), 1U);
  EXPECT_EQ(a.segments().size(), 1U);
}

TEST_F(VagentEpistemicLineage, ACommitMovesTheAnchorInPlace) {
  bindBoth(a, *adapter);
  const auto first = a.segments().front();
  a.sim.advance(5 * kNanosPerSecond);
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  ASSERT_EQ(code(a, syncId, "e1"), "Committed");
  const auto segments = a.segments();
  ASSERT_EQ(segments.size(), 1U);
  // Started where it started, anchored where the commit was.
  EXPECT_EQ(segments[0].start, first.start);
  EXPECT_GT(segments[0].anchorSteady, first.anchorSteady + 4.0);
  EXPECT_NEAR(*segments[0].wall - segments[0].anchorSteady, a.sim.offset(),
              1e-6);
}

TEST_F(VagentEpistemicLineage, AWallStepUnderSynchronisationStartsASegment) {
  bindBoth(a, *adapter);
  LogCapture log;
  a.sim.advance(2 * kNanosPerSecond);
  // The wall clock is stepped forward by NTP: the steady clock does not move.
  a.sim.wall += 5 * kNanosPerSecond;
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  ASSERT_EQ(code(a, syncId, "e1"), "Committed");
  const auto segments = a.segments();
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_NEAR(segments[1].start, a.steadySeconds(), 0.5);
  EXPECT_NEAR(*segments[1].wall - segments[1].anchorSteady, a.sim.offset(),
              1e-6);
  // The jump was found on the host once, and the commit that followed it
  // found the same segment instead of appending a duplicate.
  EXPECT_EQ(log.count("clock generation 1"), 1U);
  ASSERT_TRUE(ingestSync(syncReading(a, "e2")));
  EXPECT_EQ(a.segments().size(), 2U);
}

TEST_F(VagentEpistemicLineage, AStepWithinTheAnchorsUncertaintyIsNotAJump) {
  bindBoth(a, *adapter);
  a.sim.advance(2 * kNanosPerSecond);
  // 150 ms is inside sigma_old + sigma_new = 200 ms.
  a.sim.wall += 150 * kNanosPerMilli;
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  EXPECT_EQ(a.segments().size(), 1U);
}

TEST_F(VagentEpistemicLineage,
       AReadingQueuedBeforeAJumpStartsNoSegmentOfItsOwn) {
  bindBoth(a, *adapter);
  a.sim.advance(kNanosPerSecond);
  // Queued, not run: it carries the pre-jump offset.
  ASSERT_TRUE(adapter->ingest(syncReading(a, "e1"), TransportPeer{syncId}));
  a.sim.suspend(30 * kNanosPerSecond);
  a.host->pump();
  ASSERT_EQ(code(a, syncId, "e1"), "Committed");
  // The control turn noted the jump ahead of the older reading, which then
  // belongs to the segment before it: one segment appended, not two.
  EXPECT_EQ(a.segments().size(), 2U);
}

TEST_F(VagentEpistemicLineage, TheLineageStaysWithinItsBoundAndItsCellCost) {
  bindBoth(a, *adapter);
  // The audience (3 cells) and one anchored segment (3 + 11).
  EXPECT_EQ(*a.executor().epistemicCells(a.pid), 3U + 3U + 11U);
  for (int jump = 0; jump < 12; ++jump) {
    a.sim.advance(kNanosPerSecond);
    a.sim.suspend(30 * kNanosPerSecond);
    a.host->pump();
  }
  // Eight, the default: the oldest were evicted as the newest arrived.
  EXPECT_EQ(a.segments().size(), 8U);
  EXPECT_EQ(*a.executor().epistemicCells(a.pid),
            3U + kLineageBaseCells + 8U * kLineageSegmentCells);
  EXPECT_EQ(kLineageBaseCells + 8U * kLineageSegmentCells, 91U);
  // Newest last, and each segment starts where the one before it ended.
  const auto segments = a.segments();
  for (std::size_t i = 1; i < segments.size(); ++i)
    EXPECT_GT(segments[i].start, segments[i - 1].start);
}

TEST_F(VagentEpistemicLineage, AJumpFoundByAPumpAloneAppendsTheSegment) {
  bindBoth(a, *adapter);
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  ASSERT_EQ(a.segments().size(), 1U);
  a.sim.advance(kNanosPerSecond);
  a.sim.suspend(30 * kNanosPerSecond);
  // No message arrives, no reading is taken: the pump is the detector.
  a.host->pump();
  const auto segments = a.segments();
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_NEAR(segments[1].start, a.steadySeconds(), 1e-6);
  EXPECT_NEAR(segments[1].start - segments[0].anchorSteady, 1.0, 1e-3);
  // Pumping again finds nothing new.
  a.host->pump();
  EXPECT_EQ(a.segments().size(), 2U);
}

TEST_F(VagentEpistemicLineage, NothingIsDetectedWithoutASynchronisedWallClock) {
  // The 4.4 residual, Q10: no wall reading, no offset, no jump.
  a.executor().setWallClockSynchronised(false);
  bindBoth(a, *adapter);
  const auto first = a.segments();
  ASSERT_EQ(first.size(), 1U);
  EXPECT_FALSE(first[0].wall);
  EXPECT_FALSE(first[0].sync);
  a.sim.suspend(30 * kNanosPerSecond);
  a.host->pump();
  EXPECT_EQ(a.segments().size(), 1U);
}

// --- rebasing a record
// ---------------------------------------------------------

TEST_F(VagentEpistemicLineage,
       ASuspendBetweenARecordAndTheRebaseAgesTheRecord) {
  bindBoth(a, *adapter);
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  ASSERT_EQ(code(a, syncId, "e1"), "Committed");
  const double t = realField(a, "e1", kObsAcquiredAt);
  a.sim.advance(3 * kNanosPerSecond);
  a.sim.suspend(30 * kNanosPerSecond);
  a.host->pump();
  ASSERT_EQ(a.segments().size(), 2U);

  const auto rebased = a.rebase(a.executor().clockInstance(), t);
  ASSERT_TRUE(rebased);
  ASSERT_EQ(rebased->status, Status::Rebased);
  // The suspend's length, to the clock's resolution.
  EXPECT_NEAR(rebased->delta, -30.0, 1e-6);
  // Not younger than it really is: 3 s of running and 30 s asleep, plus the
  // 200 ms between the acquisition and the commit.
  const double age = a.steadySeconds() - (t + rebased->delta);
  EXPECT_NEAR(age, 3.0 + 30.0 + 0.2, 0.01);
  // The bound: both sigmas and the steady clock's drift over each distance.
  const double span =
      std::abs(t - a.segments()[0].anchorSteady) +
      std::abs(a.steadySeconds() - a.segments()[1].anchorSteady);
  EXPECT_NEAR(rebased->uncertainty, kTwoSigmas + 200e-6 * span, 1e-6);
  EXPECT_GE(rebased->uncertainty, kTwoSigmas);
}

TEST_F(VagentEpistemicLineage, ARebaseWithinOneSegmentIsZeroWithBothSigmas) {
  bindBoth(a, *adapter);
  const double t = a.steadySeconds();
  a.sim.advance(10 * kNanosPerSecond);
  a.host->pump();
  const auto rebased = a.rebase(a.executor().clockInstance(), t);
  ASSERT_TRUE(rebased);
  ASSERT_EQ(rebased->status, Status::Rebased);
  EXPECT_EQ(rebased->delta, 0.0);
  EXPECT_GE(rebased->uncertainty, kTwoSigmas);
}

TEST_F(VagentEpistemicLineage, ARebaseAcrossAResumeLandsWithinItsBound) {
  bindBoth(a, *adapter);
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  const double t         = realField(a, "e1", kObsAcquiredAt);
  const auto oldInstance = a.executor().clockInstance();
  const double oldOffset = a.sim.offset();
  a.sim.advance(2 * kNanosPerSecond);
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  // The same wall clock, another host whose steady epoch is 1,000 s apart.
  Side b(-1000 * kNanosPerSecond);
  b.sim.wall = a.sim.wall + 4 * kNanosPerSecond;
  ASSERT_TRUE(b.resume(*saved));
  auto second = makeAdapter(b);
  ASSERT_TRUE(second->bind(b.pid, binding(syncId, "sensor:sync")));
  b.host->pump();
  ASSERT_NE(b.executor().clockInstance(), oldInstance);
  const auto segments = b.segments();
  // The old segment is carried bit for bit and the new instance appends its
  // own.
  ASSERT_EQ(segments.size(), 2U);
  EXPECT_EQ(segments[0].instance, oldInstance);
  EXPECT_EQ(segments[1].instance, b.executor().clockInstance());

  const auto rebased = b.rebase(oldInstance, t);
  ASSERT_TRUE(rebased);
  ASSERT_EQ(rebased->status, Status::Rebased);
  // The record's moment in the new host's steady basis is t carried by the
  // difference of the two hosts' wall-minus-steady offsets.
  const double expected = oldOffset - b.sim.offset();
  EXPECT_LE(std::abs(rebased->delta - expected), rebased->uncertainty);
  EXPECT_NEAR(rebased->delta, expected, 1e-6);
  EXPECT_NEAR(rebased->uncertainty, kTwoSigmas, 0.01);
  // The instance the new host named itself is in the lineage too.
  EXPECT_TRUE(b.rebase(b.executor().clockInstance(), b.steadySeconds()));
}

TEST_F(VagentEpistemicLineage, AnUnsynchronisedAnchorFailsTheRebase) {
  Side u;
  u.launch(AgentSettings{}, false);
  auto through = makeAdapter(u);
  ASSERT_TRUE(through->bind(u.pid, binding(syncId, "sensor:sync")));
  u.host->pump();
  ASSERT_EQ(u.segments().size(), 1U);
  const auto rebased =
      u.rebase(u.executor().clockInstance(), u.steadySeconds());
  ASSERT_TRUE(rebased);
  EXPECT_EQ(rebased->status, Status::Unanchored);
  // The anchored one beside it rebases.
  bindBoth(a, *adapter);
  const auto fine = a.rebase(a.executor().clockInstance(), a.steadySeconds());
  ASSERT_TRUE(fine);
  EXPECT_EQ(fine->status, Status::Rebased);
}

TEST_F(VagentEpistemicLineage,
       AnUnanchoredSegmentSplitsOnceTheClockSynchronises) {
  // The stretch before it cannot be compared with what follows.
  a.executor().setWallClockSynchronised(false);
  bindBoth(a, *adapter);
  const double early = a.steadySeconds();
  a.sim.advance(2 * kNanosPerSecond);
  a.executor().setWallClockSynchronised(true);
  ASSERT_TRUE(ingestSync(syncReading(a, "e1")));
  ASSERT_EQ(a.segments().size(), 2U);
  const auto rebased = a.rebase(a.executor().clockInstance(), early);
  ASSERT_TRUE(rebased);
  EXPECT_EQ(rebased->status, Status::Unanchored);
  const auto later = a.rebase(a.executor().clockInstance(), a.steadySeconds());
  ASSERT_TRUE(later);
  EXPECT_EQ(later->status, Status::Rebased);
}

TEST(VagentEpistemicLineageBound, AnEvictedSegmentFailsTheRebase) {
  Side s;
  AgentSettings settings;
  settings.epistemic.clockLineage = 2;
  s.launch(settings);
  xanadu::MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  registry.load();
  auto key  = newKey();
  auto made = std::make_unique<EpistemicAdapter>(s.executor(), AdapterLimits{},
                                                 registry, masters);
  SourceBinding b;
  b.alias      = "sensor:sync";
  b.principal  = issuerOf(key.publicKey());
  b.lifetime   = std::chrono::hours(1);
  b.predicates = {"seen"};
  ASSERT_TRUE(made->bind(s.pid, b));
  s.host->pump();
  const auto instance = s.executor().clockInstance();
  std::vector<double> inside;
  for (int jump = 0; jump < 3; ++jump) {
    s.sim.advance(2 * kNanosPerSecond);
    // A second before the jump, so it is well inside the segment it ends.
    inside.push_back(s.steadySeconds() - 1.0);
    s.sim.suspend(30 * kNanosPerSecond);
    s.host->pump();
  }
  // Four segments were made and the setting keeps the newest two.
  const auto segments = s.segments();
  ASSERT_EQ(segments.size(), 2U);
  ASSERT_TRUE(s.rebase(instance, inside[0]));
  EXPECT_EQ(s.rebase(instance, inside[0])->status, Status::SegmentGone);
  EXPECT_EQ(s.rebase(instance, inside[1])->status, Status::SegmentGone);
  // t in the newest segments still rebases; so does one just after the cut.
  EXPECT_EQ(s.rebase(instance, s.steadySeconds())->status, Status::Rebased);
  EXPECT_EQ(s.rebase(instance, segments[0].start)->status, Status::Rebased);
  // An instance the lineage never held is gone, not a zero.
  EXPECT_EQ(s.rebase("0000000000000000", s.steadySeconds())->status,
            Status::SegmentGone);
}

// --- the host's detection and the invalidation
// -------------------------------------

TEST_F(VagentEpistemicLineage,
       ASuspendOnSteadyBasisPredicatesIsFoundAtTheFirstObserve) {
  LogCapture log;
  bindBoth(a, *adapter);
  ASSERT_TRUE(ingestSync(syncReading(a, "s1")));
  // The free source pairs once, then commits.
  {
    ASSERT_FALSE(ingestFree(freeReading(a, "probe")));
    ASSERT_EQ(challenges.size(), 1U);
    ASSERT_TRUE(ingestFree(reply(a, challenges.back())));
  }
  ASSERT_TRUE(ingestFree(freeReading(a, "p1")));
  ASSERT_EQ(code(a, freeId, "p1"), "Committed");
  ASSERT_EQ(code(a, syncId, "s1"), "Committed");
  // One mapping table entry per device basis, each at its own first version.
  EXPECT_EQ(intField(a, "p1", kObsClockMappingVersion), 1);
  EXPECT_EQ(intField(a, "s1", kObsClockMappingVersion), 1);
  EXPECT_FALSE(adapter->clockPending(a.pid, freeId));

  const auto s1 = stamp(a, "s1");
  const auto p1 = stamp(a, "p1");
  const auto rn = std::pair{weights(a, syncId), weights(a, freeId)};
  // The re-pairing rate limit counts steady time.
  a.sim.advance(2 * kNanosPerSecond);
  a.sim.suspend(30 * kNanosPerSecond);
  ASSERT_EQ(a.segments().size(), 1U);

  // The first observe after the jump, with no pump and no commit between.
  const double signedAt = a.steadySeconds();
  const auto message    = freeReading(a, "p2");
  const auto refused    = ingestFree(message);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::UnknownClockMapping);
  EXPECT_EQ(log.count("clock generation 1"), 1U);
  EXPECT_EQ(log.count("its paired mapping is dropped"), 1U);
  ASSERT_EQ(challenges.size(), 2U);
  const auto pending = adapter->clockPending(a.pid, freeId);
  ASSERT_TRUE(pending);
  EXPECT_EQ(*pending, a.executor().hostTime().steady);
  // The synchronised source has no pending state: its composition was
  // re-minted, not dropped.
  EXPECT_FALSE(adapter->clockPending(a.pid, syncId));
  // A host refusal: R and N do not move, and the segment is in the arena.
  EXPECT_EQ(rn, std::pair(weights(a, syncId), weights(a, freeId)));
  ASSERT_EQ(a.segments().size(), 2U);

  // The device answers once; the same bytes then resume ingestion under the
  // next mapping version, and pending is cleared by the pairing.
  ASSERT_TRUE(ingestFree(reply(a, challenges.back())));
  EXPECT_FALSE(adapter->clockPending(a.pid, freeId));
  ASSERT_TRUE(ingestFree(message));
  ASSERT_EQ(code(a, freeId, "p2"), "Committed");
  EXPECT_EQ(intField(a, "p2", kObsClockMappingVersion), 2);
  EXPECT_NEAR(realField(a, "p2", kObsAcquiredAt), signedAt - 0.2, 0.01);
  EXPECT_EQ(challenges.size(), 2U);

  // The synchronised source's next reading commits under the next version
  // and is placed in the post-jump steady basis, not 30 s ahead of it.
  const double syncedAt = a.steadySeconds();
  ASSERT_TRUE(ingestSync(syncReading(a, "s2")));
  ASSERT_EQ(code(a, syncId, "s2"), "Committed");
  EXPECT_EQ(intField(a, "s2", kObsClockMappingVersion), 2);
  EXPECT_NEAR(realField(a, "s2", kObsAcquiredAt), syncedAt - 0.2, 0.01);

  // Nothing re-mapped what was stored before the jump.
  EXPECT_EQ(stamp(a, "s1"), s1);
  EXPECT_EQ(stamp(a, "p1"), p1);
  // The commit-time comparison found the segment the host had queued.
  EXPECT_EQ(a.segments().size(), 2U);
}

TEST_F(VagentEpistemicLineage, AWallBasisMappingIsLeftAloneByAJump) {
  bindBoth(a, *adapter);
  ASSERT_TRUE(ingestSync(syncReading(a, "s1")));
  a.sim.advance(2 * kNanosPerSecond);
  a.sim.suspend(30 * kNanosPerSecond);
  ASSERT_TRUE(ingestSync(syncReading(a, "s2")));
  // The wall side of the composition keeps its offset (0) and uncertainty: a
  // wall reading never passed through the steady clock. Only the version,
  // which one mint shares between its two timelines, rose.
  EXPECT_NEAR(realField(a, "s2", kObsAcquiredUncertainty),
              realField(a, "s1", kObsAcquiredUncertainty), 1e-9);
  EXPECT_EQ(intField(a, "s1", kObsClockMappingVersion), 1);
  EXPECT_EQ(intField(a, "s2", kObsClockMappingVersion), 2);
}

// --- U-19, settings, registry and translation
// ---------------------------------

TEST_F(VagentEpistemicLineage,
       ALatchedSourceStaysLatchedThroughAResumeAndAPairing) {
  Side u;
  u.launch(AgentSettings{}, false);
  auto first = makeAdapter(u);
  ASSERT_TRUE(first->bind(u.pid, binding(freeId, "sensor:free")));
  u.host->pump();
  ASSERT_FALSE(ingest(u, *first, freeReading(u, "probe"), freeId));
  ASSERT_TRUE(ingest(u, *first, reply(u, challenges.back()), freeId));
  ASSERT_TRUE(ingest(u, *first, freeReading(u, "e1"), freeId));
  ASSERT_EQ(code(u, freeId, "e1"), "Committed");
  // Latch it as an operator's isolate will.
  {
    const auto &ctx  = *u.executor().context(u.pid);
    const auto dims  = registerDimensions(u.arena());
    const auto vd    = valueDims(ctx.core);
    const auto agent = *u.executor().agentCell(u.pid);
    for (const auto profile : zigzag::rankAfter(u.arena(), agent, dims.trust)) {
      const auto faulted = fieldValue(u.arena(), vd, profile, kProfileFaulted);
      ASSERT_TRUE(faulted);
      ASSERT_TRUE(u.arena().setValueBits(*faulted, xanadu::ValueKind::Bool, 1));
    }
  }
  ASSERT_TRUE(u.executor().sourceHealth(u.pid, freeId)->faulted);
  const auto saved = u.executor().checkpoint(u.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  // Resumed with no synchronised wall clock: no segment of the lineage has an
  // anchor, so nothing in it could rebase anything.
  Side v;
  ASSERT_TRUE(v.resume(*saved, false));
  for (const auto &segment : v.segments()) EXPECT_FALSE(segment.wall);
  auto second = makeAdapter(v);
  ASSERT_TRUE(second->bind(v.pid, binding(freeId, "sensor:free")));
  v.host->pump();
  ASSERT_TRUE(v.executor().sourceHealth(v.pid, freeId)->faulted);

  ASSERT_FALSE(ingest(v, *second, freeReading(v, "probe2"), freeId));
  ASSERT_TRUE(ingest(v, *second, reply(v, challenges.back()), freeId));
  for (int i = 0; i < 3; ++i) {
    v.sim.advance(100 * kNanosPerMilli);
    (void)ingest(v, *second, freeReading(v, "r" + std::to_string(i)), freeId);
    EXPECT_NE(code(v, freeId, "r" + std::to_string(i)), "Committed");
  }
  // Still latched, in the arena and in the host's cache.
  const auto health = v.executor().sourceHealth(v.pid, freeId);
  ASSERT_TRUE(health);
  EXPECT_TRUE(health->faulted);
  EXPECT_EQ(health->state, HealthState::Faulted);
}

TEST_F(VagentEpistemicLineage,
       AResumeCarriesTheLineageAndAppendsItsOwnSegment) {
  bindBoth(a, *adapter);
  for (int jump = 0; jump < 2; ++jump) {
    a.sim.advance(2 * kNanosPerSecond);
    a.sim.suspend(30 * kNanosPerSecond);
    a.host->pump();
  }
  const auto before = a.segments();
  ASSERT_EQ(before.size(), 3U);
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());

  Side b(-500 * kNanosPerSecond);
  b.sim.wall = a.sim.wall;
  ASSERT_TRUE(b.resume(*saved));
  // Carried before anything runs: exact doubles, instances as text.
  ASSERT_EQ(b.segments(), before);
  EXPECT_EQ(b.lineageRecords(), 1U);
  auto second = makeAdapter(b);
  ASSERT_TRUE(second->bind(b.pid, binding(syncId, "sensor:sync")));
  b.host->pump();
  const auto after = b.segments();
  ASSERT_EQ(after.size(), 4U);
  EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin()));
  EXPECT_EQ(after.back().instance, b.executor().clockInstance());
  EXPECT_NE(after.back().instance, before.back().instance);
}

TEST_F(VagentEpistemicLineage, AnUpgradeCarriesTheLineageUnchanged) {
  bindBoth(a, *adapter);
  a.sim.advance(2 * kNanosPerSecond);
  a.sim.suspend(30 * kNanosPerSecond);
  a.host->pump();
  const auto before = a.segments();
  ASSERT_EQ(before.size(), 2U);
  auto next = CompiledProgram::fromSource(std::string(kProgram) + "\n");
  ASSERT_TRUE(next) << next.error();
  auto held = a.host->retainProgram(std::move(*next));
  ASSERT_TRUE(held);
  a.host->pump();
  const auto upgraded = a.host->upgrade(a.pid, held);
  ASSERT_TRUE(upgraded) << (upgraded ? "" : toString(upgraded.error()));
  // The same executor, so the same instance: nothing is appended.
  EXPECT_EQ(a.segments(), before);
  EXPECT_EQ(a.lineageRecords(), 1U);
}

TEST_F(VagentEpistemicLineage, AMalformedLineageRefusesAResumeByName) {
  bindBoth(a, *adapter);
  {
    const auto &ctx  = *a.executor().context(a.pid);
    auto &arena      = a.arena();
    const auto dims  = registerDimensions(arena);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *a.executor().agentCell(a.pid);
    for (const auto cell : zigzag::rankAfter(arena, agent, dims.epistemic)) {
      if (arena.textOf(cell) != kLineageRecordName) continue;
      const auto list = fieldValue(arena, vd, cell, kLineageSegments);
      ASSERT_TRUE(list);
      const auto segment = arena.linked(*list, vd.grab);
      ASSERT_TRUE(segment);
      const auto instance = fieldValue(arena, vd, *segment, kSegInstance);
      ASSERT_TRUE(instance);
      zigzag::expectWritten(
          arena.setContent(*instance, std::array{arena.intern("not-an-id")}));
    }
  }
  const auto saved = a.executor().checkpoint(a.pid);
  ASSERT_TRUE(saved) << toString(saved.error());
  Side b;
  auto program = CompiledProgram::fromSource(kProgram);
  ASSERT_TRUE(program);
  b.host             = std::make_unique<LocalHost>(std::move(*program));
  const auto resumed = b.executor().resume(*saved);
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().code, TransferCode::StateInconsistent);
  EXPECT_EQ(resumed.error().detail.subject, kLineageRecordName);
}

TEST_F(VagentEpistemicLineage, TheLineageIsRegisteredAsCarriedState) {
  const auto entries = allOwnedStructures();
  const auto found = std::ranges::find_if(entries, [](const OwnedStructure &e) {
    return e.id == "agent.clock_lineage";
  });
  ASSERT_NE(found, entries.end());
  EXPECT_EQ(found->resume, Fate::Carried);
  EXPECT_EQ(found->upgrade, Fate::Carried);
  EXPECT_EQ(found->dimension, "d.epistemic");
  EXPECT_EQ(found->where, "agent rank");
}

TEST(VagentEpistemicLineageSettings, TheTwoSettingsShipWithTheirDefaults) {
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  ASSERT_TRUE(model.isValid()) << model.validationError();
  const auto settings = AgentSettings::fromModel(model);
  EXPECT_EQ(settings.epistemic, EpistemicLimits{});
  EXPECT_EQ(settings.epistemic.clockLineage, 8);
  EXPECT_EQ(settings.epistemic.steadyDriftPpm, 200);
  EXPECT_EQ(settings.epistemic.wallSyncUncertaintyMs, 100);

  auto ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                                xanadu::settings::kVagentEpistemicClockLineage,
                                std::int64_t{3});
  store.repointCurrentVersion(ver);
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentEpistemicSteadyDriftPpm,
                           std::int64_t{0});
  store.repointCurrentVersion(ver);
  const auto changed =
      AgentSettings::fromModel(xanadu::SystemStoreModel::fromStore(store));
  EXPECT_EQ(changed.epistemic.clockLineage, 3);
  EXPECT_EQ(changed.epistemic.steadyDriftPpm, 0);

  // One value out of its range keeps every default.
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentEpistemicClockLineage,
                           std::int64_t{1});
  store.repointCurrentVersion(ver);
  EXPECT_EQ(AgentSettings::fromModel(xanadu::SystemStoreModel::fromStore(store))
                .epistemic,
            EpistemicLimits{});
}

} // namespace
