#include "common/xanadu/vagent/runtime/epistemic_adapter_wire.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <type_traits>
#include <utility>

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/identity/identity_engine.hpp"
#include "common/xanadu/identity/standard_crypto_engine.hpp"

#include <openssl/rand.h>

namespace xanadu::vagent::runtime {

namespace {

using bencode::Dict;
using bencode::Value;
using Refusal = std::unexpected<EpistemicRefusal>;

constexpr auto kMalformed = Refusal(EpistemicRefusal::MalformedMessage);

// Envelope, body, header and payload keys. One-letter outer keys keep the
// fixed overhead of a small reading small.
constexpr std::string_view kEnvBody        = "b";
constexpr std::string_view kEnvCertificate = "c";
constexpr std::string_view kEnvKey         = "k";
constexpr std::string_view kEnvSignature   = "s";
constexpr std::string_view kBodyHeader     = "h";
constexpr std::string_view kBodyPayload    = "p";

constexpr std::string_view kHKind         = "kind";
constexpr std::string_view kHIssuer       = "issuer";
constexpr std::string_view kHIncarnation  = "incarnation";
constexpr std::string_view kHSequence     = "sequence";
constexpr std::string_view kHAudience     = "audience";
constexpr std::string_view kHSent         = "sent";
constexpr std::string_view kHEpoch        = "epoch";
constexpr std::string_view kHBasis        = "basis";
constexpr std::string_view kHUncertainty  = "clock_uncertainty";
constexpr std::string_view kHExpires      = "expires";
constexpr std::string_view kHLowWatermark = "low_watermark";

constexpr std::string_view kPEventId     = "event_id";
constexpr std::string_view kPPredicate   = "predicate";
constexpr std::string_view kPPolicy      = "policy";
constexpr std::string_view kPTuple       = "tuple";
constexpr std::string_view kPAcquired    = "acquired";
constexpr std::string_view kPAcquiredUnc = "acquired_uncertainty";
constexpr std::string_view kPFrame       = "frame";
constexpr std::string_view kPUnits       = "units";
constexpr std::string_view kPVariance    = "variance";
constexpr std::string_view kPAction      = "action";
constexpr std::string_view kPCode        = "code";
constexpr std::string_view kPNonce       = "nonce";

constexpr std::string_view kKindNames[] = {
    "observation", "heartbeat", "health_report", "clock_pair", "verification"};
constexpr std::string_view kBasisNames[]  = {"synchronised", "free"};
constexpr std::string_view kActionNames[] = {"raise", "clear", "fail"};

/// How the kind table (3.2) treats an optional header field.
enum class Presence : std::uint8_t { Refused, Optional, Required };

struct KindRow {
  Presence expires;
  Presence lowWatermark;
};

constexpr KindRow rowOf(const SignedKind kind) {
  switch (kind) {
  case SignedKind::Observation:
    return {Presence::Optional, Presence::Refused};
  case SignedKind::Heartbeat:
  case SignedKind::HealthReport:
    return {Presence::Required, Presence::Refused};
  case SignedKind::ClockPair:
    return {Presence::Refused, Presence::Refused};
  case SignedKind::Verification:
    // V-D decides `expires`; until then it may ride along.
    return {Presence::Optional, Presence::Required};
  }
  return {Presence::Refused, Presence::Refused};
}

bool isLowerHex64(std::string_view text) {
  return text.size() == kSignedHexIdBytes &&
         std::ranges::all_of(text, [](const char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

/// A dictionary read field by field. `closed()` is true only if every key
/// was read, which is what makes each key set closed: a field nobody reads
/// is a field nobody checked. Each key is taken at most once, so a count is
/// enough to know that.
class Fields {
public:
  explicit Fields(const Dict &dict) : dict_(dict) {}

  [[nodiscard]] const Value *take(std::string_view key) {
    const auto found = dict_.find(std::string(key));
    if (found == dict_.end()) return nullptr;
    ++read_;
    return &found->second;
  }

  [[nodiscard]] bool closed() const { return read_ == dict_.size(); }

  /// A bounded Int at least @p least.
  [[nodiscard]] std::expected<std::int64_t, EpistemicRefusal>
  integer(std::string_view key, std::int64_t least = -kSignedIntBound + 1) {
    const auto *value = take(key);
    if (value == nullptr) return kMalformed;
    return checkedInteger(*value, least);
  }

  [[nodiscard]] std::expected<std::optional<std::int64_t>, EpistemicRefusal>
  optionalInteger(std::string_view key,
                  std::int64_t least = -kSignedIntBound + 1) {
    const auto *value = take(key);
    if (value == nullptr) return std::optional<std::int64_t>{};
    return checkedInteger(*value, least).transform([](std::int64_t v) {
      return std::optional<std::int64_t>{v};
    });
  }

  [[nodiscard]] std::expected<std::string, EpistemicRefusal>
  identifier(std::string_view key, std::size_t maxBytes) {
    const auto *value = take(key);
    if (value == nullptr || !value->isString() ||
        !isSignedIdentifier(value->asString(), maxBytes))
      return kMalformed;
    return value->asString();
  }

  [[nodiscard]] std::expected<std::optional<std::string>, EpistemicRefusal>
  optionalIdentifier(std::string_view key, std::size_t maxBytes) {
    if (!dict_.contains(std::string(key))) return std::optional<std::string>{};
    return identifier(key, maxBytes).transform([](std::string text) {
      return std::optional<std::string>{std::move(text)};
    });
  }

  [[nodiscard]] std::expected<std::string, EpistemicRefusal>
  hex64(std::string_view key) {
    const auto *value = take(key);
    if (value == nullptr || !value->isString() ||
        !isLowerHex64(value->asString()))
      return kMalformed;
    return value->asString();
  }

  /// One name from @p names, as its index.
  template <std::size_t N>
  [[nodiscard]] std::expected<std::size_t, EpistemicRefusal>
  oneOf(std::string_view key, const std::string_view (&names)[N]) {
    auto text = identifier(key, kMaxObservationNameBytes);
    if (!text) return std::unexpected(text.error());
    const auto *found = std::ranges::find(names, *text);
    if (found == std::end(names)) return kMalformed;
    return static_cast<std::size_t>(found - std::begin(names));
  }

  [[nodiscard]] std::expected<std::string, EpistemicRefusal>
  exactBytes(std::string_view key, std::size_t size) {
    const auto *value = take(key);
    if (value == nullptr || !value->isString() ||
        value->asString().size() != size)
      return kMalformed;
    return value->asString();
  }

private:
  static std::expected<std::int64_t, EpistemicRefusal>
  checkedInteger(const Value &value, std::int64_t least) {
    if (!value.isInteger()) return kMalformed;
    const auto v = value.asInteger();
    if (v <= -kSignedIntBound || v >= kSignedIntBound || v < least)
      return kMalformed;
    return v;
  }

  const Dict &dict_;
  std::size_t read_{};
};

std::expected<SignedHeader, EpistemicRefusal> decodeHeader(const Dict &dict) {
  Fields f(dict);
  SignedHeader h;
  // The kind first: an unknown kind is `UnsupportedKind` whatever else the
  // header holds, since its table row is what decides the rest.
  {
    const auto *value = f.take(kHKind);
    if (value == nullptr || !value->isString() ||
        !isSignedIdentifier(value->asString(), kMaxObservationNameBytes))
      return kMalformed;
    const auto *found = std::ranges::find(kKindNames, value->asString());
    if (found == std::end(kKindNames))
      return Refusal(EpistemicRefusal::UnsupportedKind);
    h.kind = static_cast<SignedKind>(found - std::begin(kKindNames));
  }
  auto issuer      = f.hex64(kHIssuer);
  auto incarnation = f.integer(kHIncarnation, 1);
  auto sequence    = f.integer(kHSequence, 0);
  auto audience    = f.hex64(kHAudience);
  auto sent        = f.integer(kHSent);
  auto epoch       = f.integer(kHEpoch, 0);
  auto basis       = f.oneOf(kHBasis, kBasisNames);
  auto uncertainty = f.integer(kHUncertainty);
  auto expires     = f.optionalInteger(kHExpires);
  auto watermark   = f.optionalInteger(kHLowWatermark, 0);
  if (!issuer || !incarnation || !sequence || !audience || !sent || !epoch ||
      !basis || !uncertainty || !expires || !watermark || !f.closed())
    return kMalformed;
  h.issuer           = std::move(*issuer);
  h.incarnation      = *incarnation;
  h.sequence         = *sequence;
  h.audience         = std::move(*audience);
  h.sent             = *sent;
  h.epoch            = *epoch;
  h.basis            = static_cast<SignedClockBasis>(*basis);
  h.clockUncertainty = *uncertainty;
  h.expires          = *expires;
  h.lowWatermark     = *watermark;
  const auto row     = rowOf(h.kind);
  const auto fits    = [](Presence p, bool present) {
    return p == Presence::Optional || (p == Presence::Required) == present;
  };
  if (!fits(row.expires, h.expires.has_value()) ||
      !fits(row.lowWatermark, h.lowWatermark.has_value()))
    return kMalformed;
  return h;
}

std::expected<ObservationPayload, EpistemicRefusal>
decodeObservation(const Dict &dict) {
  Fields f(dict);
  ObservationPayload o;
  auto eventId     = f.identifier(kPEventId, kMaxEventIdBytes);
  auto predicate   = f.identifier(kPPredicate, kMaxObservationNameBytes);
  auto policy      = f.hex64(kPPolicy);
  auto acquired    = f.integer(kPAcquired);
  auto uncertainty = f.integer(kPAcquiredUnc);
  auto frame       = f.optionalIdentifier(kPFrame, kMaxObservationNameBytes);
  auto units       = f.optionalIdentifier(kPUnits, kMaxObservationNameBytes);
  if (!eventId || !predicate || !policy || !acquired || !uncertainty ||
      !frame || !units)
    return kMalformed;
  if (const auto *variance = f.take(kPVariance)) {
    if (!variance->isString()) return kMalformed;
    o.variance = signedFloat(variance->asString());
    if (!o.variance) return kMalformed;
  }
  const auto *tuple = f.take(kPTuple);
  if (tuple == nullptr || !tuple->isList() || !f.closed()) return kMalformed;
  for (const auto &item : tuple->asList()) {
    if (item.isInteger()) {
      const auto v = item.asInteger();
      if (v <= -kSignedIntBound || v >= kSignedIntBound) return kMalformed;
      o.tuple.emplace_back(v);
    } else if (item.isString()) {
      o.tuple.emplace_back(item.asString());
    } else {
      return kMalformed;
    }
  }
  o.eventId             = std::move(*eventId);
  o.predicate           = std::move(*predicate);
  o.policy              = std::move(*policy);
  o.acquired            = *acquired;
  o.acquiredUncertainty = *uncertainty;
  o.frame               = frame->value_or("");
  o.units               = units->value_or("");
  return o;
}

std::expected<SignedPayload, EpistemicRefusal>
decodePayload(const SignedKind kind, const Dict &dict) {
  switch (kind) {
  case SignedKind::Observation:
    return decodeObservation(dict);
  case SignedKind::Heartbeat:
    if (!dict.empty()) return kMalformed;
    return HeartbeatPayload{};
  case SignedKind::HealthReport: {
    Fields f(dict);
    auto action = f.oneOf(kPAction, kActionNames);
    auto code   = f.identifier(kPCode, kMaxHealthCodeBytes);
    if (!action || !code || !f.closed()) return kMalformed;
    return HealthReportPayload{static_cast<ReportAction>(*action),
                               std::move(*code)};
  }
  case SignedKind::ClockPair: {
    Fields f(dict);
    auto nonce = f.exactBytes(kPNonce, kSignedNonceBytes);
    if (!nonce || !f.closed()) return kMalformed;
    ClockPairPayload pair;
    std::memcpy(pair.nonce.data(), nonce->data(), kSignedNonceBytes);
    return pair;
  }
  case SignedKind::Verification:
    break;
  }
  return Refusal(EpistemicRefusal::UnsupportedKind);
}

std::expected<SignedEnvelope, EpistemicRefusal>
decodeSignedEnvelope(std::string_view bytes) {
  Value outer;
  try {
    outer = bencode::decode(bytes);
  } catch (const std::exception &) {
    return kMalformed;
  }
  if (!outer.isDict()) return kMalformed;
  Fields f(outer.asDict());
  const auto *body = f.take(kEnvBody);
  auto key         = f.exactBytes(kEnvKey, sizeof(identity::PubKey32::bytes));
  auto signature =
      f.exactBytes(kEnvSignature, sizeof(identity::Signature64::bytes));
  const auto *certificate = f.take(kEnvCertificate);
  if (body == nullptr || !body->isString() || !key || !signature || !f.closed())
    return kMalformed;
  SignedEnvelope envelope;
  if (certificate != nullptr) {
    if (!certificate->isString() || certificate->asString().empty())
      return kMalformed;
    const auto &der = certificate->asString();
    envelope.certificate.emplace(der.begin(), der.end());
  }
  envelope.body = body->asString();
  std::memcpy(envelope.key.bytes.data(), key->data(), key->size());
  std::memcpy(envelope.signature.bytes.data(), signature->data(),
              signature->size());
  return envelope;
}

Value tupleItemValue(const SignedTupleItem &item) {
  return std::visit(
      [](const auto &v) {
        if constexpr (std::is_same_v<std::decay_t<decltype(v)>, std::int64_t>)
          return Value::integer(v);
        else
          return Value::string(v);
      },
      item);
}

Value payloadValue(const SignedPayload &payload) {
  return std::visit(
      [](const auto &p) {
        using P = std::decay_t<decltype(p)>;
        Dict d;
        if constexpr (std::is_same_v<P, ObservationPayload>) {
          d.emplace(kPEventId, Value::string(p.eventId));
          d.emplace(kPPredicate, Value::string(p.predicate));
          d.emplace(kPPolicy, Value::string(p.policy));
          bencode::List items;
          for (const auto &item : p.tuple)
            items.push_back(tupleItemValue(item));
          d.emplace(kPTuple, Value::list(std::move(items)));
          d.emplace(kPAcquired, Value::integer(p.acquired));
          d.emplace(kPAcquiredUnc, Value::integer(p.acquiredUncertainty));
          if (!p.frame.empty()) d.emplace(kPFrame, Value::string(p.frame));
          if (!p.units.empty()) d.emplace(kPUnits, Value::string(p.units));
          if (p.variance)
            d.emplace(kPVariance,
                      Value::string(encodeSignedFloat(*p.variance)));
        } else if constexpr (std::is_same_v<P, HealthReportPayload>) {
          d.emplace(kPAction, Value::string(std::string(toString(p.action))));
          d.emplace(kPCode, Value::string(p.code));
        } else if constexpr (std::is_same_v<P, ClockPairPayload>) {
          d.emplace(kPNonce,
                    Value::string(std::string(p.nonce.begin(), p.nonce.end())));
        }
        return Value::dict(std::move(d));
      },
      payload);
}

} // namespace

std::string_view toString(const SignedKind kind) noexcept {
  return kKindNames[static_cast<std::size_t>(kind)];
}
std::string_view toString(const SignedClockBasis basis) noexcept {
  return kBasisNames[static_cast<std::size_t>(basis)];
}
std::string_view toString(const ReportAction action) noexcept {
  return kActionNames[static_cast<std::size_t>(action)];
}

std::expected<SignedMessage, EpistemicRefusal>
decodeSignedMessage(const std::string_view bytes, const std::size_t maxBytes) {
  if (bytes.size() > maxBytes)
    return Refusal(EpistemicRefusal::OversizeMessage);
  auto envelope = decodeSignedEnvelope(bytes);
  if (!envelope) return std::unexpected(envelope.error());
  Value body;
  try {
    body = bencode::decode(envelope->body);
  } catch (const bencode::NonCanonicalError &) {
    return Refusal(EpistemicRefusal::NonCanonicalMessage);
  } catch (const std::exception &) {
    return kMalformed;
  }
  // The decoder refuses every non-canonical form it knows; this is the belt
  // over its braces, for a form nobody thought of. One encode.
  if (body.encode() != envelope->body)
    return Refusal(EpistemicRefusal::NonCanonicalMessage);
  if (!body.isDict()) return kMalformed;
  Fields f(body.asDict());
  const auto *header  = f.take(kBodyHeader);
  const auto *payload = f.take(kBodyPayload);
  if (header == nullptr || payload == nullptr || !header->isDict() ||
      !payload->isDict() || !f.closed())
    return kMalformed;
  auto decodedHeader = decodeHeader(header->asDict());
  if (!decodedHeader) return std::unexpected(decodedHeader.error());
  auto decodedPayload = decodePayload(decodedHeader->kind, payload->asDict());
  if (!decodedPayload) return std::unexpected(decodedPayload.error());
  return SignedMessage{std::move(*envelope), std::move(*decodedHeader),
                       std::move(*decodedPayload)};
}

std::string encodeSignedBody(const SignedHeader &header,
                             const SignedPayload &payload) {
  Dict h;
  h.emplace(kHKind, Value::string(std::string(toString(header.kind))));
  h.emplace(kHIssuer, Value::string(header.issuer));
  h.emplace(kHIncarnation, Value::integer(header.incarnation));
  h.emplace(kHSequence, Value::integer(header.sequence));
  h.emplace(kHAudience, Value::string(header.audience));
  h.emplace(kHSent, Value::integer(header.sent));
  h.emplace(kHEpoch, Value::integer(header.epoch));
  h.emplace(kHBasis, Value::string(std::string(toString(header.basis))));
  h.emplace(kHUncertainty, Value::integer(header.clockUncertainty));
  if (header.expires) h.emplace(kHExpires, Value::integer(*header.expires));
  if (header.lowWatermark)
    h.emplace(kHLowWatermark, Value::integer(*header.lowWatermark));
  Dict body;
  body.emplace(kBodyHeader, Value::dict(std::move(h)));
  body.emplace(kBodyPayload, payloadValue(payload));
  return Value::dict(std::move(body)).encode();
}

std::string encodeSignedEnvelope(const SignedEnvelope &envelope) {
  Dict d;
  d.emplace(kEnvBody, Value::string(envelope.body));
  if (envelope.certificate)
    d.emplace(kEnvCertificate,
              Value::string(std::string(envelope.certificate->begin(),
                                        envelope.certificate->end())));
  d.emplace(kEnvKey, Value::string(std::string(envelope.key.bytes.begin(),
                                               envelope.key.bytes.end())));
  d.emplace(kEnvSignature,
            Value::string(std::string(envelope.signature.bytes.begin(),
                                      envelope.signature.bytes.end())));
  return Value::dict(std::move(d)).encode();
}

std::string signedBytesOf(const std::string_view body) {
  std::string out(identity::kDomainVagentSigned);
  out += body;
  return out;
}

std::expected<SignedEnvelope, identity::ValidationError>
signEnvelope(const identity::StandardCryptoEngine &device, std::string body,
             std::optional<std::vector<std::uint8_t>> certificate) {
  const auto signature = device.signWithDomain(
      identity::kDomainVagentSigned,
      {reinterpret_cast<const std::uint8_t *>(body.data()), body.size()});
  if (!signature) return std::unexpected(signature.error());
  return SignedEnvelope{std::move(body), std::move(certificate),
                        device.publicKey(), *signature};
}

std::string issuerOf(const identity::PubKey32 &key) {
  return identity::StandardCryptoEngine::computeFingerprintSha256(key);
}

std::string encodeSignedFloat(const double value) {
  auto bits = std::bit_cast<std::uint64_t>(value);
  std::string out(kSignedFloatBytes, '\0');
  for (std::size_t i = kSignedFloatBytes; i-- > 0; bits >>= 8U)
    out[i] = static_cast<char>(bits & 0xFFU);
  return out;
}

std::optional<double> signedFloat(const std::string_view bytes) {
  if (bytes.size() != kSignedFloatBytes) return std::nullopt;
  std::uint64_t bits = 0;
  for (const char c : bytes)
    bits = (bits << 8U) | static_cast<unsigned char>(c);
  const auto value = std::bit_cast<double>(bits);
  // `-0` is refused rather than normalised, so two byte strings never mean
  // one reading.
  if (!std::isfinite(value) || (value == 0.0 && std::signbit(value)))
    return std::nullopt;
  return value;
}

std::optional<bool> signedBool(const std::int64_t value) {
  if (value == 0) return false;
  if (value == 1) return true;
  return std::nullopt;
}

bool isSignedIdentifier(const std::string_view text,
                        const std::size_t maxBytes) {
  return !text.empty() && text.size() <= maxBytes &&
         std::ranges::all_of(text, [](const char c) {
           const auto u = static_cast<unsigned char>(c);
           return u >= 0x21U && u <= 0x7EU;
         });
}

bool isSignedText(const std::string_view text, const std::size_t maxBytes) {
  if (text.size() > maxBytes) return false;
  std::size_t i = 0;
  while (i < text.size()) {
    const auto lead      = static_cast<unsigned char>(text[i]);
    std::size_t length   = 0;
    std::uint32_t code   = 0;
    std::uint32_t lowest = 0;
    if (lead < 0x80U) {
      ++i;
      continue;
    }
    if ((lead & 0xE0U) == 0xC0U) {
      length = 2, code = lead & 0x1FU, lowest = 0x80U;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3, code = lead & 0x0FU, lowest = 0x800U;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4, code = lead & 0x07U, lowest = 0x10000U;
    } else {
      return false;
    }
    if (text.size() - i < length) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0U) != 0x80U) return false;
      code = (code << 6U) | (next & 0x3FU);
    }
    if (code < lowest || code > 0x10FFFFU ||
        (code >= 0xD800U && code <= 0xDFFFU))
      return false;
    i += length;
  }
  return true;
}

std::optional<std::string> mintAudienceToken() {
  std::array<unsigned char, kSignedHexIdBytes / 2> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(kSignedHexIdBytes);
  for (const auto b : bytes) {
    out.push_back(digits[b >> 4U]);
    out.push_back(digits[b & 0xFU]);
  }
  return out;
}

} // namespace xanadu::vagent::runtime
