/**
 * @file vagent_epistemic_adapter_wire_test.cpp
 * @brief The signed-ingress codec and verifier (P19 I3-3a1).
 *
 * The canonical envelope and body, the closed key sets, the identifier
 * class and the Int bound; the kind table; domain separation; the pinned
 * and delegated key paths with the master-keyed cache and the wall
 * rollback guard; the durable, fail-closed revocation registry over the
 * identity ledger; the pre-signature seen-header refusal; the per-peer
 * budget; and what refusals may log. The measurements `i3-3-design.md`
 * section 12 names for a1 print as `[measure]` lines.
 */
#include <gleditor/logging.hpp>
#include <gtest/gtest.h>
#include <spdlog/sinks/ostream_sink.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"
#include "common/xanadu/merkle_ledger.hpp"
#include "common/xanadu/user_permascroll.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_verify.hpp"
#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"

namespace {

using namespace xanadu::vagent::runtime;
namespace id = xanadu::identity;
using xanadu::MerkleLedger;
using xanadu::bencode::Dict;
using xanadu::bencode::Value;
using WallClock = std::chrono::system_clock;

// An audience no key or issuer could equal, so a log line that leaked it
// is found by a plain substring search.
const std::string kAudience = std::string(32, 'a') + std::string(32, 'd');
const std::string kPolicy(64, 'c');

std::string hexOf(std::span<const std::uint8_t> bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (const auto b : bytes) {
    out.push_back(digits[b >> 4U]);
    out.push_back(digits[b & 0xFU]);
  }
  return out;
}

id::StandardCryptoEngine newKey() {
  auto engine = id::StandardCryptoEngine::generate();
  EXPECT_TRUE(engine.has_value());
  return std::move(*engine);
}

SignedHeader headerFor(const id::StandardCryptoEngine &device,
                       const SignedKind kind, const std::int64_t sequence) {
  SignedHeader h;
  h.kind             = kind;
  h.issuer           = issuerOf(device.publicKey());
  h.incarnation      = 1;
  h.sequence         = sequence;
  h.audience         = kAudience;
  h.sent             = 1'000'000'000;
  h.epoch            = 0;
  h.basis            = SignedClockBasis::Synchronised;
  h.clockUncertainty = 1'000'000;
  if (kind == SignedKind::Heartbeat || kind == SignedKind::HealthReport)
    h.expires = 2'000'000'000;
  return h;
}

ObservationPayload observation(std::string eventId = "e1") {
  ObservationPayload o;
  o.eventId             = std::move(eventId);
  o.predicate           = "at";
  o.policy              = kPolicy;
  o.tuple               = {std::string("self"), encodeSignedFloat(1.5)};
  o.acquired            = 900'000'000;
  o.acquiredUncertainty = 10'000'000;
  o.frame               = "enu";
  o.units               = "m";
  o.variance            = 0.25;
  return o;
}

SignedPayload payloadFor(const SignedKind kind) {
  switch (kind) {
  case SignedKind::Observation:
    return observation();
  case SignedKind::Heartbeat:
    return HeartbeatPayload{};
  case SignedKind::HealthReport:
    return HealthReportPayload{ReportAction::Raise, "battery_low"};
  case SignedKind::ClockPair: {
    ClockPairPayload pair;
    pair.nonce.fill(0x5A);
    return pair;
  }
  case SignedKind::Verification:
    break;
  }
  return HeartbeatPayload{};
}

std::string seal(const id::StandardCryptoEngine &device, std::string body,
                 std::optional<std::vector<std::uint8_t>> certificate = {}) {
  auto envelope = signEnvelope(device, std::move(body), std::move(certificate));
  EXPECT_TRUE(envelope.has_value());
  return encodeSignedEnvelope(*envelope);
}

std::string message(const id::StandardCryptoEngine &device,
                    const SignedHeader &h, const SignedPayload &p,
                    std::optional<std::vector<std::uint8_t>> certificate = {}) {
  return seal(device, encodeSignedBody(h, p), std::move(certificate));
}

/// The body re-encoded after @p edit has changed its header and payload
/// dictionaries, then signed again, so only the shape is wrong.
std::string
edited(const id::StandardCryptoEngine &device, const SignedHeader &h,
       const SignedPayload &p,
       const std::function<void(Dict &header, Dict &payload)> &edit) {
  const auto body = xanadu::bencode::decode(encodeSignedBody(h, p));
  Dict header     = body.find("h")->asDict();
  Dict payload    = body.find("p")->asDict();
  edit(header, payload);
  Dict outer;
  outer.emplace("h", Value::dict(std::move(header)));
  outer.emplace("p", Value::dict(std::move(payload)));
  return seal(device, Value::dict(std::move(outer)).encode());
}

std::expected<SignedMessage, EpistemicRefusal> decoded(std::string_view b) {
  return decodeSignedMessage(b, kMaxSignedMessageBytes);
}

// --- ledger and identity helpers --------------------------------------------

void addMaster(MerkleLedger &ledger, const id::StandardCryptoEngine &master,
               bool revoked = false) {
  xanadu::IdentityKeyLink link;
  link.fingerprint      = issuerOf(master.publicKey());
  link.identity         = "Sensor Owner";
  link.email            = "owner@test.invalid";
  link.publicKeyArmored = hexOf(master.publicKey().bytes);
  link.timestamp        = 1;
  link.revoked          = revoked;
  ledger.appendKey(link);
}

std::uint64_t nowSeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          WallClock::now().time_since_epoch())
          .count());
}

id::X509Certificate certify(const id::StandardCryptoEngine &master,
                            const id::PubKey32 &device, std::uint64_t notBefore,
                            std::uint64_t notAfter, std::uint64_t serial = 7) {
  id::CertificateConstraints c;
  c.serialNumber = serial;
  c.notBefore    = notBefore;
  c.notAfter     = notAfter;
  c.deviceId     = "sensor-1";
  c.deviceName   = "sensor-1";
  c.authorName   = "Sensor Owner";
  c.authorEmail  = "owner@test.invalid";
  auto cert      = master.createDelegationCertificate(master, device, c);
  EXPECT_TRUE(cert.has_value());
  return *cert;
}

id::X509Certificate currentCert(const id::StandardCryptoEngine &master,
                                const id::PubKey32 &device,
                                std::uint64_t serial = 7) {
  const auto now = nowSeconds();
  return certify(master, device, now - 3600, now + 86400, serial);
}

id::DeviceRevocationRecord revocation(const id::StandardCryptoEngine &master,
                                      const id::PubKey32 &device,
                                      std::uint64_t serial = 0) {
  id::DeviceRevocationRecord r;
  r.serialNumber      = serial;
  r.devicePublicKey   = device;
  r.masterFingerprint = *id::Fingerprint::fromString(
      id::StandardCryptoEngine::computeFingerprint(master.publicKey()));
  r.revocationDate  = nowSeconds();
  r.reason          = id::RevocationReason::KeyCompromise;
  const auto buffer = r.signingBuffer();
  r.masterSignature = *master.sign(
      {reinterpret_cast<const std::uint8_t *>(buffer.data()), buffer.size()});
  return r;
}

WallReading synced(WallClock::time_point at = WallClock::now()) {
  return {at, true};
}

SignerTrust pinned(const id::StandardCryptoEngine &device) {
  return {issuerOf(device.publicKey()), PinnedTrust{}};
}

SignerTrust delegated(const id::StandardCryptoEngine &device,
                      const id::StandardCryptoEngine &master) {
  return {issuerOf(device.publicKey()),
          DelegatedTrust{issuerOf(master.publicKey())}};
}

/// One host's adapter state: the ledger, the registry over it, the master
/// lookup and a verifier.
struct Rig {
  explicit Rig(SignedIngressLimits limits = {}, bool load = true)
      : verifier(limits, registry, masters) {
    if (load) registry.load();
  }
  MerkleLedger ledger;
  DurableRevocationRegistry registry{ledger};
  LedgerMasterKeys masters{ledger};
  SignedVerifier verifier;
  TransportPeer peer{std::string(64, '9')};

