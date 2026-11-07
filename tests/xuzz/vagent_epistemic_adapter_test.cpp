/**
 * @file vagent_epistemic_adapter_test.cpp
 * @brief The signed-ingress adapter end to end (P19 I3-3a2): signed bytes
 *        from a transport peer into an agent's arbitrated predicate.
 *
 * The audience, minted once from the CSPRNG and carried as text through a
 * checkpoint; replays to another actor or predicate; relay marking from the
 * transport only; bindings the adapter refuses; signed incarnation advances
 * and the step bound; a2's clock mapping; and the step-4 memory that settled
 * outcomes feed.
 */
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/system_docs.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"
#include "common/xanadu/vagent/runtime/epistemic_translate_registry.hpp"
#include "common/xanadu/vagent/runtime/program.hpp"
#include "common/xanadu/vagent/runtime/runtime_abi.hpp"
#include "common/xanadu/zigzag/cell_views.hpp"

namespace {

using namespace xanadu::vagent::runtime;
namespace id = xanadu::identity;
using xanadu::identity::ActorPid;
using zigzag::CellRef;

constexpr auto kProgram = R"vagent(
predicate at(key who: Str, x: Float)
  arbitrated by truest(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 1, prior_beta: 1, clock: "wall", max_age: 10s,
    cluster_tolerance: 0.5, frame: "enu", units: "m",
    min_variance: 0.0001, max_variance: 100.0)
predicate door(key id: Str, open: Bool)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "wall", max_age: 10s)
predicate odo(key id: Str, n: Int)
  arbitrated by ranked(model: "beta_reputation", min_confidence: 0.5,
    prior_alpha: 2, prior_beta: 2, clock: "actor_mapped", max_age: 10s)
event Quit { }
agent A {
  var adds: Int := 0
  on Quit { terminate }
  on +at(?w, ?x) { adds := adds + 1 }
}
)vagent";

std::int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

id::StandardCryptoEngine newKey() {
  auto engine = id::StandardCryptoEngine::generate();
  EXPECT_TRUE(engine.has_value());
  return std::move(*engine);
}

/// One agent on its own host, with the ledger, registry and master lookup an
/// adapter needs, and a pinned device.
struct Side {
  /// Shared when two agents sit behind one executor, and so one adapter.
  std::shared_ptr<LocalHost> host;
  ActorPid pid;
  AgentExecutor &executor() { return host->executor(); }

  void launch(std::string_view name = "a") {
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program) << program.error();
    host = std::make_shared<LocalHost>(std::move(*program));
    start(name);
  }
  void launchBeside(const Side &other, std::string_view name) {
    host = other.host;
    start(name);
  }

private:
  void start(std::string_view name) {
    const auto launched = host->launch("A", name);
    ASSERT_TRUE(launched);
    pid = *launched;
    host->pump();
    executor().setWallClockSynchronised(true);
  }
};

class VagentEpistemicAdapter : public ::testing::Test {
protected:
  Side a;
  xanadu::MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  std::unique_ptr<EpistemicAdapter> adapter;
  id::StandardCryptoEngine device = newKey();
  std::string principal           = issuerOf(device.publicKey());
  std::vector<IngressAudit> audits;

  void SetUp() override {
    registry.load();
    a.launch();
    adapter = makeAdapter(a.executor());
  }

  std::unique_ptr<EpistemicAdapter> makeAdapter(AgentExecutor &executor,
                                                AdapterLimits limits = {}) {
    auto made =
        std::make_unique<EpistemicAdapter>(executor, limits, registry, masters);
    made->setAuditSink(
        [this](const IngressAudit &event) { audits.push_back(event); });
    return made;
  }

  SourceBinding binding(std::vector<std::string> predicates = {"at"},
                        IncarnationPolicy policy = IncarnationPolicy::Fixed,
                        std::string alias        = "sensor:gps") const {
    SourceBinding b;
    b.alias             = std::move(alias);
    b.principal         = principal;
    b.incarnationPolicy = policy;
    b.lifetime          = std::chrono::hours(1);
    b.predicates        = std::move(predicates);
    return b;
  }

