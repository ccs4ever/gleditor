#include "common/xanadu/evidence/effect_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

namespace xanadu::evidence {
namespace {
using Attributes = std::map<std::string, std::string>;
std::string now() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
template <class T> T integer(std::string_view text) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error("invalid retained effect integer");
  return value;
}
void putValue(Attributes &attributes, const std::string &prefix,
              const Value &value) {
  constexpr std::array types{StorageClass::Null, StorageClass::Integer,
                             StorageClass::Real, StorageClass::Text,
                             StorageClass::Blob};
  attributes[prefix + "-type"] =
      std::string(storageName(types.at(value.index())));
  std::string bytes;
  if (const auto *v = std::get_if<std::int64_t>(&value))
    bytes = std::to_string(*v);
  else if (const auto *v = std::get_if<double>(&value))
    bytes = std::to_string(std::bit_cast<std::uint64_t>(*v));
  else if (const auto *v = std::get_if<std::string>(&value))
    bytes = *v;
  else if (const auto *v = std::get_if<Binary>(&value))
    bytes = v->bytes;
  attributes[prefix + "-bytes"] = std::move(bytes);
}
Value getValue(const Attributes &attributes, const std::string &prefix) {
  const auto &type  = attributes.at(prefix + "-type");
  const auto &bytes = attributes.at(prefix + "-bytes");
  if (type == storageName(StorageClass::Null)) return std::monostate{};
  if (type == storageName(StorageClass::Integer))
    return integer<std::int64_t>(bytes);
  if (type == storageName(StorageClass::Real))
    return std::bit_cast<double>(integer<std::uint64_t>(bytes));
  if (type == storageName(StorageClass::Text)) return bytes;
  if (type == storageName(StorageClass::Blob)) return Binary{bytes};
  throw std::runtime_error("unknown retained effect value type");
}
Attributes encode(const EffectRequest &request) {
  Attributes a{
      {"operation", std::string(effectKindName(request.kind))},
      {"target", request.target.empty() ? canonicalEffectTarget(request)
                                        : request.target},
      {"cwd", request.cwd},
      {"stdin", request.stdinBytes},
      {"sql", request.sql},
      {"timeout-ms", std::to_string(request.limits.timeoutMs)},
      {"output-budget", std::to_string(request.limits.maxOutputBytes)},
      {"input-budget", std::to_string(request.limits.maxInputBytes)},
      {"field-budget", std::to_string(request.limits.maxFields)},
      {"terminate-grace-ms", std::to_string(request.limits.terminateGraceMs)},
      {"affected-rows", std::to_string(request.expectedAffectedRows)},
      {"fake-delay-ms", std::to_string(request.fakeDelayMs)},
      {"fake-unknown", request.fakeUnknown ? "yes" : "no"},
      {"argc", std::to_string(request.argv.size())},
      {"envc", std::to_string(request.environment.size())},
      {"paramc", std::to_string(request.parameters.size())},
      {"guard", request.guard ? "yes" : "no"}};
  if (request.fediversePost) {
    const auto &post              = *request.fediversePost;
    a["fediverse-origin"]         = post.instanceOrigin;
    a["fediverse-account"]        = post.accountId;
    a["fediverse-text"]           = post.text;
    a["fediverse-warning"]        = post.contentWarning;
    a["fediverse-visibility"]     = post.visibility;
    a["fediverse-reply"]          = post.replyToRestId.value_or("");
    a["fediverse-has-reply"]      = post.replyToRestId ? "yes" : "no";
    a["fediverse-idempotency"]    = post.idempotencyKey;
    a["fediverse-actor"]          = post.accountActorUri;
    a["fediverse-audience-count"] = std::to_string(post.audience.size());
    a["fediverse-max-pages"]      = std::to_string(post.limits.maxPages);
    a["fediverse-timeout-ms"]     = std::to_string(post.limits.timeoutMs);
    a["fediverse-header-budget"]  = std::to_string(post.limits.maxHeaderBytes);
    a["fediverse-record-budget"] =
        std::to_string(post.limits.records.maxRecords);
    a["fediverse-byte-budget"] = std::to_string(post.limits.records.maxBytes);
    a["fediverse-string-budget"] =
        std::to_string(post.limits.records.maxStringBytes);
    a["fediverse-part-budget"] = std::to_string(post.limits.records.maxParts);
    a["fediverse-depth-budget"] =
        std::to_string(post.limits.records.maxPartDepth);
    for (std::size_t i = 0; i < post.audience.size(); ++i)
      a["fediverse-audience-" + std::to_string(i)] = post.audience[i];
  }
  if (request.ircSend) {
    const auto &send        = *request.ircSend;
    a["irc-server"]         = send.serverAuthority;
    a["irc-network"]        = send.networkIncarnation;
    a["irc-connection"]     = send.connectionIncarnation;
    a["irc-target"]         = send.target;
    a["irc-payload"]        = send.payload;
    a["irc-audience-count"] = std::to_string(send.audience.size());
    for (std::size_t i = 0; i < send.audience.size(); ++i)
      a["irc-audience-" + std::to_string(i)] = send.audience[i];
  }
  if (request.mailSubmit) {
    const auto &mail           = *request.mailSubmit;
    a["mail-server-url"]       = mail.profile.serverUrl;
    a["mail-server-authority"] = mail.profile.serverAuthority;
    a["mail-account"]          = mail.profile.accountScope;
    a["mail-ca-bundle"]        = mail.profile.caBundlePath;
    a["mail-allow-cleartext"] =
        mail.profile.allowCleartextLoopbackWithoutCredentials ? "yes" : "no";
    a["mail-timeout-ms"]    = std::to_string(mail.profile.timeoutMilliseconds);
    a["mail-message-id"]    = mail.draft.messageId;
    a["mail-envelope-from"] = mail.draft.envelopeFrom;
    a["mail-envelope-count"] =
        std::to_string(mail.draft.envelopeRecipients.size());
    a["mail-draft-bytes"]    = mail.draft.bytes;
    a["mail-correlation-id"] = mail.draft.correlationId;
    for (std::size_t i = 0; i < mail.draft.envelopeRecipients.size(); ++i)
      a["mail-envelope-" + std::to_string(i)] =
          mail.draft.envelopeRecipients[i];
  }
  for (std::size_t i = 0; i < request.argv.size(); ++i)
    a["arg-" + std::to_string(i)] = request.argv[i];
  std::size_t i{};
  for (const auto &[key, value] : request.environment) {
    const auto p    = "env-" + std::to_string(i++);
    a[p + "-key"]   = key;
    a[p + "-value"] = value;
  }
  for (i = 0; i < request.parameters.size(); ++i)
    putValue(a, "param-" + std::to_string(i), request.parameters[i]);
  if (request.guard) {
    a["guard-query"]  = request.guard->query;
    a["guard-paramc"] = std::to_string(request.guard->parameters.size());
    for (i = 0; i < request.guard->parameters.size(); ++i)
      putValue(a, "guard-param-" + std::to_string(i),
               request.guard->parameters[i]);
    putValue(a, "guard-expected", request.guard->expected);
  }
  return a;
}
EffectRequest decode(const Record &record) {
  if (record.kind != "effect-intent")
    throw std::invalid_argument("record is not an effect intent");
  const auto &a = record.attributes;
  EffectRequest r;
  const auto &kind = a.at("operation");
  if (kind == "fake")
    r.kind = EffectKind::DelayedFake;
  else if (kind == "sql-patch")
    r.kind = EffectKind::SqlPatch;
  else if (kind == "process")
    r.kind = EffectKind::Process;
  else if (kind == "fediverse-post")
    r.kind = EffectKind::FediversePost;
  else if (kind == "irc-send")
    r.kind = EffectKind::IrcSend;
  else if (kind == "mail-submit")
    r.kind = EffectKind::MailSubmit;
  else
    throw std::runtime_error("unknown retained effect operation");
  r.target               = a.at("target");
  r.cwd                  = a.at("cwd");
  r.stdinBytes           = a.at("stdin");
  r.sql                  = a.at("sql");
  r.limits               = {integer<std::uint64_t>(a.at("timeout-ms")),
                            integer<std::uint64_t>(a.at("output-budget")),
                            integer<std::uint64_t>(a.at("input-budget")),
                            integer<std::uint64_t>(a.at("field-budget")),
                            integer<std::uint64_t>(a.at("terminate-grace-ms"))};
  r.expectedAffectedRows = integer<std::uint64_t>(a.at("affected-rows"));
  r.fakeDelayMs          = integer<std::uint64_t>(a.at("fake-delay-ms"));
  r.fakeUnknown          = a.at("fake-unknown") == "yes";
  const auto count       = [&](const char *name) {
    auto n = integer<std::size_t>(a.at(name));
    if (n > a.size())
      throw std::runtime_error("retained effect count exceeds its fields");
    return n;
  };
  for (std::size_t i = 0; i < count("argc"); ++i)
    r.argv.push_back(a.at("arg-" + std::to_string(i)));
  for (std::size_t i = 0; i < count("envc"); ++i) {
    const auto p = "env-" + std::to_string(i);
    r.environment.emplace(a.at(p + "-key"), a.at(p + "-value"));
  }
  for (std::size_t i = 0; i < count("paramc"); ++i)
    r.parameters.push_back(getValue(a, "param-" + std::to_string(i)));
  if (a.at("guard") == "yes") {
    r.guard = SqlGuard{.query    = a.at("guard-query"),
                       .expected = getValue(a, "guard-expected")};
    for (std::size_t i = 0; i < count("guard-paramc"); ++i)
      r.guard->parameters.push_back(
          getValue(a, "guard-param-" + std::to_string(i)));
  }
  if (r.kind == EffectKind::FediversePost) {
    FediversePostRequest post;
    post.instanceOrigin = a.at("fediverse-origin");
    post.accountId      = a.at("fediverse-account");
    post.text           = a.at("fediverse-text");
    post.contentWarning = a.at("fediverse-warning");
    post.visibility     = a.at("fediverse-visibility");
    if (a.at("fediverse-has-reply") == "yes")
      post.replyToRestId = a.at("fediverse-reply");
    post.idempotencyKey   = a.at("fediverse-idempotency");
    post.accountActorUri  = a.at("fediverse-actor");
    post.limits.maxPages  = integer<std::size_t>(a.at("fediverse-max-pages"));
    post.limits.timeoutMs = integer<long>(a.at("fediverse-timeout-ms"));
    post.limits.maxHeaderBytes =
        integer<std::size_t>(a.at("fediverse-header-budget"));
    post.limits.records.maxRecords =
        integer<std::size_t>(a.at("fediverse-record-budget"));
    post.limits.records.maxBytes =
        integer<std::size_t>(a.at("fediverse-byte-budget"));
    post.limits.records.maxStringBytes =
        integer<std::size_t>(a.at("fediverse-string-budget"));
    post.limits.records.maxParts =
        integer<std::size_t>(a.at("fediverse-part-budget"));
    post.limits.records.maxPartDepth =
        integer<std::size_t>(a.at("fediverse-depth-budget"));
    for (std::size_t i = 0;
         i < integer<std::size_t>(a.at("fediverse-audience-count")); ++i)
      post.audience.push_back(a.at("fediverse-audience-" + std::to_string(i)));
    r.fediversePost = std::move(post);
  }
  if (r.kind == EffectKind::IrcSend) {
    IrcSendRequest send;
    send.serverAuthority       = a.at("irc-server");
    send.networkIncarnation    = a.at("irc-network");
    send.connectionIncarnation = a.at("irc-connection");
    send.target                = a.at("irc-target");
    send.payload               = a.at("irc-payload");
    for (std::size_t i = 0;
         i < integer<std::size_t>(a.at("irc-audience-count")); ++i)
      send.audience.push_back(a.at("irc-audience-" + std::to_string(i)));
    r.ircSend = std::move(send);
  }
  if (r.kind == EffectKind::MailSubmit) {
    MailSubmitRequest mail;
    mail.profile.serverUrl       = a.at("mail-server-url");
    mail.profile.serverAuthority = a.at("mail-server-authority");
    mail.profile.accountScope    = a.at("mail-account");
    mail.profile.caBundlePath    = a.at("mail-ca-bundle");
    mail.profile.allowCleartextLoopbackWithoutCredentials =
        a.at("mail-allow-cleartext") == "yes";
    mail.profile.timeoutMilliseconds = integer<long>(a.at("mail-timeout-ms"));
    mail.draft.messageId             = a.at("mail-message-id");
    mail.draft.envelopeFrom          = a.at("mail-envelope-from");
    mail.draft.bytes                 = a.at("mail-draft-bytes");
    mail.draft.correlationId         = a.at("mail-correlation-id");
    for (std::size_t i = 0;
         i < integer<std::size_t>(a.at("mail-envelope-count")); ++i)
      mail.draft.envelopeRecipients.push_back(
          a.at("mail-envelope-" + std::to_string(i)));
    r.mailSubmit = std::move(mail);
  }
  if (const auto problem = effectProblem(r)) throw std::runtime_error(*problem);
  return r;
}
} // namespace