  std::expected<SignedMessage, EpistemicRefusal>
  run(std::string_view bytes, std::optional<SignerTrust> trust,
      WallReading wall = synced(),
      std::chrono::steady_clock::time_point at =
          std::chrono::steady_clock::now()) {
    const auto lookup = [&](const SignedHeader &) { return trust; };
    return verifier.verify(bytes, peer, lookup, wall, at);
  }
};

std::optional<EpistemicRefusal>
refusalOf(const std::expected<SignedMessage, EpistemicRefusal> &result) {
  if (result) return std::nullopt;
  return result.error();
}

// ============================================================================
// Codec
// ============================================================================

TEST(SignedWireTest, EveryKindRoundTripsThroughItsCanonicalBytes) {
  const auto device = newKey();
  for (const auto kind : {SignedKind::Observation, SignedKind::Heartbeat,
                          SignedKind::HealthReport, SignedKind::ClockPair}) {
    const auto h     = headerFor(device, kind, 5);
    const auto p     = payloadFor(kind);
    const auto bytes = message(device, h, p);
    const auto m     = decoded(bytes);
    ASSERT_TRUE(m) << toString(kind) << " " << epistemicRefusalName(m.error());
    EXPECT_EQ(m->header, h);
    EXPECT_EQ(m->payload, p);
    EXPECT_EQ(m->envelope.body, encodeSignedBody(h, p));
    EXPECT_EQ(m->envelope.key, device.publicKey());
    EXPECT_FALSE(m->envelope.certificate);
    EXPECT_EQ(encodeSignedEnvelope(m->envelope), bytes);
  }
}

TEST(SignedWireTest, AFloatIsItsBigEndianBitPatternAndOnlyFiniteNonNegZero) {
  const auto one = encodeSignedFloat(1.5);
  EXPECT_EQ(one, std::string("\x3F\xF8\0\0\0\0\0\0", 8));
  EXPECT_EQ(signedFloat(one), 1.5);
  for (const double v :
       {0.0, -2.25, 1e-310, 6.02e23, std::numeric_limits<double>::max()})
    EXPECT_EQ(signedFloat(encodeSignedFloat(v)), v) << v;
  for (const double v : {-0.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()})
    EXPECT_FALSE(signedFloat(encodeSignedFloat(v))) << v;
  EXPECT_FALSE(signedFloat(std::string(7, '\0')));
  EXPECT_FALSE(signedFloat(std::string(9, '\0')));
  EXPECT_EQ(signedBool(0), false);
  EXPECT_EQ(signedBool(1), true);
  EXPECT_FALSE(signedBool(2));
  EXPECT_FALSE(signedBool(-1));
}

TEST(SignedWireTest, ANegativeZeroOrNonFiniteVarianceRefusesAtDecode) {
  const auto device = newKey();
  const auto h      = headerFor(device, SignedKind::Observation, 1);
  for (const double v : {-0.0, std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity()}) {
    auto p     = observation();
    p.variance = v;
    EXPECT_EQ(refusalOf(decoded(message(device, h, p))),
              EpistemicRefusal::MalformedMessage)
        << v;
  }
  const auto shortFloat = edited(device, h, observation(), [](Dict &, Dict &p) {
    p.insert_or_assign("variance", Value::string(std::string(7, '\0')));
  });
  EXPECT_EQ(refusalOf(decoded(shortFloat)), EpistemicRefusal::MalformedMessage);
}

TEST(SignedWireTest, AnIntAtTwoToTheSixtySecondRefusesAndOneBelowPasses) {
  const auto device = newKey();
  constexpr auto at = kSignedIntBound;
  auto h            = headerFor(device, SignedKind::Observation, at - 1);
  EXPECT_TRUE(decoded(message(device, h, observation())));
  h.sequence = at;
  EXPECT_EQ(refusalOf(decoded(message(device, h, observation()))),
            EpistemicRefusal::MalformedMessage);
  h = headerFor(device, SignedKind::Observation, 1);
  // A negative time passes decode: whether it is a fault is the device's.
  h.sent = -5;
  EXPECT_TRUE(decoded(message(device, h, observation())));
  h.sent = -at;
  EXPECT_EQ(refusalOf(decoded(message(device, h, observation()))),
            EpistemicRefusal::MalformedMessage);
  h          = headerFor(device, SignedKind::Observation, 1);
  auto p     = observation();
  p.acquired = at;
  EXPECT_EQ(refusalOf(decoded(message(device, h, p))),
            EpistemicRefusal::MalformedMessage);
  p       = observation();
  p.tuple = {std::string("self"), std::int64_t{at}};
  EXPECT_EQ(refusalOf(decoded(message(device, h, p))),
            EpistemicRefusal::MalformedMessage);
  h             = headerFor(device, SignedKind::Observation, 1);
  h.incarnation = 0;
  EXPECT_EQ(refusalOf(decoded(message(device, h, observation()))),
            EpistemicRefusal::MalformedMessage);
}

TEST(SignedWireTest, AnUnknownKeyAtEveryLevelRefuses) {
  const auto device = newKey();
  const auto h      = headerFor(device, SignedKind::Observation, 1);
  const auto p      = observation();
  // Envelope.
  {
    auto envelope = *signEnvelope(device, encodeSignedBody(h, p));
    auto outer    = xanadu::bencode::decode(encodeSignedEnvelope(envelope));
    Dict d        = outer.asDict();
    d.emplace("x", Value::integer(1));
    EXPECT_EQ(refusalOf(decoded(Value::dict(std::move(d)).encode())),
              EpistemicRefusal::MalformedMessage);
  }
  // Body.
  {
    auto body = xanadu::bencode::decode(encodeSignedBody(h, p));
    Dict d    = body.asDict();
    d.emplace("q", Value::dict({}));
    EXPECT_EQ(refusalOf(decoded(seal(device, Value::dict(d).encode()))),
              EpistemicRefusal::MalformedMessage);
  }
  // Header and every kind's payload.
  for (const auto kind : {SignedKind::Observation, SignedKind::Heartbeat,
                          SignedKind::HealthReport, SignedKind::ClockPair}) {
    const auto hk = headerFor(device, kind, 1);
    const auto pk = payloadFor(kind);
    EXPECT_EQ(refusalOf(decoded(edited(device, hk, pk,
                                       [](Dict &header, Dict &) {
                                         header.emplace("zzz",
                                                        Value::integer(0));
                                       }))),
              EpistemicRefusal::MalformedMessage)
        << toString(kind);
    EXPECT_EQ(refusalOf(decoded(edited(device, hk, pk,
                                       [](Dict &, Dict &payload) {
                                         payload.emplace("extra",
                                                         Value::integer(0));
                                       }))),
              EpistemicRefusal::MalformedMessage)
        << toString(kind);
  }
}