  /// Bind and run the source-change turn, so the audience commits.
  void bind(Side &side, SourceBinding b, EpistemicAdapter *through = nullptr) {
    auto *on    = through != nullptr ? through : adapter.get();
    auto result = on->bind(side.pid, std::move(b));
    ASSERT_TRUE(result) << epistemicRefusalName(result.error());
    side.host->pump();
  }

  std::string audienceOf(Side &side) {
    const auto audience = side.executor().audience(side.pid);
    EXPECT_TRUE(audience);
    return audience.value_or("");
  }

  std::string policyOf(Side &side, std::string_view predicate) {
    const auto bound = side.executor().boundPredicate(side.pid, principal,
                                                      std::string(predicate));
    EXPECT_TRUE(bound) << predicate;
    return bound ? policyHash(bound->policy) : std::string(64, '0');
  }

  SignedHeader header(std::string audience, std::int64_t sequence,
                      std::int64_t incarnation = 1) const {
    SignedHeader h;
    h.kind             = SignedKind::Observation;
    h.issuer           = principal;
    h.incarnation      = incarnation;
    h.sequence         = sequence;
    h.audience         = std::move(audience);
    h.sent             = nowNs() - 500'000'000;
    h.epoch            = 0;
    h.basis            = SignedClockBasis::Synchronised;
    h.clockUncertainty = 1'000'000;
    return h;
  }

  ObservationPayload at(std::string eventId, std::string policy,
                        double x = 1.5) const {
    ObservationPayload o;
    o.eventId             = std::move(eventId);
    o.predicate           = "at";
    o.policy              = std::move(policy);
    o.tuple               = {std::string("self"), encodeSignedFloat(x)};
    o.acquired            = nowNs() - 1'000'000'000;
    o.acquiredUncertainty = 10'000'000;
    o.frame               = "enu";
    o.units               = "m";
    o.variance            = 0.25;
    return o;
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

  /// An observation on `at` for @p side, sequence @p sequence.
  std::string reading(Side &side, std::int64_t sequence,
                      std::int64_t incarnation = 1) {
    return seal(header(audienceOf(side), sequence, incarnation),
                at("e" + std::to_string(sequence), policyOf(side, "at")));
  }

  std::expected<TransportReceipt, EpistemicRefusal>
  ingest(Side &side, const std::string &bytes,
         std::optional<std::string> peer = std::nullopt,
         EpistemicAdapter *through       = nullptr) {
    auto *on    = through != nullptr ? through : adapter.get();
    auto result = on->ingest(bytes, TransportPeer{peer.value_or(principal)});
    side.host->pump();
    return result;
  }

  std::optional<IngestOutcome> outcome(Side &side, std::string eventId,
                                       std::uint64_t incarnation = 1) {
    return side.executor().ingestOutcome(side.pid, principal, incarnation,
                                         eventId);
  }
  static std::string code(const std::optional<IngestOutcome> &o) {
    if (!o) return "absent";
    if (o->kind == IngestOutcome::Kind::Committed) return "Committed";
    return o->code.value_or("pending");
  }

  /// A text field of the record ingested as @p eventId, if any.
  std::optional<std::string> recordField(Side &side, std::string_view eventId,
                                         std::string_view field) {
    const auto &ctx  = *side.executor().context(side.pid);
    auto &arena      = const_cast<zigzag::ArenaManifold &>(ctx.arena);
    const auto dims  = registerDimensions(arena);
    const auto vd    = valueDims(ctx.core);
    const auto agent = *side.executor().agentCell(side.pid);
    for (const auto predicate : zigzag::rankAfter(arena, agent, dims.belief))
      for (const auto clause :
           zigzag::rankAfter(arena, predicate, ctx.core.dims().clause))
        for (const auto support : zigzag::rankAfter(arena, clause, dims.source))
          for (const auto record :
               zigzag::rankAfter(arena, support, dims.epistemic)) {
            const auto id = fieldValue(arena, vd, record, kObsEventId);
            if (!id || arena.textOf(*id) != eventId) continue;
            const auto value = fieldValue(arena, vd, record, field);
            return value ? std::optional{arena.textOf(*value)} : std::nullopt;
          }
    return std::nullopt;
  }
};

// --- the audience -----------------------------------------------------------

TEST_F(VagentEpistemicAdapter, AVerifiedObservationIsCommittedUnderItsSource) {
  bind(a, binding());
  const auto audience = audienceOf(a);
  EXPECT_EQ(audience.size(), 64U);
  ASSERT_TRUE(ingest(a, reading(a, 1)));
  EXPECT_EQ(code(outcome(a, "e1")), "Committed");
  // A direct copy: the transport peer is the issuer.
  EXPECT_FALSE(recordField(a, "e1", kObsRelayedBy));
  const auto health = a.executor().sourceHealth(a.pid, principal);
  ASSERT_TRUE(health);
  EXPECT_FALSE(health->faulted);
}

TEST_F(VagentEpistemicAdapter, TheAudienceIsUnavailableUntilTheBindCommits) {
  // Bound, but the source-change turn that writes the audience has not run:
  // the agent may be the one a message means, so it is retryable.
  ASSERT_TRUE(adapter->bind(a.pid, binding()));
  EXPECT_FALSE(a.executor().audience(a.pid));
  const auto early = adapter->ingest(
      seal(header(std::string(64, 'f'), 1), at("e1", std::string(64, 'c'))),
      TransportPeer{principal});
  ASSERT_FALSE(early);
  EXPECT_EQ(early.error(), EpistemicRefusal::AudienceUnavailable);
  EXPECT_EQ(outcomeClassOf(epistemicRefusalName(early.error())),
            OutcomeClass::Retryable);
  // Once it commits, readings signed for it land.
  a.host->pump();
  ASSERT_TRUE(ingest(a, reading(a, 1)));
  EXPECT_EQ(code(outcome(a, "e1")), "Committed");
}

TEST_F(VagentEpistemicAdapter, AnUnknownAudienceIsAMismatch) {
  bind(a, binding());
  const auto refused = ingest(
      a, seal(header(std::string(64, 'f'), 1), at("e1", policyOf(a, "at"))));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::AudienceMismatch);
  EXPECT_EQ(code(outcome(a, "e1")), "absent");
}