struct EffectRuntime::Impl {
  EvidenceCase &active;
  std::shared_ptr<UserPermascroll> scroll;
  std::filesystem::path path;
  std::string document;
  std::string executor{UserPermascroll::generateUniqueDeviceId()};
  EffectRuntimeOptions options;
  FediverseTransport fediverse;
  std::map<RecordId, std::shared_ptr<FediverseCredentialLease>>
      fediverseCredentials;
  std::map<RecordId, std::shared_ptr<IrcSession>> ircSessions;
  std::map<RecordId, std::shared_ptr<MailCredentialLease>>
      mailCredentialsByIntent;
  std::map<RecordId, std::weak_ptr<FediverseAuthority>> fediverseAuthorities;
  struct Grant {
    RecordId intent;
    std::shared_ptr<EffectAuthority> authority;
    std::shared_ptr<std::atomic_bool> protocolRevoked{
        std::make_shared<std::atomic_bool>()};
  };
  std::map<RecordId, Grant> grants;
  std::optional<RecordId> attempt;
  std::atomic_bool cancelled{};
  std::atomic_bool running{};
  std::thread worker;
  mutable std::mutex guard;
  std::optional<EffectResult> acceptance;
  std::optional<EffectResult> completion;

  Impl(EvidenceCase &e, std::shared_ptr<UserPermascroll> s,
       EffectRuntimeOptions o)
      : active(e), scroll(std::move(s)),
        path(std::filesystem::canonical(e.path())),
        document(e.store().documentId().str()), options(o),
        fediverse(o.fediverse) {
    if (!e.isOpen()) throw std::invalid_argument("open a private case first");
  }
  bool activeMatches() const {
    if (!active.isOpen() || active.store().documentId().str() != document)
      return false;
    std::error_code error;
    return std::filesystem::canonical(active.path(), error) == path && !error;
  }
  void requireActive() const {
    if (!activeMatches())
      throw std::logic_error("effect runtime is bound to a different case");
  }
  std::filesystem::path receiptRoot() const {
    if (!scroll->config().storageDir.empty())
      return scroll->config().storageDir / "effect-receipts";
    return path.parent_path().empty() ? std::filesystem::path(".")
                                      : path.parent_path();
  }
  std::string receiptPrefix() const {
    return (scroll->config().storageDir.empty()
                ? path.filename().string() + ".receipt-"
                : std::string{}) +
           document + "-";
  }
  void receipt(EvidenceCase &journal, RecordId parent,
               const EffectResult &result, std::string_view completionId = {},
               bool accepted = false) {
    Attributes attributes{
        {"outcome",
         accepted ? "accepted" : std::string(outcomeName(result.outcome))},
        {"resolved-target", result.resolvedTarget},
        {"may-have-executed", result.mayHaveExecuted ? "yes" : "no"},
        {"finished-unix-ms", now()},
        {"executor", executor}};
    if (!completionId.empty()) attributes["completion-id"] = completionId;
    if (result.exitCode)
      attributes["exit-code"] = std::to_string(*result.exitCode);
    if (result.signal) attributes["signal"] = std::to_string(*result.signal);
    if (result.affectedRows)
      attributes["affected-rows"] = std::to_string(*result.affectedRows);
    if (result.fediversePost) {
      const auto &post          = *result.fediversePost;
      attributes["protocol"]    = "fediverse-rest-post";
      attributes["http-status"] = std::to_string(post.httpStatus);
      attributes["post-state"]  = std::to_string(static_cast<int>(post.state));
      attributes["post-accepted"]             = post.accepted ? "yes" : "no";
      attributes["post-rest-id"]              = post.restId;
      attributes["post-object-uri"]           = post.objectUri.value_or("");
      attributes["post-actor-uri"]            = post.actorUri.value_or("");
      attributes["post-actor-identity-basis"] = post.actorIdentityBasis;
      attributes["post-activity-uri"]         = post.activityUri.value_or("");
      attributes["post-idempotency-key"]      = post.idempotencyKey;
      attributes["post-visibility"]           = post.visibility;
      attributes["post-error"]                = post.error;
      attributes["post-federation-delivery"] =
          post.federationDeliveryConfirmed ? "yes" : "no";
      attributes["post-response-redacted"] =
          post.responseRedacted ? "yes" : "no";
      attributes["post-response-kind"] =
          post.responseRedacted ? "withheld-derivative" : "http-entity-body";
      attributes["post-response-body"]  = post.responseBody;
      attributes["post-audience-count"] = std::to_string(post.audience.size());
      for (std::size_t i = 0; i < post.audience.size(); ++i)
        attributes["post-audience-" + std::to_string(i)] = post.audience[i];
    }
    if (result.ircSend) {
      const auto &send       = *result.ircSend;
      attributes["protocol"] = "irc-send";
      attributes["irc-send-state"] =
          std::to_string(static_cast<int>(send.state));
      attributes["irc-server-reply"] = send.serverReply;
      attributes["irc-echo-id"]      = send.echoMessageId.value_or("");
      attributes["irc-detail"]       = send.detail;
    }
    if (result.smtpSubmission) {
      const auto &smtp         = *result.smtpSubmission;
      attributes["protocol"]   = "smtp-submission";
      attributes["smtp-state"] = std::to_string(static_cast<int>(smtp.state));
      attributes["smtp-data-started"] = smtp.dataStarted ? "yes" : "no";
      attributes["smtp-final-response-code"] =
          std::to_string(smtp.finalResponseCode);
      attributes["smtp-correlation-id"]          = smtp.correlationId;
      attributes["smtp-diagnostic"]              = smtp.safeDiagnostic;
      attributes["local-acceptance-is-delivery"] = "no";
      attributes["smtp-recipient-count"] =
          std::to_string(smtp.recipients.size());
      for (std::size_t i = 0; i < smtp.recipients.size(); ++i) {
        const auto &recipient           = smtp.recipients[i];
        const auto prefix               = "smtp-recipient-" + std::to_string(i);
        attributes[prefix + "-address"] = recipient.address;
        attributes[prefix + "-state"] =
            std::to_string(static_cast<int>(recipient.state));
        attributes[prefix + "-response-code"] =
            std::to_string(recipient.responseCode);
      }
    }
    const auto id = journal.appendRecord(
        accepted ? "effect-acceptance" : "effect-receipt",
        accepted ? "accepted" : std::string(outcomeName(result.outcome)),
        result.detail, parent, attributes);
    if (!result.stdoutBytes.empty())
      std::ignore =
          journal.appendRecord("effect-output", "stdout", result.stdoutBytes,
                               id, {{"stream", "stdout"}});
    if (!result.stderrBytes.empty())
      std::ignore =
          journal.appendRecord("effect-output", "stderr", result.stderrBytes,
                               id, {{"stream", "stderr"}});
    if (result.fediversePost) {
      const auto &post = *result.fediversePost;
      std::ignore      = journal.appendRecord(
          "fediverse-post-receipt", post.accepted ? "accepted" : "not-accepted",
          post.error, id,
          {{"http-status", std::to_string(post.httpStatus)},
                {"rest-id", post.restId},
                {"object-uri", post.objectUri.value_or("")},
                {"actor-uri", post.actorUri.value_or("")},
                {"actor-identity-basis", post.actorIdentityBasis},
                {"activity-uri", post.activityUri.value_or("")},
                {"visibility", post.visibility},
                {"idempotency-key", post.idempotencyKey},
                {"federation-delivery-confirmed",
            post.federationDeliveryConfirmed ? "yes" : "no"}});
      if (!post.responseBody.empty() || post.responseRedacted)
        std::ignore = journal.appendRecord(
            "fediverse-post-response",
            post.responseRedacted ? "withheld-derivative" : "http-entity-body",
            post.responseBody, id,
            {{"redacted", post.responseRedacted ? "yes" : "no"}});
      for (std::size_t i = 0; i < post.audience.size(); ++i)
        std::ignore = journal.appendRecord(
            "fediverse-recipient-outcome", std::to_string(i),
            post.accepted
                ? "REST acceptance does not confirm recipient delivery"
                : "no REST acceptance for this requested audience",
            id,
            {{"recipient", post.audience[i]},
             {"outcome",
              post.accepted ? "accepted-unconfirmed" : "not-accepted"}});
    }
    if (result.ircSend) {
      const auto &send = *result.ircSend;
      std::ignore      = journal.appendRecord(
          "irc-send-receipt", std::to_string(static_cast<int>(send.state)),
          send.detail, id,
          {{"server-reply", send.serverReply},
                {"echo-message-id", send.echoMessageId.value_or("")}});
    }
    if (result.smtpSubmission) {
      const auto &smtp = *result.smtpSubmission;
      std::ignore      = journal.appendRecord(
          "smtp-submission-receipt",
          std::to_string(static_cast<int>(smtp.state)), smtp.safeDiagnostic, id,
          {{"data-started", smtp.dataStarted ? "yes" : "no"},
                {"final-response-code", std::to_string(smtp.finalResponseCode)},
                {"correlation-id", smtp.correlationId},
                {"local-acceptance-is-delivery", "no"}});
      for (std::size_t i = 0; i < smtp.recipients.size(); ++i) {
        const auto &recipient = smtp.recipients[i];
        std::ignore           = journal.appendRecord(
            "smtp-recipient-outcome", std::to_string(i), recipient.address, id,
            {{"state", std::to_string(static_cast<int>(recipient.state))},
                       {"response-code", std::to_string(recipient.responseCode)},
                       {"inbox-delivery-confirmed", "no"}});
      }
    }
    journal.durableSave();
  }
};