TEST(SignedWireTest, ANewlineNulBidiOrSpaceInAnyIdentifierRefuses) {
  const auto device = newKey();
  const std::vector<std::string> bad{"a\nb",
                                     std::string("a\0b", 3),
                                     "\xE2\x80\xAE"
                                     "evil",
                                     "a b",
                                     "",
                                     "caf\xC3\xA9"};
  const auto obs    = headerFor(device, SignedKind::Observation, 1);
  const auto report = headerFor(device, SignedKind::HealthReport, 1);
  const auto check  = [&](const SignedHeader &h, const SignedPayload &p,
                         std::string_view field, bool inHeader) {
    for (const auto &text : bad) {
      const auto bytes = edited(device, h, p, [&](Dict &header, Dict &payload) {
        (inHeader ? header : payload)
            .insert_or_assign(std::string(field), Value::string(text));
      });
      EXPECT_EQ(refusalOf(decoded(bytes)), EpistemicRefusal::MalformedMessage)
          << field << " "
          << hexOf({reinterpret_cast<const std::uint8_t *>(text.data()),
                    text.size()});
    }
  };
  for (const auto *field : {"event_id", "predicate", "frame", "units"})
    check(obs, observation(), field, false);
  check(report, payloadFor(SignedKind::HealthReport), "code", false);
  check(report, payloadFor(SignedKind::HealthReport), "action", false);
  for (const auto *field : {"kind", "basis"})
    check(obs, observation(), field, true);
  // The hex identifiers: uppercase, short and non-hex all refuse.
  const std::vector<std::string> badHex{
      std::string(64, 'A'), std::string(63, 'a'), std::string(63, 'a') + "g"};
  for (const auto &text : badHex) {
    for (const auto *field : {"issuer", "audience"})
      EXPECT_EQ(refusalOf(decoded(edited(device, obs, observation(),
                                         [&](Dict &header, Dict &) {
                                           header.insert_or_assign(
                                               field, Value::string(text));
                                         }))),
                EpistemicRefusal::MalformedMessage)
          << field;
    EXPECT_EQ(refusalOf(decoded(edited(device, obs, observation(),
                                       [&](Dict &, Dict &payload) {
                                         payload.insert_or_assign(
                                             "policy", Value::string(text));
                                       }))),
              EpistemicRefusal::MalformedMessage);
  }
  // Over the length bound.
  auto p    = observation();
  p.eventId = std::string(kMaxEventIdBytes + 1, 'e');
  EXPECT_EQ(refusalOf(decoded(message(device, obs, p))),
            EpistemicRefusal::MalformedMessage);
  p.eventId = std::string(kMaxEventIdBytes, 'e');
  EXPECT_TRUE(decoded(message(device, obs, p)));
  auto r = HealthReportPayload{ReportAction::Raise,
                               std::string(kMaxHealthCodeBytes + 1, 'x')};
  EXPECT_EQ(refusalOf(decoded(message(device, report, r))),
            EpistemicRefusal::MalformedMessage);
}

TEST(SignedWireTest, TupleTextIsAReadingAndMayBeUtf8) {
  EXPECT_TRUE(isSignedText("caf\xC3\xA9 \xF0\x9F\x8C\x8D", 64));
  EXPECT_TRUE(isSignedText("", 64));
  EXPECT_FALSE(isSignedText("\xC0\xAF", 64));           // overlong
  EXPECT_FALSE(isSignedText("\xED\xA0\x80", 64));       // surrogate
  EXPECT_FALSE(isSignedText("\xF4\x90\x80\x80", 64));   // past U+10FFFF
  EXPECT_FALSE(isSignedText("\xE2\x82", 64));           // truncated
  EXPECT_FALSE(isSignedText(std::string(65, 'a'), 64)); // bound
  EXPECT_TRUE(isSignedIdentifier("sensor.temp_1", 64));
  EXPECT_FALSE(isSignedIdentifier("sensor\x7F", 64));
}

TEST(SignedWireTest, TheKindTableRequiresAllowsAndRefusesTheOptionalFields) {
  const auto device = newKey();
  // `expires`: required on heartbeats and reports, refused on clock pairs.
  for (const auto kind : {SignedKind::Heartbeat, SignedKind::HealthReport}) {
    auto h    = headerFor(device, kind, 1);
    h.expires = std::nullopt;
    EXPECT_EQ(refusalOf(decoded(message(device, h, payloadFor(kind)))),
              EpistemicRefusal::MalformedMessage)
        << toString(kind);
  }
  auto pair    = headerFor(device, SignedKind::ClockPair, 1);
  pair.expires = 5;
  EXPECT_EQ(refusalOf(decoded(
                message(device, pair, payloadFor(SignedKind::ClockPair)))),
            EpistemicRefusal::MalformedMessage);
  auto obs = headerFor(device, SignedKind::Observation, 1);
  EXPECT_TRUE(decoded(message(device, obs, observation())));
  obs.expires = 5;
  EXPECT_TRUE(decoded(message(device, obs, observation())));
  // `low_watermark` belongs to verification only, so it cannot ride on
  // another kind.
  for (const auto kind : {SignedKind::Observation, SignedKind::Heartbeat,
                          SignedKind::HealthReport, SignedKind::ClockPair}) {
    auto h         = headerFor(device, kind, 1);
    h.lowWatermark = 0;
    EXPECT_EQ(refusalOf(decoded(message(device, h, payloadFor(kind)))),
              EpistemicRefusal::MalformedMessage)
        << toString(kind);
  }
  // Verification: its header is fixed now and its payload is I3-4a's.
  auto v = headerFor(device, SignedKind::Verification, 1);
  EXPECT_EQ(refusalOf(decoded(message(device, v, HeartbeatPayload{}))),
            EpistemicRefusal::MalformedMessage);
  v.lowWatermark = 0;
  EXPECT_EQ(refusalOf(decoded(message(device, v, HeartbeatPayload{}))),
            EpistemicRefusal::UnsupportedKind);
  EXPECT_EQ(refusalOf(decoded(edited(device, obs, observation(),
                                     [](Dict &header, Dict &) {
                                       header.insert_or_assign(
                                           "kind", Value::string("drift"));
                                     }))),
            EpistemicRefusal::UnsupportedKind);
  // A heartbeat's payload is empty; a clock pair's nonce is 32 bytes.
  EXPECT_EQ(refusalOf(decoded(
                edited(device, headerFor(device, SignedKind::ClockPair, 1),
                       payloadFor(SignedKind::ClockPair),
                       [](Dict &, Dict &payload) {
                         payload.insert_or_assign(
                             "nonce", Value::string(std::string(31, 'n')));
                       }))),
            EpistemicRefusal::MalformedMessage);
}

TEST(SignedWireTest, ASecondSpellingOfTheBodyIsNonCanonical) {
  const auto device = newKey();
  const auto h      = headerFor(device, SignedKind::Observation, 1);
  const auto body   = encodeSignedBody(h, observation());
  // A leading zero.
  auto zero        = body;
  const auto where = zero.find("11:incarnationi1e");
  ASSERT_NE(where, std::string::npos);
  zero.replace(where, 17, "11:incarnationi01e");
  EXPECT_EQ(refusalOf(decoded(seal(device, zero))),
            EpistemicRefusal::NonCanonicalMessage);
  // Keys out of order, and a repeated key.
  const auto parsed  = xanadu::bencode::decode(body);
  const auto hBytes  = std::string(parsed.find("h")->rawEncoding());
  const auto pBytes  = std::string(parsed.find("p")->rawEncoding());
  const auto swapped = "d1:p" + pBytes + "1:h" + hBytes + "e";
  EXPECT_EQ(refusalOf(decoded(seal(device, swapped))),
            EpistemicRefusal::NonCanonicalMessage);
  const auto twice = "d1:h" + hBytes + "1:h" + hBytes + "1:p" + pBytes + "e";
  EXPECT_EQ(refusalOf(decoded(seal(device, twice))),
            EpistemicRefusal::NonCanonicalMessage);
  // Garbage is malformed, not a second spelling.
  EXPECT_EQ(refusalOf(decoded(seal(device, "d1:h"))),
            EpistemicRefusal::MalformedMessage);
  // The typed decoder error is a runtime_error for every other caller.
  EXPECT_THROW((void)xanadu::bencode::decode("i-0e"),
               xanadu::bencode::NonCanonicalError);
  EXPECT_THROW((void)xanadu::bencode::decode("i-0e"), std::runtime_error);
}