TEST_F(VagentEpistemicAdapter, AReadingForOneActorNeverReachesAnother) {
  // The same device bound to two agents: its bytes name one audience.
  Side b;
  b.launchBeside(a, "b");
  bind(a, binding());
  bind(b, binding());
  ASSERT_NE(audienceOf(a), audienceOf(b));
  ASSERT_TRUE(ingest(a, reading(a, 1)));
  b.host->pump();
  EXPECT_EQ(code(outcome(a, "e1")), "Committed");
  EXPECT_EQ(code(outcome(b, "e1")), "absent");
  // Signed for B but bound only on A: B's audience resolves, the issuer is
  // not B's source.
  adapter->unbind(b.pid, principal);
  const auto other = newKey();
  auto ob          = binding({"at"}, IncarnationPolicy::Fixed, "sensor:o");
  ob.principal     = issuerOf(other.publicKey());
  bind(b, ob);
  // One program, so `a`'s binding gives the policy hash `b` declares too.
  const auto refused =
      ingest(b, seal(header(audienceOf(b), 2), at("e2", policyOf(a, "at"))));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::UnknownSource);
}

TEST_F(VagentEpistemicAdapter, AnotherPredicatesPolicyRefusesPolicyMismatch) {
  bind(a, binding({"at", "door"}));
  // `at`'s reading signed with `door`'s policy hash: the declaration the
  // device was configured for is not this one.
  const auto bytes =
      seal(header(audienceOf(a), 1), at("e1", policyOf(a, "door")));
  const auto refused = ingest(a, bytes);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::PolicyMismatch);
  EXPECT_EQ(code(outcome(a, "e1")), "PolicyMismatch");
  EXPECT_EQ(outcomeClassOf("PolicyMismatch"), OutcomeClass::NeedsHostAction);
  // A predicate the source is not bound on is not its to report.
  auto unbound      = at("e2", policyOf(a, "at"));
  unbound.predicate = "odo";
  const auto other  = ingest(a, seal(header(audienceOf(a), 2), unbound));
  ASSERT_FALSE(other);
  EXPECT_EQ(other.error(), EpistemicRefusal::UnknownSource);
}