EffectRuntime::EffectRuntime(EvidenceCase &e,
                             std::shared_ptr<UserPermascroll> s,
                             EffectRuntimeOptions options)
    : impl_(std::make_unique<Impl>(e, std::move(s), options)) {
  recover();
}
EffectRuntime::~EffectRuntime() {
  impl_->cancelled = true;
  if (impl_->worker.joinable()) impl_->worker.join();
  try {
    std::ignore = poll();
  } catch (...) {
    // The durable admission remains outcome-unknown if receipt storage fails.
  }
}

RecordId EffectRuntime::draft(const EffectRequest &request) {
  impl_->requireActive();
  if (const auto problem = effectProblem(request))
    throw std::invalid_argument(*problem);
  auto attributes                  = encode(request);
  attributes["canonical-target"]   = canonicalEffectTarget(request);
  attributes["target-fingerprint"] = effectTargetFingerprint(request);
  attributes["created-unix-ms"]    = now();
  const auto id                    = impl_->active.appendRecord(
      "effect-intent", std::string(effectKindName(request.kind)),
      "Immutable reviewed effect request", {}, attributes);
  impl_->active.durableSave();
  return id;
}
EffectRequest EffectRuntime::request(RecordId id) const {
  impl_->requireActive();
  return decode(impl_->active.inspect(id));
}
void EffectRuntime::bindFediverseCredential(
    RecordId intent, std::shared_ptr<FediverseCredentialLease> credential) {
  impl_->requireActive();
  const auto request = this->request(intent);
  if (request.kind != EffectKind::FediversePost || !request.fediversePost ||
      !credential ||
      !credential->matches(request.fediversePost->instanceOrigin,
                           request.fediversePost->accountId) ||
      !credential->usable())
    throw std::invalid_argument(
        "Fediverse credential does not match the exact post intent");
  impl_->fediverseCredentials.insert_or_assign(intent, std::move(credential));
}
void EffectRuntime::bindIrcSession(RecordId intent,
                                   std::shared_ptr<IrcSession> session) {
  impl_->requireActive();
  const auto request = this->request(intent);
  if (request.kind != EffectKind::IrcSend || !request.ircSend || !session)
    throw std::invalid_argument(
        "IRC session does not match an IRC send intent");
  const auto identity = session->identity();
  if (identity.serverAuthority != request.ircSend->serverAuthority ||
      identity.networkIncarnation != request.ircSend->networkIncarnation ||
      identity.connectionIncarnation != request.ircSend->connectionIncarnation)
    throw std::invalid_argument(
        "IRC session identity does not match the send intent");
  impl_->ircSessions.insert_or_assign(intent, std::move(session));
}
void EffectRuntime::bindMailCredential(
    RecordId intent, std::shared_ptr<MailCredentialLease> credential) {
  impl_->requireActive();
  const auto request = this->request(intent);
  if (request.kind != EffectKind::MailSubmit || !request.mailSubmit ||
      !credential)
    throw std::invalid_argument(
        "mail credential does not match a mail submission intent");
  const auto &profile = request.mailSubmit->profile;
  const MailCredentialScope scope{profile.serverAuthority, profile.accountScope,
                                  MailCredentialOperation::Submit};
  if (!credential->matches(scope))
    throw std::invalid_argument(
        "mail credential scope does not match the exact submission intent");
  impl_->mailCredentialsByIntent.insert_or_assign(intent,
                                                  std::move(credential));
}
RecordId EffectRuntime::authorize(RecordId intent, EffectGrantPolicy policy) {
  if (policy.lifetime.count() <= 0 || policy.lifetime > std::chrono::hours(24))
    throw std::invalid_argument(
        "authority lifetime must be positive and at most one day");
  if (!busy()) impl_->cancelled = false;
  const auto r = request(intent);
  if (r.kind == EffectKind::FediversePost &&
      (!impl_->fediverseCredentials.contains(intent) ||
       !impl_->fediverseCredentials.at(intent)->usable()))
    throw std::runtime_error(
        "bind a live Fediverse credential before authorizing");
  if (r.kind == EffectKind::IrcSend && !impl_->ircSessions.contains(intent))
    throw std::runtime_error("bind a live IRC session before authorizing");
  if (r.kind == EffectKind::MailSubmit &&
      (!impl_->mailCredentialsByIntent.contains(intent) ||
       !impl_->mailCredentialsByIntent.at(intent)->matches(
           {r.mailSubmit->profile.serverAuthority,
            r.mailSubmit->profile.accountScope,
            MailCredentialOperation::Submit})))
    throw std::runtime_error(
        "bind a live submission-scoped mail credential before authorizing");
  const auto record    = impl_->active.inspect(intent);
  auto authority       = std::make_shared<EffectAuthority>();
  authority->operation = r.kind;
  authority->expiresAt = std::chrono::steady_clock::now() + policy.lifetime;
  authority->canonicalTarget   = record.attributes.at("canonical-target");
  authority->targetFingerprint = record.attributes.at("target-fingerprint");
  if (const auto problem =
          effectAuthorityProblem(r, *authority, impl_->cancelled))
    throw std::runtime_error(*problem);
  const auto id = impl_->active.appendRecord(
      "effect-authority", "session grant",
      "This record conveys no authority after session teardown", intent,
      {{"executor", impl_->executor},
       {"operation", std::string(effectKindName(r.kind))},
       {"target", authority->canonicalTarget},
       {"valid-for-ms", std::to_string(policy.lifetime.count())}});
  impl_->active.durableSave();
  impl_->grants.emplace(id, Impl::Grant{intent, std::move(authority)});
  return id;
}
void EffectRuntime::revoke(RecordId id) {
  impl_->requireActive();
  const auto found = impl_->grants.find(id);
  if (found == impl_->grants.end())
    throw std::invalid_argument("authority is not held by this session");
  found->second.authority->revoked = true;
  found->second.protocolRevoked->store(true);
  if (const auto credential =
          impl_->fediverseCredentials.find(found->second.intent);
      credential != impl_->fediverseCredentials.end())
    static_cast<void>(credential->second->revoke());
  if (const auto protocol =
          impl_->fediverseAuthorities.find(found->second.intent);
      protocol != impl_->fediverseAuthorities.end())
    if (const auto authority = protocol->second.lock())
      authority->revoked = true;
  std::ignore =
      impl_->active.appendRecord("effect-revocation", "revoked", "", id);
  impl_->active.durableSave();
}
void EffectRuntime::revokeAll() {
  for (auto &[id, grant] : impl_->grants) {
    std::ignore              = id;
    grant.authority->revoked = true;
    grant.protocolRevoked->store(true);
    if (const auto credential = impl_->fediverseCredentials.find(grant.intent);
        credential != impl_->fediverseCredentials.end())
      static_cast<void>(credential->second->revoke());
    if (const auto protocol = impl_->fediverseAuthorities.find(grant.intent);
        protocol != impl_->fediverseAuthorities.end())
      if (const auto authority = protocol->second.lock())
        authority->revoked = true;
  }
}
RecordId EffectRuntime::dispatch(RecordId intent, RecordId grantId, bool retry,
                                 bool acknowledgeDuplication) {
  impl_->requireActive();
  if (busy())
    throw std::runtime_error(
        "an effect result must be retained before another dispatch");
  const auto grant = impl_->grants.find(grantId);
  if (grant == impl_->grants.end() || grant->second.intent != intent)
    throw std::invalid_argument(
        "authorize this exact intent in the current session first");
  const auto r     = request(intent);
  impl_->cancelled = false;
  if (const auto problem =
          effectAuthorityProblem(r, *grant->second.authority, impl_->cancelled))
    throw std::runtime_error(*problem);
  const auto records = impl_->active.records();
  std::vector<RecordId> attempts;
  for (const auto &record : records)
    if (record.kind == "effect-attempt" && record.parent == intent)
      attempts.push_back(record.id);
  if (!attempts.empty()) {
    if (!retry)
      throw std::runtime_error(
          "intent already attempted; recovery never resends it");
    const auto last    = attempts.back();
    const auto receipt = std::ranges::find_if(records, [&](const auto &record) {
      return (record.kind == "effect-receipt" ||
              record.kind == "effect-acceptance") &&
             record.parent == last;
    });
    if ((receipt == records.end() ||
         receipt->attributes.at("may-have-executed") == "yes") &&
        !acknowledgeDuplication)
      throw std::runtime_error(
          "prior outcome may have executed; independent retry requires "
          "explicit duplication acknowledgement");
  } else if (retry)
    throw std::invalid_argument("there is no previous attempt to retry");
  const auto attempt = impl_->active.appendRecord(
      "effect-attempt", "admitted", "Durably admitted before external I/O",
      intent,
      {{"executor", impl_->executor},
       {"generation", std::to_string(attempts.size() + 1)},
       {"admitted-unix-ms", now()},
       {"authority-revision", std::to_string(grantId)},
       {"duplicate-risk-acknowledged", acknowledgeDuplication ? "yes" : "no"}});
  impl_->active.durableSave();
  impl_->attempt             = attempt;
  impl_->running             = true;
  const auto authority       = grant->second.authority;
  const auto protocolRevoked = grant->second.protocolRevoked;
  const auto fediverseCredential =
      impl_->fediverseCredentials.contains(intent)
          ? impl_->fediverseCredentials.at(intent)
          : std::shared_ptr<FediverseCredentialLease>{};
  const auto ircSession     = impl_->ircSessions.contains(intent)
                                  ? impl_->ircSessions.at(intent)
                                  : std::shared_ptr<IrcSession>{};
  const auto mailCredential = impl_->mailCredentialsByIntent.contains(intent)
                                  ? impl_->mailCredentialsByIntent.at(intent)
                                  : std::shared_ptr<MailCredentialLease>{};
  std::shared_ptr<FediverseAuthority> fediverseAuthority;
  if (r.kind == EffectKind::FediversePost) {
    const auto &post   = *r.fediversePost;
    fediverseAuthority = std::make_shared<FediverseAuthority>(
        post.instanceOrigin, post.accountId, FediverseAction::PublishStatus,
        fediverseRequestFingerprint(post), post.audience, authority->expiresAt);
    impl_->fediverseAuthorities[intent] = fediverseAuthority;
  }
  const auto fediverse = &impl_->fediverse;
  try {
    impl_->worker = std::thread([state = impl_.get(), r, authority,
                                 protocolRevoked, fediverseCredential,
                                 fediverseAuthority, ircSession, mailCredential,
                                 fediverse] {
      EffectResult result;
      try {
        if (r.kind == EffectKind::SqlPatch)
          result = executeSqlPatch(r, *authority, state->cancelled);
        else if (r.kind == EffectKind::Process)
          result = executeProcess(r, *authority, state->cancelled);
        else if (r.kind == EffectKind::FediversePost) {
          result.resolvedTarget = authority->canonicalTarget;
          if (const auto problem =
                  effectAuthorityProblem(r, *authority, state->cancelled)) {
            result.outcome = EffectOutcome::Rejected;
            result.detail  = *problem;
          } else if (protocolRevoked->load() || !fediverseCredential ||
                     !fediverseCredential->usable()) {
            result.outcome = EffectOutcome::Rejected;
            result.detail =
                "Fediverse credential or authority is no longer active";
          } else {
            auto postResult =
                fediverse->post(*r.fediversePost, fediverseCredential,
                                fediverseAuthority, state->cancelled);
            result.mayHaveExecuted = postResult.mayHaveExecuted;
            result.fediversePost   = std::move(postResult);
            if (result.fediversePost->accepted)
              result.outcome = EffectOutcome::Accepted;
            else if (result.fediversePost->mayHaveExecuted)
              result.outcome = EffectOutcome::Unknown;
            else if (result.fediversePost->state == CaptureState::Cancelled)
              result.outcome = EffectOutcome::Cancelled;
            else
              result.outcome = EffectOutcome::Rejected;
            result.detail =
                result.fediversePost->error.empty()
                    ? (result.fediversePost->accepted
                           ? "REST server accepted the status; federation "
                             "delivery is unconfirmed"
                           : "Fediverse REST post did not receive acceptance")
                    : result.fediversePost->error;
          }
        } else if (r.kind == EffectKind::IrcSend) {
          result.resolvedTarget = authority->canonicalTarget;
          if (const auto problem =
                  effectAuthorityProblem(r, *authority, state->cancelled)) {
            result.outcome = EffectOutcome::Rejected;
            result.detail  = *problem;
          } else if (protocolRevoked->load() || !ircSession) {
            result.outcome = EffectOutcome::Rejected;
            result.detail  = "IRC session or authority is no longer active";
          } else {
            const auto &send = *r.ircSend;
            IrcSendAuthority ircAuthority{
                .serverAuthority       = send.serverAuthority,
                .networkIncarnation    = send.networkIncarnation,
                .connectionIncarnation = send.connectionIncarnation,
                .target                = send.target,
                .payload               = send.payload,
                .audience              = send.audience,
                .expiresAt             = authority->expiresAt,
                .revoked               = protocolRevoked};
            auto sendResult = ircSession->send(ircAuthority, state->cancelled);
            result.mayHaveExecuted = sendResult.state != IrcSendState::Rejected;
            result.ircSend         = std::move(sendResult);
            result.outcome =
                result.ircSend->state == IrcSendState::EchoAcknowledged
                    ? EffectOutcome::Accepted
                : result.ircSend->state == IrcSendState::WrittenUnknown
                    ? EffectOutcome::Unknown
                    : EffectOutcome::Rejected;
            result.detail = result.ircSend->detail;
          }
        } else if (r.kind == EffectKind::MailSubmit) {
          result.resolvedTarget = authority->canonicalTarget;
          if (const auto problem =
                  effectAuthorityProblem(r, *authority, state->cancelled)) {
            result.outcome = EffectOutcome::Rejected;
            result.detail  = *problem;
          } else if (protocolRevoked->load() || !mailCredential ||
                     !mailCredential->matches(
                         {r.mailSubmit->profile.serverAuthority,
                          r.mailSubmit->profile.accountScope,
                          MailCredentialOperation::Submit})) {
            result.outcome = EffectOutcome::Rejected;
            result.detail =
                "mail credential or submission authority is no longer active";
          } else {
            const auto &mail = *r.mailSubmit;
            MailSubmissionAuthority mailAuthority{
                .serverAuthority    = mail.profile.serverAuthority,
                .accountScope       = mail.profile.accountScope,
                .draftFingerprint   = mailDraftFingerprint(mail.draft),
                .envelopeFrom       = mail.draft.envelopeFrom,
                .envelopeRecipients = mail.draft.envelopeRecipients,
                .expiresAt          = authority->expiresAt,
                .revoked            = protocolRevoked};
            auto smtpResult =
                submitSmtp(mail.profile, mail.draft, *mailCredential,
                           mailAuthority, state->cancelled);
            result.mayHaveExecuted =
                smtpResult.state == SmtpTransactionState::Accepted ||
                smtpResult.state == SmtpTransactionState::OutcomeUnknown;
            result.smtpSubmission = std::move(smtpResult);
            switch (result.smtpSubmission->state) {
            case SmtpTransactionState::Accepted:
              result.outcome = EffectOutcome::Accepted;
              result.detail =
                  "SMTP server accepted local submission; inbox delivery "
                  "is unconfirmed";
              break;
            case SmtpTransactionState::OutcomeUnknown:
              result.outcome = EffectOutcome::Unknown;
              result.detail =
                  "SMTP result is unknown after possible message transfer";
              break;
            case SmtpTransactionState::Rejected:
              result.outcome = EffectOutcome::Rejected;
              result.detail  = "SMTP server rejected the submission";
              break;
            case SmtpTransactionState::Cancelled:
              result.outcome = EffectOutcome::Cancelled;
              result.detail  = "SMTP submission cancelled before message data";
              break;
            case SmtpTransactionState::Failed:
              result.outcome = EffectOutcome::Failed;
              result.detail  = "SMTP submission failed before acceptance";
              break;
            }
          }
        } else {
          result.resolvedTarget = r.target;
          if (const auto problem =
                  effectAuthorityProblem(r, *authority, state->cancelled)) {
            result.outcome = EffectOutcome::Rejected;
            result.detail  = *problem;
          } else {
            // The delayed fake accepts once before completion; cancellation
            // cannot turn that accepted occurrence back into an unsent draft.
            result.mayHaveExecuted = true;
            {
              const std::scoped_lock lock(state->guard);
              state->acceptance = EffectResult{
                  .resolvedTarget = r.target,
                  .detail = "Controlled fake accepted the action; completion "
                            "and delivery remain unconfirmed",
                  .mayHaveExecuted = true};
            }
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start <
                       std::chrono::milliseconds(
                           std::min(r.fakeDelayMs, r.limits.timeoutMs)) &&
                   !state->cancelled.load())
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            result.outcome = r.fakeUnknown || r.fakeDelayMs > r.limits.timeoutMs
                                 ? EffectOutcome::Unknown
                                 : EffectOutcome::Completed;
            result.detail  = state->cancelled.load()
                                 ? "Late acceptance retained after "
                                   "cancellation; no continuation resumed"
                                 : "Controlled fake completion";
          }
        }
      } catch (const std::exception &) {
        result.outcome         = EffectOutcome::Unknown;
        result.mayHaveExecuted = true;
        result.detail = "Adapter terminated without a conclusive outcome";
      }
      {
        const std::scoped_lock lock(state->guard);
        state->completion = std::move(result);
      }
      state->running = false;
    });
  } catch (...) {
    impl_->running = false;
    {
      const std::scoped_lock lock(impl_->guard);
      impl_->completion =
          EffectResult{.outcome = EffectOutcome::Failed,
                       .detail  = "Worker could not start; no external I/O"};
    }
    std::ignore = poll();
    throw;
  }
  return attempt;
}
void EffectRuntime::cancel() {
  impl_->cancelled = true;
  if (impl_->attempt && impl_->activeMatches()) {
    std::ignore = impl_->active.appendRecord(
        "effect-cancellation", "requested",
        "Cancellation does not prove the external effect was avoided",
        *impl_->attempt, {{"requested-unix-ms", now()}});
    impl_->active.durableSave();
  }
}
bool EffectRuntime::busy() const { return impl_->attempt.has_value(); }
bool EffectRuntime::poll() {
  bool terminalRetained = false;
  for (;;) {
    std::optional<EffectResult> result;
    bool accepted = false;
    {
      const std::scoped_lock lock(impl_->guard);
      if (impl_->acceptance) {
        result   = impl_->acceptance;
        accepted = true;
      } else if (impl_->completion) {
        result = impl_->completion;
      } else
        return terminalRetained;
    }
    if (!accepted && impl_->worker.joinable()) impl_->worker.join();
    const auto completionId = impl_->executor + "-" +
                              std::to_string(*impl_->attempt) +
                              (accepted ? "-accepted" : "-terminal");
    try {
      if (impl_->activeMatches())
        impl_->receipt(impl_->active, *impl_->attempt, *result, completionId,
                       accepted);
      else {
        EvidenceCase journal(impl_->scroll);
        journal.open(impl_->path);
        if (journal.store().documentId().str() != impl_->document)
          throw std::runtime_error("original effect case was replaced");
        impl_->receipt(journal, *impl_->attempt, *result, completionId,
                       accepted);
      }
    } catch (const std::exception &) {
      const auto root = impl_->receiptRoot();
      std::filesystem::create_directories(root);
      if (!impl_->scroll->config().storageDir.empty() &&
          ::chmod(root.c_str(), S_IRWXU) != 0)
        throw std::runtime_error("cannot make receipt inbox private");
      const auto inboxPath =
          root / (impl_->receiptPrefix() + std::to_string(*impl_->attempt) +
                  "-" + impl_->executor);
      EvidenceCase inbox(impl_->scroll);
      if (std::filesystem::exists(inboxPath))
        inbox.open(inboxPath);
      else
        inbox.create(inboxPath, "Late effect receipt inbox");
      const auto envelopes = inbox.records();
      const auto previous =
          std::ranges::find_if(envelopes, [&](const auto &record) {
            return record.kind == "effect-inbox" &&
                   record.attributes.at("original-case") == impl_->document &&
                   record.attributes.at("original-attempt") ==
                       std::to_string(*impl_->attempt) &&
                   record.attributes.at("executor") == impl_->executor;
          });
      const auto entry =
          previous != envelopes.end()
              ? previous->id
              : inbox.appendRecord(
                    "effect-inbox", "late receipt",
                    "Receipt retained independently of an unavailable or "
                    "replaced case",
                    {},
                    {{"original-case", impl_->document},
                     {"original-attempt", std::to_string(*impl_->attempt)},
                     {"executor", impl_->executor}});
      impl_->receipt(inbox, entry, *result, completionId, accepted);
    }
    {
      const std::scoped_lock lock(impl_->guard);
      if (accepted)
        impl_->acceptance.reset();
      else
        impl_->completion.reset();
    }
    if (!accepted) {
      impl_->attempt.reset();
      terminalRetained = true;
    }
  }
}
void EffectRuntime::recover() {
  impl_->requireActive();
  const auto prefix = impl_->receiptPrefix();
  const auto parent = impl_->receiptRoot();
  std::filesystem::create_directories(parent);
  if (!impl_->scroll->config().storageDir.empty() &&
      ::chmod(parent.c_str(), S_IRWXU) != 0)
    throw std::runtime_error("cannot make receipt inbox private");
  auto known = impl_->active.records();
  std::set<std::string> completions;
  for (const auto &record : known)
    if ((record.kind == "effect-receipt" ||
         record.kind == "effect-acceptance") &&
        record.attributes.contains("completion-id"))
      completions.insert(record.attributes.at("completion-id"));
  for (const auto &entry : std::filesystem::directory_iterator(parent)) {
    if (!entry.is_directory() ||
        !entry.path().filename().string().starts_with(prefix))
      continue;
    EvidenceCase inbox(impl_->scroll);
    inbox.open(entry.path());
    const auto stored = inbox.records();
    for (const auto &envelope : stored) {
      if (envelope.kind != "effect-inbox" ||
          envelope.attributes.at("original-case") != impl_->document)
        continue;
      const auto attempt =
          integer<RecordId>(envelope.attributes.at("original-attempt"));
      if (impl_->active.inspect(attempt).kind != "effect-attempt")
        throw std::runtime_error(
            "late receipt references an invalid original attempt");
      for (const auto &receipt : stored) {
        if ((receipt.kind != "effect-receipt" &&
             receipt.kind != "effect-acceptance") ||
            receipt.parent != envelope.id)
          continue;
        const auto &a           = receipt.attributes;
        const auto completionId = a.at("completion-id");
        if (completions.contains(completionId)) continue;
        EffectResult result;
        const bool accepted = receipt.kind == "effect-acceptance";
        bool recognized     = accepted;
        for (const auto outcome :
             {EffectOutcome::Completed, EffectOutcome::Rejected,
              EffectOutcome::Failed, EffectOutcome::Cancelled,
              EffectOutcome::Unknown, EffectOutcome::Partial,
              EffectOutcome::Accepted})
          if (outcomeName(outcome) == a.at("outcome")) {
            result.outcome = outcome;
            recognized     = true;
          }
        if (!recognized)
          throw std::runtime_error("unknown late receipt outcome");
        result.detail          = receipt.text;
        result.resolvedTarget  = a.at("resolved-target");
        result.mayHaveExecuted = a.at("may-have-executed") == "yes";
        if (a.contains("exit-code"))
          result.exitCode = integer<int>(a.at("exit-code"));
        if (a.contains("signal")) result.signal = integer<int>(a.at("signal"));
        if (a.contains("affected-rows"))
          result.affectedRows = integer<std::uint64_t>(a.at("affected-rows"));
        if (a.contains("protocol") &&
            a.at("protocol") == "fediverse-rest-post") {
          FediversePostResult post;
          post.state =
              static_cast<CaptureState>(integer<int>(a.at("post-state")));
          post.httpStatus = integer<long>(a.at("http-status"));
          post.accepted   = a.at("post-accepted") == "yes";
          post.restId     = a.at("post-rest-id");
          if (!a.at("post-object-uri").empty())
            post.objectUri = a.at("post-object-uri");
          if (!a.at("post-actor-uri").empty())
            post.actorUri = a.at("post-actor-uri");
          if (a.contains("post-actor-identity-basis"))
            post.actorIdentityBasis = a.at("post-actor-identity-basis");
          if (!a.at("post-activity-uri").empty())
            post.activityUri = a.at("post-activity-uri");
          post.idempotencyKey = a.at("post-idempotency-key");
          post.visibility     = a.at("post-visibility");
          post.error          = a.at("post-error");
          post.federationDeliveryConfirmed =
              a.at("post-federation-delivery") == "yes";
          post.responseBody     = a.at("post-response-body");
          post.responseRedacted = a.at("post-response-redacted") == "yes";
          post.mayHaveExecuted  = result.mayHaveExecuted;
          for (std::size_t i = 0;
               i < integer<std::size_t>(a.at("post-audience-count")); ++i)
            post.audience.push_back(a.at("post-audience-" + std::to_string(i)));
          result.fediversePost = std::move(post);
        } else if (a.contains("protocol") && a.at("protocol") == "irc-send") {
          IrcSendReceipt send;
          send.state =
              static_cast<IrcSendState>(integer<int>(a.at("irc-send-state")));
          send.serverReply = a.at("irc-server-reply");
          if (!a.at("irc-echo-id").empty())
            send.echoMessageId = a.at("irc-echo-id");
          send.detail    = a.at("irc-detail");
          result.ircSend = std::move(send);
        } else if (a.contains("protocol") &&
                   a.at("protocol") == "smtp-submission") {
          SmtpSubmissionResult smtp;
          const auto state = integer<int>(a.at("smtp-state"));
          if (state < static_cast<int>(SmtpTransactionState::Accepted) ||
              state > static_cast<int>(SmtpTransactionState::Cancelled))
            throw std::runtime_error("invalid retained SMTP state");
          smtp.state       = static_cast<SmtpTransactionState>(state);
          smtp.dataStarted = a.at("smtp-data-started") == "yes";
          smtp.finalResponseCode =
              integer<int>(a.at("smtp-final-response-code"));
          smtp.correlationId  = a.at("smtp-correlation-id");
          smtp.safeDiagnostic = a.at("smtp-diagnostic");
          const auto count = integer<std::size_t>(a.at("smtp-recipient-count"));
          if (count > a.size())
            throw std::runtime_error(
                "retained SMTP recipient count is invalid");
          for (std::size_t i = 0; i < count; ++i) {
            const auto prefix         = "smtp-recipient-" + std::to_string(i);
            const auto recipientState = integer<int>(a.at(prefix + "-state"));
            if (recipientState <
                    static_cast<int>(SmtpRecipientState::Accepted) ||
                recipientState > static_cast<int>(SmtpRecipientState::Unknown))
              throw std::runtime_error("invalid retained SMTP recipient state");
            smtp.recipients.push_back(
                {a.at(prefix + "-address"),
                 static_cast<SmtpRecipientState>(recipientState),
                 integer<int>(a.at(prefix + "-response-code"))});
          }
          result.smtpSubmission = std::move(smtp);
        }
        for (const auto &output : stored) {
          if (output.kind != "effect-output" || output.parent != receipt.id)
            continue;
          if (output.attributes.at("stream") == "stdout")
            result.stdoutBytes += output.text;
          else if (output.attributes.at("stream") == "stderr")
            result.stderrBytes += output.text;
        }
        impl_->receipt(impl_->active, attempt, result, completionId, accepted);
        completions.insert(completionId);
      }
    }
  }
  const auto records = impl_->active.records();
  std::set<RecordId> receipted;
  for (const auto &record : records)
    if (record.kind == "effect-receipt" && record.parent)
      receipted.insert(*record.parent);
  for (const auto &record : records)
    if (record.kind == "effect-attempt" && !receipted.contains(record.id) &&
        impl_->attempt != record.id)
      impl_->receipt(
          impl_->active, record.id,
          EffectResult{
              .outcome = EffectOutcome::Unknown,
              .detail =
                  "Recovered durable admission without a conclusive receipt; "
                  "reconcile explicitly, never automatically resend",
              .mayHaveExecuted = true});
}
std::vector<RecordId> EffectRuntime::importHistory(std::string_view history,
                                                   std::string_view source) {
  impl_->requireActive();
  const CaptureLimits limits;
  if (history.size() > limits.maxBytes || source.size() > limits.maxBytes)
    throw std::invalid_argument("history exceeds import byte budget");
  const auto occurrences = std::count(history.begin(), history.end(), '\n') +
                           (!history.empty() && history.back() != '\n');
  if (static_cast<std::size_t>(occurrences) > limits.maxRows)
    throw std::invalid_argument("history exceeds occurrence budget");
  std::vector<RecordId> result;
  const auto batch = impl_->active.appendRecord(
      "history-import", std::string(source),
      "Import is evidence, never dispatch", {}, {{"captured-unix-ms", now()}});
  std::size_t offset{}, ordinal{};
  while (offset < history.size()) {
    const auto end = history.find('\n', offset);
    const auto length =
        end == history.npos ? history.size() - offset : end - offset;
    result.push_back(impl_->active.appendRecord(
        "history-occurrence", std::to_string(ordinal++),
        history.substr(offset, length), batch,
        {{"timestamp", "unknown"},
         {"cwd", "unknown"},
         {"exit-status", "unknown"},
         {"session", "unknown"},
         {"source-byte-offset", std::to_string(offset)}}));
    if (end == history.npos) break;
    offset = end + 1;
  }
  impl_->active.durableSave();
  return result;
}
} // namespace xanadu::evidence