TEST(SignedWireTest, TheEnvelopeIsClosedAndSized) {
  const auto device = newKey();
  const auto h      = headerFor(device, SignedKind::Observation, 1);
  const auto bytes  = message(device, h, observation());
  EXPECT_EQ(refusalOf(decodeSignedMessage(bytes, bytes.size() - 1)),
            EpistemicRefusal::OversizeMessage);
  EXPECT_TRUE(decodeSignedMessage(bytes, bytes.size()));
  EXPECT_EQ(refusalOf(decoded(bytes + "x")),
            EpistemicRefusal::MalformedMessage);
  auto envelope = *signEnvelope(device, encodeSignedBody(h, observation()));
  // An empty certificate is a present-but-empty optional.
  envelope.certificate = std::vector<std::uint8_t>{};
  EXPECT_EQ(refusalOf(decoded(encodeSignedEnvelope(envelope))),
            EpistemicRefusal::MalformedMessage);
  auto outer = xanadu::bencode::decode(bytes).asDict();
  outer.insert_or_assign("k", Value::string(std::string(31, 'k')));
  EXPECT_EQ(refusalOf(decoded(Value::dict(outer).encode())),
            EpistemicRefusal::MalformedMessage);
  outer = xanadu::bencode::decode(bytes).asDict();
  outer.insert_or_assign("s", Value::string(std::string(63, 's')));
  EXPECT_EQ(refusalOf(decoded(Value::dict(outer).encode())),
            EpistemicRefusal::MalformedMessage);
  outer = xanadu::bencode::decode(bytes).asDict();
  outer.erase("b");
  EXPECT_EQ(refusalOf(decoded(Value::dict(outer).encode())),
            EpistemicRefusal::MalformedMessage);
  // An absent optional payload field is absent, never empty.
  EXPECT_EQ(refusalOf(decoded(edited(device, h, observation(),
                                     [](Dict &, Dict &payload) {
                                       payload.insert_or_assign(
                                           "frame", Value::string(""));
                                     }))),
            EpistemicRefusal::MalformedMessage);
  auto noFrame  = observation();
  noFrame.frame = "";
  noFrame.units = "";
  const auto m  = decoded(message(device, h, noFrame));
  ASSERT_TRUE(m);
  EXPECT_EQ(std::get<ObservationPayload>(m->payload).frame, "");
}

TEST(SignedWireTest, EveryDomainIsPrefixFreeOfEveryOther) {
  const std::vector<std::string_view> domains{
      id::kDomainManifest, id::kDomainDeviceCsr, id::kDomainDeviceRevocation,
      id::kDomainPeerAuth, id::kDomainOracleAttestation,
      id::kDomainVagentSigned,
      // Not a constant, but signed with the same engine.
      "xanadu-master-superseded-v1\n"};
  for (std::size_t i = 0; i < domains.size(); ++i)
    for (std::size_t j = 0; j < domains.size(); ++j)
      if (i != j)
        EXPECT_FALSE(domains[j].starts_with(domains[i]))
            << domains[i] << " is a prefix of " << domains[j];
  EXPECT_EQ(signedBytesOf("abc"), std::string(id::kDomainVagentSigned) + "abc");
}

// ============================================================================
// Verifier: keys, signatures, trust modes
// ============================================================================

TEST(SignedVerifierTest, APinnedMessageVerifiesOnce) {
  Rig rig;
  const auto device = newKey();
  const auto bytes  = message(
      device, headerFor(device, SignedKind::Observation, 1), observation());
  const auto m = rig.run(bytes, pinned(device));
  ASSERT_TRUE(m) << epistemicRefusalName(m.error());
  EXPECT_EQ(rig.verifier.stats().signatureChecks, 1U);
  EXPECT_EQ(rig.verifier.stats().pathValidations, 0U);
}

TEST(SignedVerifierTest, AForgedOrOtherDomainSignatureRefuses) {
  Rig rig;
  const auto device = newKey();
  const auto h      = headerFor(device, SignedKind::Observation, 1);
  const auto body   = encodeSignedBody(h, observation());
  auto envelope     = *signEnvelope(device, body);
  envelope.signature.bytes[3] ^= 0x01U;
  EXPECT_EQ(refusalOf(rig.run(encodeSignedEnvelope(envelope), pinned(device))),
            EpistemicRefusal::BadSignature);
  // The same body signed under the manifest domain.
  auto other      = *signEnvelope(device, body);
  other.signature = *device.signWithDomain(
      id::kDomainManifest,
      {reinterpret_cast<const std::uint8_t *>(body.data()), body.size()});
  EXPECT_EQ(refusalOf(rig.run(encodeSignedEnvelope(other), pinned(device))),
            EpistemicRefusal::BadSignature);
  // Signed by somebody else's key under this device's issuer.
  const auto thief = newKey();
  auto stolen      = *signEnvelope(thief, body);
  stolen.key       = device.publicKey();
  EXPECT_EQ(refusalOf(rig.run(encodeSignedEnvelope(stolen), pinned(device))),
            EpistemicRefusal::BadSignature);
}

TEST(SignedVerifierTest, TheIssuerIsTheKeysHashComparedAsText) {
  Rig rig;
  const auto device = newKey();
  const auto other  = newKey();
  auto h            = headerFor(device, SignedKind::Observation, 1);
  h.issuer          = issuerOf(other.publicKey());
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, observation()), pinned(other))),
      EpistemicRefusal::IssuerKeyMismatch);
  EXPECT_EQ(rig.verifier.stats().signatureChecks, 0U);
}

TEST(SignedVerifierTest, ASmallOrderOrNonCanonicalKeyRefuses) {
  id::PubKey32 identityPoint{};
  identityPoint.bytes[0] = 0x01;
  EXPECT_FALSE(isAcceptableSignerKey(identityPoint));
  // y = p + 1, a second spelling of the order-1 point y = 1; and y = p.
  for (const std::uint8_t low : {0xEEU, 0xEDU, 0xFFU}) {
    id::PubKey32 key{};
    key.bytes.fill(0xFF);
    key.bytes[0]  = low;
    key.bytes[31] = 0x7F;
    EXPECT_FALSE(isAcceptableSignerKey(key)) << int{low};
    key.bytes[31] = 0xFF; // the sign bit is not part of y
    EXPECT_FALSE(isAcceptableSignerKey(key)) << int{low};
  }
  // p - 1 is canonical.
  id::PubKey32 below{};
  below.bytes.fill(0xFF);
  below.bytes[0]  = 0xEC;
  below.bytes[31] = 0x7F;
  // ...but it is the order-2 point (0, -1), which is small order.
  EXPECT_FALSE(isAcceptableSignerKey(below));
  EXPECT_TRUE(isAcceptableSignerKey(newKey().publicKey()));

  // Through the verifier, with the issuer set to match the bad key so the
  // key check is what refuses.
  Rig rig;
  const auto device = newKey();
  for (const auto &bad : {identityPoint, [] {
                            id::PubKey32 k{};
                            k.bytes.fill(0xFF);
                            k.bytes[0]  = 0xEE;
                            k.bytes[31] = 0x7F;
                            return k;
                          }()}) {
    auto h        = headerFor(device, SignedKind::Observation, 1);
    h.issuer      = issuerOf(bad);
    auto envelope = *signEnvelope(device, encodeSignedBody(h, observation()));
    envelope.key  = bad;
    EXPECT_EQ(refusalOf(rig.run(encodeSignedEnvelope(envelope),
                                SignerTrust{h.issuer, PinnedTrust{}})),
              EpistemicRefusal::InvalidSignerKey);
  }
}