// --- relays and bindings ----------------------------------------------------

TEST_F(VagentEpistemicAdapter, ARelayIsMarkedFromTheTransportOnly) {
  bind(a, binding());
  const std::string relay(64, '9');
  ASSERT_TRUE(ingest(a, reading(a, 1), relay));
  EXPECT_EQ(code(outcome(a, "e1")), "Committed");
  EXPECT_EQ(recordField(a, "e1", kObsRelayedBy), relay);
  // The relay cannot claim to be the origin by signing as itself: its key is
  // not the issuer's.
  const auto relayKey = newKey();
  const auto forged =
      seal(header(audienceOf(a), 2), at("e2", policyOf(a, "at")), relayKey);
  const auto refused = ingest(a, forged, issuerOf(relayKey.publicKey()));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::IssuerKeyMismatch);
}

TEST_F(VagentEpistemicAdapter, AnAliasBoundToTwoPrincipalsRefuses) {
  bind(a, binding());
  const auto other   = newKey();
  auto second        = binding();
  second.principal   = issuerOf(other.publicKey());
  const auto refused = adapter->bind(a.pid, second);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::AliasInUse);
}

TEST_F(VagentEpistemicAdapter, ABindingTheAdapterCannotHoldRefuses) {
  auto upper = binding();
  for (auto &c : upper.principal)
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  ASSERT_NE(upper.principal, principal);
  auto shortName = binding();
  shortName.principal.pop_back();
  auto lookalikeMaster  = binding();
  lookalikeMaster.trust = DelegatedTrust{std::string(64, 'A')};
  auto none             = binding({});
  auto twice            = binding({"at", "at"});
  auto forever          = binding();
  forever.lifetime      = std::chrono::steady_clock::duration::zero();
  auto zero             = binding();
  zero.incarnation      = 0;
  for (const auto &bad :
       {upper, shortName, lookalikeMaster, none, twice, forever, zero}) {
    const auto refused = adapter->bind(a.pid, bad);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), EpistemicRefusal::InvalidBinding);
  }
  EXPECT_FALSE(a.executor().boundPredicate(a.pid, principal, "at"));
}

TEST_F(VagentEpistemicAdapter, AFailedBindLeavesNoPredicateBound) {
  // The principal's `door` binds, then its alias collides with another
  // principal's on `at`: the adapter unbinds `door` again.
  const auto other = newKey();
  auto first       = binding({"at"}, IncarnationPolicy::Fixed, "sensor:x");
  first.principal  = issuerOf(other.publicKey());
  ASSERT_TRUE(adapter->bind(a.pid, first));
  auto mine = binding({"door", "at"}, IncarnationPolicy::Fixed, "sensor:x");
  const auto refused = adapter->bind(a.pid, mine);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::AliasInUse);
  EXPECT_FALSE(a.executor().boundPredicate(a.pid, principal, "door"));
}

// --- incarnations -----------------------------------------------------------

TEST_F(VagentEpistemicAdapter, ASignedAdvanceRebindsEveryPredicateOnce) {
  bind(a, binding({"at", "door"}, IncarnationPolicy::Signed));
  ASSERT_TRUE(ingest(a, reading(a, 1)));
  ASSERT_TRUE(ingest(a, reading(a, 2, 2)));
  EXPECT_EQ(code(outcome(a, "e2", 2)), "Committed");
  for (const auto *predicate : {"at", "door"}) {
    const auto bound = a.executor().boundPredicate(a.pid, principal, predicate);
    ASSERT_TRUE(bound) << predicate;
    EXPECT_EQ(bound->incarnation, 2U) << predicate;
  }
  ASSERT_EQ(audits.size(), 1U);
  EXPECT_EQ(audits[0].kind, IngressAudit::Kind::IncarnationAdvanced);
  EXPECT_EQ(audits[0].principal, principal);
  EXPECT_EQ(audits[0].from, 1U);
  EXPECT_EQ(audits[0].to, 2U);
  // The arena moved with it: the profile is at the new incarnation.
  EXPECT_EQ(a.executor().sourceHealth(a.pid, principal)->incarnation, 2U);
}

