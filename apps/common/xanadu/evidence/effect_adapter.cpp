#include "common/xanadu/evidence/effect_adapter.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>
#include <sys/stat.h>

namespace xanadu::evidence {
namespace {
bool hasNul(std::string_view text) { return text.find('\0') != text.npos; }

bool validValue(const Value &value) {
  if (value.valueless_by_exception()) return false;
  const auto *real = std::get_if<double>(&value);
  return !real || std::isfinite(*real);
}
} // namespace

std::string_view effectKindName(EffectKind kind) {
  switch (kind) {
  case EffectKind::DelayedFake:
    return "fake";
  case EffectKind::SqlPatch:
    return "sql-patch";
  case EffectKind::Process:
    return "process";
  case EffectKind::FediversePost:
    return "fediverse-post";
  case EffectKind::IrcSend:
    return "irc-send";
  case EffectKind::MailSubmit:
    return "mail-submit";
  }
  throw std::invalid_argument("unknown effect kind");
}

std::string_view outcomeName(EffectOutcome outcome) {
  switch (outcome) {
  case EffectOutcome::Completed:
    return "completed";
  case EffectOutcome::Rejected:
    return "rejected";
  case EffectOutcome::Failed:
    return "failed";
  case EffectOutcome::Cancelled:
    return "cancelled";
  case EffectOutcome::Unknown:
    return "outcome-unknown";
  case EffectOutcome::Partial:
    return "partial";
  case EffectOutcome::Accepted:
    return "accepted";
  }
  throw std::invalid_argument("unknown effect outcome");
}

std::string canonicalEffectTarget(const EffectRequest &request) {
  if (request.kind == EffectKind::FediversePost && request.fediversePost)
    return request.fediversePost->instanceOrigin + "/accounts/" +
           request.fediversePost->accountId;
  if (request.kind == EffectKind::IrcSend && request.ircSend)
    return request.ircSend->serverAuthority + "/" +
           request.ircSend->networkIncarnation;
  if (request.kind == EffectKind::MailSubmit && request.mailSubmit)
    return request.mailSubmit->profile.serverAuthority + "/account/" +
           request.mailSubmit->profile.accountScope;
  if (request.target.empty() || hasNul(request.target))
    throw std::invalid_argument("effect target is empty or contains NUL");
  if (request.kind == EffectKind::DelayedFake) return request.target;
  return std::filesystem::canonical(request.target).string();
}

std::string effectTargetFingerprint(const EffectRequest &request) {
  const auto path = canonicalEffectTarget(request);
  if (request.kind == EffectKind::FediversePost ||
      request.kind == EffectKind::IrcSend ||
      request.kind == EffectKind::MailSubmit) {
    std::string material;
    const auto append = [&](std::string_view value) {
      material += std::to_string(value.size());
      material += ':';
      material.append(value);
    };
    append(path);
    if (request.fediversePost) {
      const auto &post = *request.fediversePost;
      append(post.text);
      append(post.contentWarning);
      append(post.visibility);
      append(post.replyToRestId.value_or(""));
      append(post.idempotencyKey);
      append(post.accountActorUri);
      for (const auto &recipient : post.audience) append(recipient);
    }
    if (request.ircSend) {
      const auto &send = *request.ircSend;
      append(send.target);
      append(send.payload);
      append(send.connectionIncarnation);
      for (const auto &recipient : send.audience) append(recipient);
    }
    if (request.mailSubmit) {
      const auto &mail = *request.mailSubmit;
      append(mail.profile.serverAuthority);
      append(mail.profile.accountScope);
      append(mail.profile.serverUrl);
      append(mail.profile.caBundlePath);
      append(mail.profile.allowCleartextLoopbackWithoutCredentials ? "yes"
                                                                   : "no");
      append(std::to_string(mail.profile.timeoutMilliseconds));
      append(mailDraftFingerprint(mail.draft));
      append(mail.draft.envelopeFrom);
      append(mail.draft.messageId);
      append(mail.draft.correlationId);
      for (const auto &recipient : mail.draft.envelopeRecipients)
        append(recipient);
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length{};
    if (EVP_Digest(material.data(), material.size(), digest.data(), &length,
                   EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("cannot digest communication authority");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    for (unsigned int i = 0; i < length; ++i) {
      result += hex[digest[i] >> 4];
      result += hex[digest[i] & 15];
    }
    return result;
  }
  if (request.kind == EffectKind::DelayedFake) return path;
  struct stat status{};
  if (::stat(path.c_str(), &status) != 0)
    throw std::runtime_error("cannot inspect effect target identity");
  if (request.kind == EffectKind::SqlPatch)
    return std::to_string(status.st_dev) + ":" + std::to_string(status.st_ino);
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot read executable identity");
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("cannot initialize executable digest");
  std::array<char, 16384> block{};
  while (file) {
    file.read(block.data(), block.size());
    if (file.gcount() &&
        EVP_DigestUpdate(context.get(), block.data(), file.gcount()) != 1)
      throw std::runtime_error("cannot digest executable");
  }
  if (!file.eof()) throw std::runtime_error("cannot read executable identity");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  if (EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    throw std::runtime_error("cannot finish executable digest");
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  for (unsigned int i = 0; i < length; ++i) {
    result += hex[digest[i] >> 4];
    result += hex[digest[i] & 15];
  }
  return result;
}

std::optional<std::string> effectProblem(const EffectRequest &request) {
  if ((request.fediversePost && request.kind != EffectKind::FediversePost) ||
      (request.ircSend && request.kind != EffectKind::IrcSend) ||
      (request.mailSubmit && request.kind != EffectKind::MailSubmit))
    return "typed protocol request does not match the effect operation";
  const bool communication = request.kind == EffectKind::FediversePost ||
                             request.kind == EffectKind::IrcSend ||
                             request.kind == EffectKind::MailSubmit;
  if (!communication && (request.target.empty() || hasNul(request.target)))
    return "effect target is empty or contains NUL";
  if (request.limits.timeoutMs == 0 ||
      request.limits.timeoutMs >
          static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    return "effect timeout is outside the supported range";
  if (request.limits.terminateGraceMs >
      static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    return "effect termination grace is outside supported range";
  if (request.limits.maxOutputBytes > std::numeric_limits<std::size_t>::max() ||
      request.limits.maxInputBytes > std::numeric_limits<std::size_t>::max())
    return "effect byte budget is outside the supported range";
  const auto fields = request.argv.size() + request.environment.size() +
                      request.parameters.size() +
                      (request.guard ? request.guard->parameters.size() : 0);
  if (fields > request.limits.maxFields)
    return "effect field count exceeds its budget";
  if (communication &&
      (!request.argv.empty() || !request.cwd.empty() ||
       !request.environment.empty() || !request.stdinBytes.empty() ||
       !request.sql.empty() || !request.parameters.empty() || request.guard ||
       request.expectedAffectedRows != 1 || request.fakeDelayMs != 0 ||
       request.fakeUnknown))
    return "communication action contains unrelated generic effect fields";
  if (communication && !request.target.empty() &&
      request.target != canonicalEffectTarget(request))
    return "communication action target must come from its typed request";
  std::uint64_t bytes{};
  const auto admit = [&](std::string_view text) {
    if (text.size() > request.limits.maxInputBytes - bytes) return false;
    bytes += text.size();
    return true;
  };
  if (!admit(request.target) || !admit(request.stdinBytes) ||
      !admit(request.sql) || !admit(request.cwd))
    return "effect input exceeds its byte budget";
  for (const auto &arg : request.argv)
    if (hasNul(arg) || !admit(arg)) return "invalid or excessive argv input";
  for (const auto &[key, value] : request.environment)
    if (key.empty() || key.find('=') != key.npos || hasNul(key) ||
        hasNul(value) || !admit(key) || !admit(value))
      return "invalid or excessive environment input";
  if (hasNul(request.cwd) || hasNul(request.sql))
    return "NUL in effect path or SQL";
  const auto checkValue = [&](const Value &value) {
    if (!validValue(value)) return false;
    if (const auto *text = std::get_if<std::string>(&value))
      return admit(*text);
    if (const auto *blob = std::get_if<Binary>(&value))
      return admit(blob->bytes);
    return true;
  };
  for (const auto &value : request.parameters)
    if (!checkValue(value)) return "invalid or excessive SQL parameter";
  if (request.kind == EffectKind::SqlPatch) {
    if (request.sql.empty() || !request.guard || request.guard->query.empty() ||
        hasNul(request.guard->query) || !admit(request.guard->query))
      return "SQL patch requires an explicit bounded guard query";
    if (!checkValue(request.guard->expected))
      return "invalid SQL expected value";
    for (const auto &value : request.guard->parameters)
      if (!checkValue(value)) return "invalid SQL guard parameter";
    if (request.expectedAffectedRows > static_cast<std::uint64_t>(INT64_MAX))
      return "expected row count exceeds SQLite range";
  } else if (request.kind == EffectKind::FediversePost) {
    if (!request.fediversePost || request.ircSend || request.mailSubmit)
      return "Fediverse post requires its typed request only";
    const auto &post = *request.fediversePost;
    if (post.instanceOrigin.empty() || post.accountId.empty() ||
        post.text.empty() || post.visibility == "direct" ||
        (post.visibility != "public" && post.visibility != "unlisted"))
      return "Fediverse post requires a supported public audience";
    if (post.text.size() > request.limits.maxInputBytes ||
        post.contentWarning.size() > request.limits.maxInputBytes ||
        post.audience.size() > request.limits.maxFields)
      return "Fediverse post exceeds its input budget";
    if (post.audience != std::vector<std::string>{
                             "https://www.w3.org/ns/activitystreams#Public"})
      return "Fediverse post requires the public ActivityStreams audience";
    if (post.instanceOrigin.find('\0') != std::string::npos ||
        post.accountId.find('\0') != std::string::npos ||
        post.text.find('\0') != std::string::npos)
      return "NUL in Fediverse request";
    if (const auto problem = fediversePostProblem(
            post, FediverseTransportPolicy{.allowLoopbackHttpForTests = true}))
      return problem;
  } else if (request.kind == EffectKind::IrcSend) {
    if (!request.ircSend || request.fediversePost || request.mailSubmit)
      return "IRC send requires its typed request only";
    const auto &send = *request.ircSend;
    if (send.serverAuthority.empty() || send.networkIncarnation.empty() ||
        send.connectionIncarnation.empty() || send.target.empty() ||
        send.payload.empty() ||
        send.payload.find_first_of("\r\n") != std::string::npos ||
        hasNul(send.payload) || hasNul(send.target) ||
        hasNul(send.serverAuthority) || hasNul(send.networkIncarnation))
      return "IRC send has an invalid target or payload";
    if (hasNul(send.connectionIncarnation) ||
        send.audience != std::vector<std::string>{send.target})
      return "IRC send audience must be exactly its target";
    if (send.payload.size() > request.limits.maxInputBytes ||
        send.audience.size() > request.limits.maxFields)
      return "IRC send exceeds its input budget";
  } else if (request.kind == EffectKind::MailSubmit) {
    if (!request.mailSubmit || request.fediversePost || request.ircSend)
      return "mail submission requires its typed request only";
    const auto &mail = *request.mailSubmit;
    if (mail.profile.timeoutMilliseconds <= 0 ||
        static_cast<std::uint64_t>(mail.profile.timeoutMilliseconds) >
            request.limits.timeoutMs ||
        mail.draft.bytes.size() > request.limits.maxInputBytes ||
        mail.draft.envelopeRecipients.size() > request.limits.maxFields)
      return "mail submission exceeds its timeout or payload budget";
    if (const auto problem = smtpSubmissionProblem(mail.profile, mail.draft))
      return problem;
  } else if (request.kind == EffectKind::Process && request.cwd.empty()) {
    return "process requires an explicit working directory";
  } else if (request.kind != EffectKind::Process &&
             request.kind != EffectKind::DelayedFake) {
    return "unknown effect operation";
  }
  return {};
}

std::optional<std::string>
effectAuthorityProblem(const EffectRequest &request,
                       const EffectAuthority &authority,
                       const std::atomic_bool &cancel) {
  if (cancel.load()) return "effect cancelled before dispatch";
  if (authority.revoked.load()) return "effect authority was revoked";
  if (std::chrono::steady_clock::now() >= authority.expiresAt)
    return "effect authority expired before dispatch";
  if (authority.operation != request.kind)
    return "effect operation is outside authority";
  if (const auto problem = effectProblem(request)) return problem;
  try {
    if (canonicalEffectTarget(request) != authority.canonicalTarget)
      return "effect target is outside authority";
    if (!authority.targetFingerprint.empty() &&
        effectTargetFingerprint(request) != authority.targetFingerprint)
      return "effect target identity changed";
  } catch (const std::exception &) {
    return "effect target cannot be resolved";
  }
  if (cancel.load() || authority.revoked.load())
    return "effect authority is no longer active";
  return {};
}
} // namespace xanadu::evidence