TEST(SignedVerifierTest, ABindingDecidesTheCertificateAndAMissingOneRefuses) {
  Rig rig;
  const auto device = newKey();
  const auto master = newKey();
  addMaster(rig.ledger, master);
  const auto h    = headerFor(device, SignedKind::Observation, 1);
  const auto cert = currentCert(master, device.publicKey());
  // `c` on a pinned binding.
  EXPECT_EQ(refusalOf(rig.run(message(device, h, observation(), cert.der),
                              pinned(device))),
            EpistemicRefusal::UnexpectedCertificate);
  // No `c` on a delegated one.
  EXPECT_EQ(refusalOf(rig.run(message(device, h, observation()),
                              delegated(device, master))),
            EpistemicRefusal::DelegationMissing);
  // No binding, and a binding for another principal.
  EXPECT_EQ(refusalOf(rig.run(message(device, h, observation()), std::nullopt)),
            EpistemicRefusal::UnknownSource);
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, observation()), pinned(newKey()))),
      EpistemicRefusal::UnknownSource);
  EXPECT_EQ(rig.verifier.stats().signatureChecks, 0U);
}

// ============================================================================
// Delegation
// ============================================================================

TEST(SignedVerifierTest, ADelegatedKeyVerifiesThenHitsTheCache) {
  Rig rig;
  const auto device = newKey();
  const auto master = newKey();
  addMaster(rig.ledger, master);
  const auto cert  = currentCert(master, device.publicKey());
  const auto trust = delegated(device, master);
  for (int seq = 1; seq <= 3; ++seq) {
    const auto m =
        rig.run(message(device, headerFor(device, SignedKind::Heartbeat, seq),
                        HeartbeatPayload{}, cert.der),
                trust);
    ASSERT_TRUE(m) << epistemicRefusalName(m.error());
  }
  EXPECT_EQ(rig.verifier.stats().pathValidations, 1U);
  EXPECT_EQ(rig.verifier.stats().cacheHits, 2U);
}

TEST(SignedVerifierTest, ACacheHitUnderMasterADoesNotSatisfyMasterB) {
  Rig rig;
  const auto device  = newKey();
  const auto masterA = newKey();
  const auto masterB = newKey();
  addMaster(rig.ledger, masterA);
  addMaster(rig.ledger, masterB);
  const auto cert = currentCert(masterA, device.publicKey());
  const auto bytes =
      message(device, headerFor(device, SignedKind::Heartbeat, 1),
              HeartbeatPayload{}, cert.der);
  ASSERT_TRUE(rig.run(bytes, delegated(device, masterA)));
  EXPECT_EQ(rig.verifier.stats().pathValidations, 1U);
  EXPECT_EQ(refusalOf(rig.run(bytes, delegated(device, masterB))),
            EpistemicRefusal::DelegationInvalid);
  EXPECT_EQ(rig.verifier.stats().pathValidations, 2U);
  EXPECT_EQ(rig.verifier.stats().cacheHits, 0U);
}

TEST(SignedVerifierTest, ACertificateForAnotherKeyOrMasterRefuses) {
  Rig rig;
  const auto device   = newKey();
  const auto master   = newKey();
  const auto stranger = newKey();
  addMaster(rig.ledger, master);
  const auto h = headerFor(device, SignedKind::Heartbeat, 1);
  // Issued to another device.
  const auto other = currentCert(master, newKey().publicKey());
  EXPECT_EQ(refusalOf(rig.run(message(device, h, HeartbeatPayload{}, other.der),
                              delegated(device, master))),
            EpistemicRefusal::DelegationInvalid);
  // Issued by a master the binding does not name.
  const auto foreign = currentCert(stranger, device.publicKey());
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, HeartbeatPayload{}, foreign.der),
                        delegated(device, master))),
      EpistemicRefusal::DelegationInvalid);
  // A master the ledger does not hold.
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, HeartbeatPayload{}, foreign.der),
                        delegated(device, stranger))),
      EpistemicRefusal::DelegationInvalid);
  // Garbage DER.
  EXPECT_EQ(refusalOf(rig.run(message(device, h, HeartbeatPayload{},
                                      std::vector<std::uint8_t>{0x30, 0x01}),
                              delegated(device, master))),
            EpistemicRefusal::DelegationInvalid);
}

TEST(SignedVerifierTest, ADelegationIsCheckedAtReceiptWhateverItsIssueTime) {
  Rig rig;
  const auto device = newKey();
  const auto master = newKey();
  addMaster(rig.ledger, master);
  const auto now = nowSeconds();
  // Valid a year ago for a day.
  const auto expired =
      certify(master, device.publicKey(), now - 400 * 86400, now - 399 * 86400);
  // The old road judges the window at the TSV's issue time when it has one
  // (S-7): an issue time inside the old window passes an expired
  // certificate. With a zero issue time the explicit window check is
  // skipped, but path validation then falls back to the process clock, so
  // that case refuses at HEAD (recorded in i3-3-validation.md).
  xanadu::DeviceDelegation old;
  old.certificate = *id::StandardCryptoEngine::certificateFromDer(expired.der);
  old.masterFingerprint = *id::Fingerprint::fromString(
      id::StandardCryptoEngine::computeFingerprint(master.publicKey()));
  std::memcpy(old.devicePublicKey.bytes.data(), device.publicKey().bytes.data(),
              32);
  old.issuedTimestamp = now - 400 * 86400 + 3600;
  EXPECT_TRUE(old.verify(master.publicKey()));
  old.issuedTimestamp = 0;
  EXPECT_FALSE(old.verify(master.publicKey()));
  // This road judges it at receipt.
  const auto h = headerFor(device, SignedKind::Heartbeat, 1);
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, HeartbeatPayload{}, expired.der),
                        delegated(device, master))),
      EpistemicRefusal::DelegationExpired);
  const auto future =
      certify(master, device.publicKey(), now + 86400, now + 2 * 86400);
  EXPECT_EQ(
      refusalOf(rig.run(message(device, h, HeartbeatPayload{}, future.der),
                        delegated(device, master))),
      EpistemicRefusal::DelegationExpired);
}

TEST(SignedVerifierTest, AWallClockRolledBackPastTheMarkRefusesDelegation) {
  Rig rig;
  const auto device = newKey();
  const auto master = newKey();
  addMaster(rig.ledger, master);
  const auto cert  = currentCert(master, device.publicKey());
  const auto trust = delegated(device, master);
  const auto at    = WallClock::now();
  std::int64_t seq = 0;
  const auto next  = [&] {
    return message(device, headerFor(device, SignedKind::Heartbeat, ++seq),
                    HeartbeatPayload{}, cert.der);
  };
  ASSERT_TRUE(rig.run(next(), trust, synced(at)));
  // Within the bound (default 1,000 ms) is a slew, not a rollback.
  EXPECT_TRUE(
      rig.run(next(), trust, synced(at - std::chrono::milliseconds(900))));
  EXPECT_EQ(
      refusalOf(rig.run(next(), trust, synced(at - std::chrono::seconds(2)))),
      EpistemicRefusal::ClockUnsynchronised);
  // An unsynchronised host cannot judge a certificate at all.
  EXPECT_EQ(refusalOf(rig.run(next(), trust, WallReading{at, false})),
            EpistemicRefusal::ClockUnsynchronised);
  // A pinned key needs neither.
  EXPECT_TRUE(
      rig.run(message(device, headerFor(device, SignedKind::Heartbeat, ++seq),
                      HeartbeatPayload{}),
              pinned(device), WallReading{at - std::chrono::hours(1), false}));
}

// ============================================================================
// Revocation
// ============================================================================