TEST_F(VagentEpistemicAdapter, AFixedBindingIsNotAdvancedByAMessage) {
  bind(a, binding({"at"}, IncarnationPolicy::Fixed));
  const auto refused = ingest(a, reading(a, 1, 2));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::UnboundIncarnation);
  EXPECT_EQ(a.executor().boundPredicate(a.pid, principal, "at")->incarnation,
            1U);
  EXPECT_TRUE(audits.empty());
}

TEST_F(VagentEpistemicAdapter, ACapturedOldHigherIncarnationIsNotAnAdvance) {
  bind(a, binding({"at"}, IncarnationPolicy::Signed));
  // Signed a minute ago and held back: past max_age on arrival, so it cannot
  // move the binding even though its signature is good.
  auto h             = header(audienceOf(a), 1, 2);
  auto o             = at("e1", policyOf(a, "at"));
  h.sent             = nowNs() - 59'000'000'000;
  o.acquired         = nowNs() - 60'000'000'000;
  const auto refused = ingest(a, seal(h, o));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::UnboundIncarnation);
  EXPECT_EQ(a.executor().boundPredicate(a.pid, principal, "at")->incarnation,
            1U);
  EXPECT_TRUE(audits.empty());
}

TEST_F(VagentEpistemicAdapter, AnAdvancePastTheStepRefusesIncarnationStep) {
  bind(a, binding({"at"}, IncarnationPolicy::Signed));
  const auto far     = 1 + kDefaultIncarnationStep + 1;
  const auto refused = ingest(a, reading(a, 1, far));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::IncarnationStep);
  EXPECT_EQ(code(outcome(a, "e1", static_cast<std::uint64_t>(far))),
            "IncarnationStep");
  // Within the bound, the next one advances.
  ASSERT_TRUE(ingest(a, reading(a, 2, 1 + kDefaultIncarnationStep)));
  EXPECT_EQ(a.executor().boundPredicate(a.pid, principal, "at")->incarnation,
            static_cast<std::uint64_t>(1 + kDefaultIncarnationStep));
}

// --- a2's clock mapping and the tuple --------------------------------------

TEST_F(VagentEpistemicAdapter, ASynchronisedClockMapsOnBothTimelines) {
  bind(a, binding({"at", "odo"}));
  auto h             = header(audienceOf(a), 1);
  h.basis            = SignedClockBasis::Free;
  h.clockUncertainty = 0;
  const auto free    = ingest(a, seal(h, at("e1", policyOf(a, "at"))));
  ASSERT_FALSE(free);
  EXPECT_EQ(free.error(), EpistemicRefusal::UnknownClockMapping);
  EXPECT_EQ(outcomeClassOf("UnknownClockMapping"), OutcomeClass::Retryable);
  // A synchronised device needs no pairing on an actor-mapped predicate
  // either: the host composes its own steady-to-wall offset (I3-3b1 part B).
  ObservationPayload odo = at("e2", policyOf(a, "odo"));
  odo.predicate          = "odo";
  odo.tuple              = {std::string("left"), std::int64_t{3}};
  odo.frame.clear();
  odo.units.clear();
  odo.variance.reset();
  ASSERT_TRUE(ingest(a, seal(header(audienceOf(a), 2), odo)));
  EXPECT_EQ(code(outcome(a, "e2")), "Committed");
  // Retryable, so not noted: the same bytes are verified again, not refused
  // as seen.
  const auto checks = adapter->stats().signatureChecks;
  ASSERT_FALSE(ingest(a, seal(h, at("e1", policyOf(a, "at")))));
  EXPECT_EQ(adapter->stats().signatureChecks, checks + 1);
}

TEST_F(VagentEpistemicAdapter, TheTupleIsTypedByTheDeclaration) {
  bind(a, binding({"at", "door"}));
  // A Float sent as an integer: the device's own bytes, refused and logged.
  auto o              = at("e1", policyOf(a, "at"));
  o.tuple[1]          = std::int64_t{2};
  const auto mistyped = ingest(a, seal(header(audienceOf(a), 1), o));
  ASSERT_FALSE(mistyped);
  EXPECT_EQ(mistyped.error(), EpistemicRefusal::InvalidValue);
  EXPECT_EQ(code(outcome(a, "e1")), "InvalidValue");
  // A Bool is 0 or 1.
  ObservationPayload door = at("e2", policyOf(a, "door"));
  door.predicate          = "door";
  door.frame.clear();
  door.units.clear();
  door.variance.reset();
  door.tuple = {std::string("front"), std::int64_t{1}};
  ASSERT_TRUE(ingest(a, seal(header(audienceOf(a), 2), door)));
  EXPECT_EQ(code(outcome(a, "e2")), "Committed");
  door.eventId   = "e3";
  door.tuple[1]  = std::int64_t{2};
  const auto two = ingest(a, seal(header(audienceOf(a), 3), door));
  ASSERT_FALSE(two);
  EXPECT_EQ(two.error(), EpistemicRefusal::InvalidValue);
  // The wrong arity is the request's shape.
  door.eventId     = "e4";
  door.tuple       = {std::string("front")};
  const auto arity = ingest(a, seal(header(audienceOf(a), 4), door));
  ASSERT_FALSE(arity);
  EXPECT_EQ(arity.error(), EpistemicRefusal::InvalidObservation);
}

TEST_F(VagentEpistemicAdapter, ASettledReadingIsRefusedBeforeItsSignature) {
  bind(a, binding());
  const auto bytes = reading(a, 1);
  ASSERT_TRUE(ingest(a, bytes));
  ASSERT_EQ(code(outcome(a, "e1")), "Committed");
  const auto checks  = adapter->stats().signatureChecks;
  const auto refused = ingest(a, bytes);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::Duplicate);
  EXPECT_EQ(adapter->stats().signatureChecks, checks);
}

TEST_F(VagentEpistemicAdapter, OtherKindsAreVerifiedButNotYetDispatched) {
  bind(a, binding());
  auto h             = header(audienceOf(a), 1);
  h.kind             = SignedKind::Heartbeat;
  h.expires          = nowNs() + 5'000'000'000;
  const auto refused = ingest(a, seal(h, HeartbeatPayload{}));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::UnsupportedKind);
}

TEST_F(VagentEpistemicAdapter, TheFirstRevokedKeyIsAuditedOnce) {
  const auto master = newKey();
  xanadu::IdentityKeyLink link;
  link.fingerprint               = issuerOf(master.publicKey());
  link.identity                  = "Sensor Owner";
  link.email                     = "owner@test.invalid";
  static constexpr char digits[] = "0123456789abcdef";
  for (const auto b : master.publicKey().bytes) {
    link.publicKeyArmored.push_back(digits[b >> 4U]);
    link.publicKeyArmored.push_back(digits[b & 0xFU]);
  }
  link.timestamp = 1;
  ledger.appendKey(link);
  bind(a, binding());
  id::DeviceRevocationRecord r;
  r.devicePublicKey   = device.publicKey();
  r.masterFingerprint = *id::Fingerprint::fromString(
      id::StandardCryptoEngine::computeFingerprint(master.publicKey()));
  r.revocationDate  = 1;
  r.reason          = id::RevocationReason::KeyCompromise;
  const auto buffer = r.signingBuffer();
  r.masterSignature = *master.sign(
      {reinterpret_cast<const std::uint8_t *>(buffer.data()), buffer.size()});
  ASSERT_TRUE(registry.admit(r));
  for (std::int64_t seq = 1; seq <= 2; ++seq) {
    const auto refused = ingest(a, reading(a, seq));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error(), EpistemicRefusal::KeyRevoked);
  }
  ASSERT_EQ(audits.size(), 1U);
  EXPECT_EQ(audits[0].kind, IngressAudit::Kind::KeyRevoked);
  EXPECT_EQ(audits[0].principal, principal);
}

// --- minting and carrying the audience -------------------------------------