TEST(SignedVerifierTest, RevokedAcrossARestartIsRefusedUntilLoadedThenRevoked) {
  const auto master  = newKey();
  const auto pinnedD = newKey();
  const auto delegD  = newKey();
  const auto cert    = currentCert(master, delegD.publicKey());
  std::string saved;
  {
    Rig rig;
    addMaster(rig.ledger, master);
    ASSERT_TRUE(rig.registry.admit(revocation(master, pinnedD.publicKey())));
    ASSERT_TRUE(rig.registry.admit(revocation(master, delegD.publicKey())));
    saved = rig.ledger.toTsv();
  }
  // The restart: a fresh registry over the ledger read back from disk.
  Rig rig({}, /*load=*/false);
  rig.ledger             = MerkleLedger::fromTsv(saved);
  std::int64_t seq       = 0;
  const auto pinnedBytes = [&] {
    return message(pinnedD, headerFor(pinnedD, SignedKind::Heartbeat, ++seq),
                   HeartbeatPayload{});
  };
  const auto delegBytes = [&] {
    return message(delegD, headerFor(delegD, SignedKind::Heartbeat, ++seq),
                   HeartbeatPayload{}, cert.der);
  };
  EXPECT_FALSE(rig.registry.ready());
  EXPECT_EQ(refusalOf(rig.run(pinnedBytes(), pinned(pinnedD))),
            EpistemicRefusal::RevocationUnavailable);
  EXPECT_EQ(refusalOf(rig.run(delegBytes(), delegated(delegD, master))),
            EpistemicRefusal::RevocationUnavailable);
  EXPECT_EQ(outcomeClassOf("RevocationUnavailable"), OutcomeClass::Retryable);
  rig.registry.load();
  std::vector<std::string> notices;
  rig.verifier.onFirstRevocation(
      [&](std::string_view p) { notices.emplace_back(p); });
  EXPECT_EQ(refusalOf(rig.run(pinnedBytes(), pinned(pinnedD))),
            EpistemicRefusal::KeyRevoked);
  EXPECT_EQ(refusalOf(rig.run(pinnedBytes(), pinned(pinnedD))),
            EpistemicRefusal::KeyRevoked);
  EXPECT_EQ(refusalOf(rig.run(delegBytes(), delegated(delegD, master))),
            EpistemicRefusal::KeyRevoked);
  // One notice per bound issuer, the principal token only.
  EXPECT_EQ(notices, (std::vector<std::string>{issuerOf(pinnedD.publicKey()),
                                               issuerOf(delegD.publicKey())}));
  EXPECT_EQ(outcomeClassOf("KeyRevoked"), OutcomeClass::NeedsHostAction);
}

TEST(SignedVerifierTest, AnUnverifiableRevocationRecordIsIgnoredAndCounted) {
  Rig rig({}, false);
  const auto master = newKey();
  const auto forger = newKey();
  const auto device = newKey();
  addMaster(rig.ledger, master);
  auto forged = revocation(master, device.publicKey());
  forged.masterSignature.bytes[0] ^= 0x01U;
  rig.ledger.appendDeviceRevocation(forged);
  // Signed by a key the ledger does not hold.
  rig.ledger.appendDeviceRevocation(revocation(forger, device.publicKey()));
  rig.registry.load();
  EXPECT_EQ(rig.registry.rejectedRecords(), 2U);
  EXPECT_FALSE(rig.registry.isDeviceRevoked(device.publicKey(), nowSeconds()));
  EXPECT_EQ(rig.registry.admit(forged).error(),
            RevocationAdmitError::BadSignature);
  EXPECT_EQ(rig.registry.admit(revocation(forger, device.publicKey())).error(),
            RevocationAdmitError::UnknownMaster);
  EXPECT_TRUE(
      rig.run(message(device, headerFor(device, SignedKind::Heartbeat, 1),
                      HeartbeatPayload{}),
              pinned(device)));
}

TEST(SignedVerifierTest, TheLiveFeedPersistsAndTakesEffect) {
  MerkleLedger ledger;
  int writes        = 0;
  bool persistOk    = true;
  const auto master = newKey();
  addMaster(ledger, master);
  DurableRevocationRegistry registry(ledger, [&](const MerkleLedger &) {
    ++writes;
    return persistOk;
  });
  // Fail-closed answers before load, for any consumer that skips ready().
  const auto device = newKey();
  EXPECT_TRUE(registry.isDeviceRevoked(device.publicKey(), 0));
  registry.load();
  EXPECT_FALSE(registry.isDeviceRevoked(device.publicKey(), 0));
  ASSERT_TRUE(registry.admit(revocation(master, device.publicKey(), 42)));
  EXPECT_EQ(writes, 1);
  EXPECT_EQ(ledger.deviceRevocations().size(), 1U);
  EXPECT_TRUE(registry.isDeviceRevoked(device.publicKey(), 0));
  EXPECT_TRUE(registry.isSerialRevoked(42, 0));
  persistOk     = false;
  const auto d2 = newKey();
  EXPECT_EQ(registry.admit(revocation(master, d2.publicKey())).error(),
            RevocationAdmitError::NotPersisted);
  EXPECT_TRUE(registry.isDeviceRevoked(d2.publicKey(), 0));
}

TEST(SignedVerifierTest, ARevokedSerialOrMasterRefusesADelegatedKey) {
  Rig rig;
  const auto master = newKey();
  const auto device = newKey();
  const auto other  = newKey();
  addMaster(rig.ledger, master);
  const auto cert  = currentCert(master, device.publicKey(), 99);
  const auto h     = headerFor(device, SignedKind::Heartbeat, 1);
  const auto bytes = message(device, h, HeartbeatPayload{}, cert.der);
  ASSERT_TRUE(rig.run(bytes, delegated(device, master)));
  // The serial, revoked through another device's record.
  ASSERT_TRUE(rig.registry.admit(revocation(master, other.publicKey(), 99)));
  EXPECT_EQ(refusalOf(rig.run(bytes, delegated(device, master))),
            EpistemicRefusal::KeyRevoked);

  Rig second;
  addMaster(second.ledger, master);
  ASSERT_TRUE(second.run(bytes, delegated(device, master)));
  // A cache hit still consults the ledger: the master revoked since.
  addMaster(second.ledger, master, /*revoked=*/true);
  EXPECT_EQ(refusalOf(second.run(bytes, delegated(device, master))),
            EpistemicRefusal::KeyRevoked);
}

// ============================================================================
// Step 4 and step 1
// ============================================================================

TEST(SignedVerifierTest, AReplayedSettledMessageIsRefusedBeforeItsSignature) {
  Rig rig;
  const auto device = newKey();
  const auto obs    = message(
      device, headerFor(device, SignedKind::Observation, 4), observation());
  const auto first = rig.run(obs, pinned(device));
  ASSERT_TRUE(first);
  // Not settled yet (a Retryable refusal downstream): the same bytes pass.
  ASSERT_TRUE(rig.run(obs, pinned(device)));
  EXPECT_EQ(rig.verifier.stats().signatureChecks, 2U);
  rig.verifier.noteSettled(first->header);
  EXPECT_EQ(refusalOf(rig.run(obs, pinned(device))),
            EpistemicRefusal::Duplicate);
  EXPECT_EQ(rig.verifier.stats().signatureChecks, 2U);
  // A forgery carrying a used header fails step 4 just the same: it would
  // have failed its signature anyway.
  auto forged =
      *signEnvelope(newKey(), *xanadu::bencode::decode(obs).find("b").transform(
                                  [](const Value &v) { return v.asString(); }));
  forged.key = device.publicKey();
  EXPECT_EQ(refusalOf(rig.run(encodeSignedEnvelope(forged), pinned(device))),
            EpistemicRefusal::Duplicate);

  // Heartbeats and reports: at or below the floor is stale.
  for (const auto kind : {SignedKind::Heartbeat, SignedKind::HealthReport}) {
    const auto at10 =
        message(device, headerFor(device, kind, 10), payloadFor(kind));
    const auto m = rig.run(at10, pinned(device));
    ASSERT_TRUE(m);
    rig.verifier.noteSettled(m->header);
    const auto checks = rig.verifier.stats().signatureChecks;
    EXPECT_EQ(refusalOf(rig.run(at10, pinned(device))),
              EpistemicRefusal::StaleSequence);
    EXPECT_EQ(refusalOf(rig.run(
                  message(device, headerFor(device, kind, 9), payloadFor(kind)),
                  pinned(device))),
              EpistemicRefusal::StaleSequence);
    EXPECT_EQ(rig.verifier.stats().signatureChecks, checks);
    EXPECT_TRUE(
        rig.run(message(device, headerFor(device, kind, 11), payloadFor(kind)),
                pinned(device)));
  }
  // A new incarnation starts its own sequence space.
  auto reboot        = headerFor(device, SignedKind::Heartbeat, 1);
  reboot.incarnation = 2;
  EXPECT_TRUE(
      rig.run(message(device, reboot, HeartbeatPayload{}), pinned(device)));
  rig.verifier.forget(issuerOf(device.publicKey()));
  EXPECT_TRUE(rig.run(obs, pinned(device)));
}