TEST_F(VagentEpistemicAdapter, AnEntropyFailureRefusesTheBind) {
  a.executor().setAudienceMint([] { return std::nullopt; });
  const auto refused = adapter->bind(a.pid, binding({"at", "door"}));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), EpistemicRefusal::EntropyUnavailable);
  EXPECT_FALSE(a.executor().boundPredicate(a.pid, principal, "at"));
  EXPECT_FALSE(a.executor().audience(a.pid));
}

TEST_F(VagentEpistemicAdapter, ANumberLikeAudienceSurvivesACheckpointAsText) {
  // A checkpoint types text that reads as a number, so each of these must
  // come back with its own spelling, compared as text.
  const std::vector<std::string> tokens{
      std::string(64, '1'),
      std::string(11, '0') + "e1234" + std::string(48, '5'),
      "0" + std::string(63, '7'),
  };
  for (const auto &token : tokens) {
    ASSERT_EQ(token.size(), 64U);
    Side from;
    from.launch();
    from.executor().setAudienceMint([&] { return token; });
    auto first = makeAdapter(from.executor());
    bind(from, binding(), first.get());
    ASSERT_EQ(audienceOf(from), token);
    auto saved = from.executor().checkpoint(from.pid);
    ASSERT_TRUE(saved) << toString(saved.error());

    Side to;
    auto program = CompiledProgram::fromSource(kProgram);
    ASSERT_TRUE(program);
    to.host = std::make_shared<LocalHost>(std::move(*program));
    // The new host would mint another; the arena's identifier wins.
    to.executor().setAudienceMint([] { return std::string(64, 'b'); });
    const auto resumed = to.executor().resume(*saved);
    ASSERT_TRUE(resumed) << toString(resumed.error());
    to.pid = *resumed;
    to.host->pump();
    to.executor().setWallClockSynchronised(true);
    auto second = makeAdapter(to.executor());
    bind(to, binding(), second.get());
    EXPECT_EQ(to.executor().audience(to.pid), token) << token;
    // A device configured before the move still reaches the agent.
    ASSERT_TRUE(ingest(to, reading(to, 1), std::nullopt, second.get()))
        << token;
    EXPECT_EQ(code(outcome(to, "e1")), "Committed") << token;
  }
}

TEST_F(VagentEpistemicAdapter, TheAudienceIsRegisteredAsCarriedState) {
  const auto entries = allOwnedStructures();
  const auto found = std::ranges::find_if(entries, [](const OwnedStructure &e) {
    return e.id == "agent.audience";
  });
  ASSERT_NE(found, entries.end());
  EXPECT_EQ(found->resume, Fate::Carried);
  EXPECT_EQ(found->upgrade, Fate::Carried);
  EXPECT_EQ(found->dimension, "d.epistemic");
}

// --- settings
// -----------------------------------------------------------------

TEST(VagentEpistemicAdapterSettings, TheFiveSettingsShipWithTheirDefaults) {
  xanadu::Store store;
  store.setSystem(true);
  xanadu::initializeSystemStore(store, xanadu::SystemDocKind::Settings);
  const auto model = xanadu::SystemStoreModel::fromStore(store);
  ASSERT_TRUE(model.isValid()) << model.validationError();
  EXPECT_EQ(AdapterLimits::fromModel(model), AdapterLimits{});
  EXPECT_EQ(AdapterLimits{}.verify.verifyPerPeer, 500);

  auto ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                                xanadu::settings::kVagentEpistemicVerifyPerPeer,
                                std::int64_t{40});
  store.repointCurrentVersion(ver);
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentEpistemicIncarnationStep,
                           std::int64_t{8});
  store.repointCurrentVersion(ver);
  const auto changed =
      AdapterLimits::fromModel(xanadu::SystemStoreModel::fromStore(store));
  EXPECT_EQ(changed.verify.verifyPerPeer, 40);
  EXPECT_EQ(changed.incarnationStep, 8);

  // One value out of range keeps every default.
  ver = xanadu::setSetting(store, store.primaryCurrentVersion(),
                           xanadu::settings::kVagentEpistemicWallRollbackMs,
                           std::int64_t{1});
  store.repointCurrentVersion(ver);
  EXPECT_EQ(
      AdapterLimits::fromModel(xanadu::SystemStoreModel::fromStore(store)),
      AdapterLimits{});
}

} // namespace