TEST(SignedVerifierTest, TheRingIsBoundedByTheReplayRingSetting) {
  SignedIngressLimits limits;
  limits.replayRing = 2;
  Rig rig(limits);
  const auto device = newKey();
  std::vector<std::string> sent;
  for (int seq = 1; seq <= 3; ++seq) {
    sent.push_back(message(device,
                           headerFor(device, SignedKind::Observation, seq),
                           observation("e" + std::to_string(seq))));
    const auto m = rig.run(sent.back(), pinned(device));
    ASSERT_TRUE(m);
    rig.verifier.noteSettled(m->header);
  }
  // The oldest left the ring and goes on to the arena's own dedup.
  EXPECT_TRUE(rig.run(sent[0], pinned(device)));
  EXPECT_EQ(refusalOf(rig.run(sent[1], pinned(device))),
            EpistemicRefusal::Duplicate);
  EXPECT_EQ(refusalOf(rig.run(sent[2], pinned(device))),
            EpistemicRefusal::Duplicate);
}

TEST(SignedVerifierTest, ThePerPeerBudgetRefusesPastItsRate) {
  SignedIngressLimits limits;
  limits.verifyPerPeer = kMinVerifyPerPeer;
  Rig rig(limits);
  const auto at = std::chrono::steady_clock::now();
  const TransportPeer a{"peer-a"};
  const TransportPeer b{"peer-b"};
  for (int i = 0; i < kMinVerifyPerPeer; ++i)
    EXPECT_TRUE(rig.verifier.admit(a, at));
  EXPECT_EQ(rig.verifier.admit(a, at).error(),
            EpistemicRefusal::IngressBackpressure);
  // Another peer has its own bucket, and the first refills with time.
  EXPECT_TRUE(rig.verifier.admit(b, at));
  EXPECT_TRUE(rig.verifier.admit(a, at + std::chrono::milliseconds(150)));
  EXPECT_EQ(rig.verifier.admit(a, at + std::chrono::milliseconds(150)).error(),
            EpistemicRefusal::IngressBackpressure);
  // Through verify, the budget refuses before decode: garbage costs a token.
  Rig full(limits);
  for (int i = 0; i < kMinVerifyPerPeer; ++i)
    EXPECT_EQ(refusalOf(full.run("garbage", std::nullopt, synced(), at)),
              EpistemicRefusal::MalformedMessage);
  EXPECT_EQ(refusalOf(full.run("garbage", std::nullopt, synced(), at)),
            EpistemicRefusal::IngressBackpressure);
}

TEST(SignedVerifierTest, TheLimitsValidateTheirRanges) {
  EXPECT_TRUE(SignedIngressLimits{}.validate());
  const auto broken = [](auto edit) {
    SignedIngressLimits limits;
    edit(limits);
    return limits.validate().error();
  };
  EXPECT_EQ(broken([](auto &l) { l.signedMessageBytes = 1023; }),
            SignedIngressLimitError::InvalidSignedMessageBytes);
  EXPECT_EQ(broken([](auto &l) { l.signedMessageBytes = 65537; }),
            SignedIngressLimitError::InvalidSignedMessageBytes);
  EXPECT_EQ(broken([](auto &l) { l.delegationCache = 15; }),
            SignedIngressLimitError::InvalidDelegationCache);
  EXPECT_EQ(broken([](auto &l) { l.verifyPerPeer = 9; }),
            SignedIngressLimitError::InvalidVerifyPerPeer);
  EXPECT_EQ(broken([](auto &l) { l.wallRollbackMs = 3'600'001; }),
            SignedIngressLimitError::InvalidWallRollback);
  EXPECT_EQ(broken([](auto &l) { l.replayRing = 0; }),
            SignedIngressLimitError::InvalidReplayRing);
}

// ============================================================================
// What a refusal may say
// ============================================================================

TEST(SignedVerifierTest, RefusalLogsCarryOnlyThePrincipalAndTheCode) {
  std::ostringstream out;
  auto sink   = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  auto logger = gleditor::logging::category("vagent.ingress");
  logger->sinks().push_back(sink);
  const auto previous = logger->level();
  logger->set_level(spdlog::level::trace);

  Rig rig;
  const auto device = newKey();
  const auto master = newKey();
  addMaster(rig.ledger, master);
  const auto cert = currentCert(master, device.publicKey());
  auto pair       = ClockPairPayload{};
  pair.nonce.fill(0x6E);
  const auto pairBytes =
      message(device, headerFor(device, SignedKind::ClockPair, 1), pair);
  auto envelope = *decodeSignedMessage(pairBytes, kMaxSignedMessageBytes);
  envelope.envelope.signature.bytes[0] ^= 0x01U;
  // BadSignature, UnexpectedCertificate, DelegationExpired and KeyRevoked.
  (void)rig.run(encodeSignedEnvelope(envelope.envelope), pinned(device));
  (void)rig.run(message(device, headerFor(device, SignedKind::Heartbeat, 2),
                        HeartbeatPayload{}, cert.der),
                pinned(device));
  const auto now = nowSeconds();
  (void)rig.run(
      message(device, headerFor(device, SignedKind::Heartbeat, 3),
              HeartbeatPayload{},
              certify(master, device.publicKey(), now - 9000, now - 8000).der),
      delegated(device, master));
  ASSERT_TRUE(rig.registry.admit(revocation(master, device.publicKey())));
  (void)rig.run(message(device, headerFor(device, SignedKind::Heartbeat, 4),
                        HeartbeatPayload{}),
                pinned(device));

  logger->sinks().pop_back();
  logger->set_level(previous);
  const auto log = out.str();
  for (const auto *code : {"BadSignature", "UnexpectedCertificate",
                           "DelegationExpired", "KeyRevoked"})
    EXPECT_NE(log.find(code), std::string::npos) << code;
  EXPECT_NE(log.find(issuerOf(device.publicKey())), std::string::npos);
  EXPECT_EQ(log.find(kAudience), std::string::npos);
  EXPECT_EQ(log.find(hexOf(pair.nonce)), std::string::npos);
  EXPECT_EQ(log.find(std::string(32, 'n')), std::string::npos);
  EXPECT_EQ(log.find(hexOf(device.publicKey().bytes)), std::string::npos);
  EXPECT_EQ(log.find(issuerOf(master.publicKey())), std::string::npos);
  EXPECT_EQ(log.find(std::string(cert.der.begin(), cert.der.end())),
            std::string::npos);
  EXPECT_EQ(log.find(id::StandardCryptoEngine::certificateToBase64Der(cert)),
            std::string::npos);
  // And every line is one of ours, so nothing else wrote there.
  std::istringstream lines(log);
  for (std::string line; std::getline(lines, line);)
    EXPECT_NE(line.find("signed message refused"), std::string::npos) << line;
}

// ============================================================================
// The ledger carries device revocations
// ============================================================================

TEST(SignedLedgerTest, DeviceRevocationsShareTheTreeAndRoundTrip) {
  MerkleLedger ledger;
  const auto master = newKey();
  addMaster(ledger, master);
  const auto before       = ledger.root();
  const auto record       = revocation(master, newKey().publicKey(), 5);
  const auto [leaf, root] = ledger.appendDeviceRevocation(record);
  EXPECT_EQ(leaf, 1U);
  EXPECT_NE(root, before);
  addMaster(ledger, newKey());
  EXPECT_EQ(ledger.entries().back().sequence, 2U);
  EXPECT_EQ(ledger.leafCount(), 3U);
  EXPECT_EQ(ledger.size(), 2U);
  const auto proof = ledger.generateProof(1);
  EXPECT_EQ(proof.leafHash, MerkleLedger::revocationLeafHash(record));
  EXPECT_TRUE(proof.verify(ledger.root()));
  const auto restored = MerkleLedger::fromTsv(ledger.toTsv());
  EXPECT_EQ(restored.root(), ledger.root());
  ASSERT_EQ(restored.deviceRevocations().size(), 1U);
  EXPECT_EQ(restored.deviceRevocations().front(), record);
  const MerkleLedger copy = ledger;
  EXPECT_EQ(copy.root(), ledger.root());
  // Version 1 is refused by number, not read as a ledger without them.
  auto old = ledger.toTsv();
  old.replace(old.find("version\t2"), 9, "version\t1");
  EXPECT_THROW((void)MerkleLedger::fromTsv(old), std::runtime_error);
}

// ============================================================================
// Measurements (i3-3-design.md section 12, a1 rows)
// ============================================================================

TEST(SignedIngressMeasure, VerifyDecodeAndReplayCosts) {
  using Steady            = std::chrono::steady_clock;
  constexpr int kMessages = 10'000;
  const auto device       = newKey();
  std::vector<std::string> messages;
  messages.reserve(kMessages);
  for (int i = 1; i <= kMessages; ++i)
    messages.push_back(message(device,
                               headerFor(device, SignedKind::Observation, i),
                               observation("e" + std::to_string(i))));
  const auto perUs = [](Steady::duration d, int n) {
    return std::chrono::duration<double, std::micro>(d).count() / n;
  };

  auto t0 = Steady::now();
  for (const auto &m : messages)
    ASSERT_TRUE(decodeSignedMessage(m, kDefaultSignedMessageBytes));
  const auto decodeUs = perUs(Steady::now() - t0, kMessages);

  // The breakdown: the two bencode passes, the canonical belt, and a bare
  // domain-separated Ed25519 verify through the identity engine.
  const auto sample =
      *decodeSignedMessage(messages.front(), kMaxSignedMessageBytes);
  t0 = Steady::now();
  for (int i = 0; i < kMessages; ++i)
    (void)xanadu::bencode::decode(messages.front());
  const auto envelopeUs = perUs(Steady::now() - t0, kMessages);
  t0                    = Steady::now();
  for (int i = 0; i < kMessages; ++i)
    (void)xanadu::bencode::decode(sample.envelope.body);
  const auto bodyUs     = perUs(Steady::now() - t0, kMessages);
  const auto parsedBody = xanadu::bencode::decode(sample.envelope.body);
  t0                    = Steady::now();
  for (int i = 0; i < kMessages; ++i) (void)parsedBody.encode();
  const auto beltUs = perUs(Steady::now() - t0, kMessages);
  const id::StandardCryptoEngine verifier;
  const auto body = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t *>(sample.envelope.body.data()),
      sample.envelope.body.size());
  t0 = Steady::now();
  for (int i = 0; i < kMessages; ++i)
    ASSERT_TRUE(verifier.verifyWithDomain(sample.envelope.key,
                                          id::kDomainVagentSigned, body,
                                          sample.envelope.signature));
  const auto ed25519Us = perUs(Steady::now() - t0, kMessages);

  SignedIngressLimits limits;
  limits.verifyPerPeer = kMaxVerifyPerPeer;
  Rig rig(limits);
  const auto trust = pinned(device);
  std::vector<SignedHeader> headers;
  t0 = Steady::now();
  for (const auto &m : messages) {
    const auto r = rig.run(m, trust);
    ASSERT_TRUE(r);
    headers.push_back(r->header);
  }
  const auto verifyUs = perUs(Steady::now() - t0, kMessages);
  for (const auto &h : headers) rig.verifier.noteSettled(h);
  // The ring keeps the newest `replayRing` sequences; replay those.
  const auto ring   = static_cast<int>(limits.replayRing);
  const auto checks = rig.verifier.stats().signatureChecks;
  t0                = Steady::now();
  for (int i = 0; i < kMessages; ++i)
    ASSERT_EQ(refusalOf(rig.run(messages[kMessages - 1 - (i % ring)], trust)),
              EpistemicRefusal::Duplicate);
  const auto replayUs = perUs(Steady::now() - t0, kMessages);
  EXPECT_EQ(rig.verifier.stats().signatureChecks, checks);

  // Delegated, cached and uncached. Cycling more devices than the cache
  // holds through its FIFO makes every lookup a miss.
  constexpr int kDevices    = 2 * static_cast<int>(kMinDelegationCache);
  constexpr int kChains     = 1'000;
  SignedIngressLimits small = limits;
  small.delegationCache     = kMinDelegationCache;
  Rig chains(small);
  const auto master = newKey();
  addMaster(chains.ledger, master);
  std::vector<id::StandardCryptoEngine> devices;
  std::vector<std::vector<std::uint8_t>> certs;
  for (int i = 0; i < kDevices; ++i) {
    devices.push_back(newKey());
    certs.push_back(currentCert(master, devices.back().publicKey()).der);
  }
  std::vector<std::string> delegatedMessages;
  for (int i = 0; i < kChains; ++i) {
    const auto &d = devices[i % kDevices];
    delegatedMessages.push_back(
        message(d, headerFor(d, SignedKind::Heartbeat, i + 1),
                HeartbeatPayload{}, certs[i % kDevices]));
  }
  t0 = Steady::now();
  for (int i = 0; i < kChains; ++i)
    ASSERT_TRUE(chains.run(delegatedMessages[i],
                           delegated(devices[i % kDevices], master)));
  const auto uncachedUs = perUs(Steady::now() - t0, kChains);
  EXPECT_EQ(chains.verifier.stats().cacheHits, 0U);
  Rig warm(small);
  addMaster(warm.ledger, master);
  const auto &d0 = devices.front();
  std::vector<std::string> sameDevice;
  for (int i = 0; i < kChains; ++i)
    sameDevice.push_back(message(d0,
                                 headerFor(d0, SignedKind::Heartbeat, i + 1),
                                 HeartbeatPayload{}, certs.front()));
  t0 = Steady::now();
  for (const auto &m : sameDevice)
    ASSERT_TRUE(warm.run(m, delegated(d0, master)));
  const auto cachedUs = perUs(Steady::now() - t0, kChains);
  EXPECT_EQ(warm.verifier.stats().pathValidations, 1U);

  std::printf("[measure] signed decode: %.2f us/message (%d messages, %zu "
              "bytes each)\n",
              decodeUs, kMessages, messages.front().size());
  std::printf("[measure]   of which: envelope bencode %.2f us, body bencode "
              "%.2f us, canonical re-encode %.2f us\n",
              envelopeUs, bodyUs, beltUs);
  std::printf("[measure] bare Ed25519 verifyWithDomain: %.2f us/message\n",
              ed25519Us);
  std::printf("[measure] pinned verify (steps 1-5): %.2f us/message, %.0f "
              "messages/s/core\n",
              verifyUs, 1e6 / verifyUs);
  std::printf("[measure] replay refused at step 4: %.2f us/message\n",
              replayUs);
  std::printf("[measure] delegated verify, path validation uncached: %.2f "
              "us/message\n",
              uncachedUs);
  std::printf("[measure] delegated verify, cached: %.2f us/message\n",
              cachedUs);
  // Order-of-magnitude guards only; the figures are what is recorded.
  EXPECT_LT(replayUs, verifyUs);
  EXPECT_LT(cachedUs, uncachedUs);
}

} // namespace
