#include "common/xanadu/evidence/mail_transport.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

#include "common/xanadu/evidence/mail_transport_internal.hpp"

namespace xanadu::evidence {
namespace {

constexpr std::size_t maxSerializedBytes      = 32 * 1024 * 1024;
constexpr std::size_t maxEnvelopeItems        = 1024;
constexpr std::size_t maxEnvelopeAddressBytes = 4096;
constexpr auto maxTimeout                     = std::chrono::minutes(5);

struct CurlDeleter {
  void operator()(CURL *handle) const noexcept {
    if (handle) curl_easy_cleanup(handle);
  }
};
using CurlHandle = std::unique_ptr<CURL, CurlDeleter>;

struct SlistDeleter {
  void operator()(curl_slist *list) const noexcept {
    if (list) curl_slist_free_all(list);
  }
};
using CurlList = std::unique_ptr<curl_slist, SlistDeleter>;

struct UploadContext {
  const std::string &bytes;
  std::size_t offset{};
  const std::atomic_bool &cancel;
  std::function<bool()> authorized;
};

struct ProtocolTrace {
  enum class CommandKind { MailFrom, Recipient, Data };
  struct PendingCommand {
    CommandKind kind;
    std::optional<std::size_t> recipient;
  };
  std::vector<SmtpRecipientOutcome> &recipients;
  std::size_t nextRecipient{};
  std::deque<PendingCommand> pendingCommands;
  bool dataCommand{};
  bool dataReady{};
  bool dataStarted{};
  int finalResponseCode{};
  int dataCommandResponseCode{};
};

bool safeEnvelopeAddress(std::string_view address) noexcept {
  if (address.empty() || address.size() > maxEnvelopeAddressBytes) return false;
  return std::ranges::none_of(address, [](const unsigned char ch) {
    return ch <= 0x20 || ch >= 0x7f || ch == '<' || ch == '>' || ch == '\0';
  });
}

bool parseResponseCode(std::string_view line, int &code) noexcept {
  if (line.size() < 4 || line[3] != ' ') return false;
  const auto [end, error] = std::from_chars(line.data(), line.data() + 3, code);
  return error == std::errc{} && end == line.data() + 3 && code >= 100 &&
         code <= 599;
}

int traceProtocol(CURL *, curl_infotype type, char *data, std::size_t size,
                  void *context) noexcept {
  if (!data || !context) return 0;
  auto &trace = *static_cast<ProtocolTrace *>(context);
  const std::string_view bytes(data, size);
  if (type == CURLINFO_HEADER_OUT) {
    std::size_t start{};
    while (start < bytes.size()) {
      const auto end  = bytes.find('\n', start);
      const auto stop = end == std::string_view::npos ? bytes.size() : end;
      auto line       = bytes.substr(start, stop - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (line.starts_with("MAIL FROM:")) {
        trace.pendingCommands.push_back(
            {ProtocolTrace::CommandKind::MailFrom, std::nullopt});
      } else if (line.starts_with("RCPT TO:")) {
        if (trace.nextRecipient < trace.recipients.size()) {
          trace.pendingCommands.push_back(
              {ProtocolTrace::CommandKind::Recipient, trace.nextRecipient++});
        }
      } else if (line == "DATA") {
        trace.dataCommand = true;
        trace.pendingCommands.push_back(
            {ProtocolTrace::CommandKind::Data, std::nullopt});
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  } else if (type == CURLINFO_DATA_OUT && trace.dataReady && !bytes.empty()) {
    // The outgoing content may contain credentials copied into message bodies.
    // Only this fact is retained; the callback never stores the byte sequence.
    trace.dataStarted = true;
  } else if (type == CURLINFO_HEADER_IN) {
    std::size_t start{};
    while (start < bytes.size()) {
      const auto end  = bytes.find('\n', start);
      const auto stop = end == std::string_view::npos ? bytes.size() : end;
      auto line       = bytes.substr(start, stop - start);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      int code{};
      if (parseResponseCode(line, code)) {
        if (!trace.pendingCommands.empty()) {
          const auto command = trace.pendingCommands.front();
          trace.pendingCommands.pop_front();
          if (command.kind == ProtocolTrace::CommandKind::Recipient &&
              command.recipient) {
            auto &recipient        = trace.recipients[*command.recipient];
            recipient.responseCode = code;
            recipient.state        = code >= 200 && code < 300
                                         ? SmtpRecipientState::Accepted
                                         : SmtpRecipientState::Rejected;
          } else if (command.kind == ProtocolTrace::CommandKind::Data) {
            if (code == 354)
              trace.dataReady = true;
            else
              trace.dataCommandResponseCode = code;
            trace.dataCommand = false;
          }
        } else if (trace.dataStarted && trace.finalResponseCode == 0) {
          trace.finalResponseCode = code;
          trace.dataReady         = false;
        }
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return 0;
}

std::size_t readMessage(char *destination, std::size_t size, std::size_t count,
                        void *context) noexcept {
  auto &upload = *static_cast<UploadContext *>(context);
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size)
    return CURL_READFUNC_ABORT;
  const auto capacity = size * count;
  bool authorized{};
  try {
    authorized = upload.authorized();
  } catch (...) {
    return CURL_READFUNC_ABORT;
  }
  if (upload.cancel.load(std::memory_order_relaxed) || !authorized)
    return CURL_READFUNC_ABORT;
  const auto remaining = upload.bytes.size() - upload.offset;
  const auto amount    = std::min(capacity, remaining);
  if (amount == 0) return 0;
  std::copy_n(upload.bytes.data() + upload.offset, amount, destination);
  upload.offset += amount;
  return amount;
}

bool authorityCurrent(const MailSubmissionAuthority &authority) noexcept {
  return authority.revoked &&
         !authority.revoked->load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < authority.expiresAt;
}

} // namespace

std::optional<std::string>
smtpSubmissionProblem(const SmtpSubmissionProfile &profile,
                      const SerializedMailDraft &draft) {
  const auto endpoint =
      canonicalMailEndpoint(profile.serverUrl, "smtps", "smtp");
  if (!endpoint || endpoint->authority != profile.serverAuthority)
    return "SMTP endpoint authority is not canonical";
  if (!validMailToken(profile.accountScope))
    return "SMTP account scope is invalid";
  if (profile.timeoutMilliseconds <= 0 ||
      profile.timeoutMilliseconds > maxTimeout.count() * 60'000)
    return "SMTP timeout is outside its supported range";
  if (profile.caBundlePath.find('\0') != std::string::npos)
    return "SMTP CA bundle path contains NUL";
  if (draft.bytes.empty() || draft.bytes.size() > maxSerializedBytes)
    return "serialized mail draft exceeds its byte budget";
  if (draft.envelopeRecipients.empty() ||
      draft.envelopeRecipients.size() > maxEnvelopeItems ||
      !safeEnvelopeAddress(draft.envelopeFrom) ||
      std::ranges::any_of(draft.envelopeRecipients, [](const auto &address) {
        return !safeEnvelopeAddress(address);
      }))
    return "SMTP envelope is invalid or exceeds its recipient budget";
  if (draft.messageId.empty() || draft.correlationId.empty() ||
      draft.messageId.find_first_of("\r\n") != std::string::npos ||
      draft.correlationId.find_first_of("\r\n") != std::string::npos ||
      draft.messageId.find('\0') != std::string::npos ||
      draft.correlationId.find('\0') != std::string::npos)
    return "SMTP draft identity is missing or invalid";
  return {};
}

std::optional<std::string>
smtpServerAuthority(const std::string_view serverUrl) {
  const auto endpoint = canonicalMailEndpoint(serverUrl, "smtps", "smtp");
  if (!endpoint) return std::nullopt;
  return endpoint->authority;
}

SmtpSubmissionResult submitSmtp(const SmtpSubmissionProfile &profile,
                                const SerializedMailDraft &draft,
                                const MailCredentialLease &credential,
                                const MailSubmissionAuthority &authority,
                                const std::atomic_bool &cancellation) {
  SmtpSubmissionResult result{.correlationId = draft.correlationId};
  const auto endpoint =
      canonicalMailEndpoint(profile.serverUrl, "smtps", "smtp");
  if (!endpoint || endpoint->authority != profile.serverAuthority ||
      endpoint->authority != authority.serverAuthority ||
      !validMailToken(profile.accountScope) ||
      authority.accountScope != profile.accountScope || draft.bytes.empty() ||
      draft.bytes.size() > maxSerializedBytes ||
      draft.envelopeRecipients.empty() ||
      draft.envelopeRecipients.size() > maxEnvelopeItems ||
      !safeEnvelopeAddress(draft.envelopeFrom) ||
      std::ranges::any_of(
          draft.envelopeRecipients,
          [](const auto &address) { return !safeEnvelopeAddress(address); }) ||
      !authority.revoked ||
      authority.revoked->load(std::memory_order_relaxed) ||
      std::chrono::steady_clock::now() >= authority.expiresAt ||
      authority.envelopeFrom != draft.envelopeFrom ||
      authority.envelopeRecipients != draft.envelopeRecipients) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP submission authority or request is invalid";
    return result;
  }
  if (authority.draftFingerprint != mailDraftFingerprint(draft)) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP draft does not match its dispatch authority";
    return result;
  }

  const auto leaseState    = MailCredentialInternalAccess::state(credential);
  const bool noCredentials = !leaseState;
  if (noCredentials) {
    if (endpoint->secure || !profile.allowCleartextLoopbackWithoutCredentials ||
        !isLiteralLoopback(endpoint->host)) {
      result.state          = SmtpTransactionState::Failed;
      result.safeDiagnostic = "SMTP credential lease is required";
      return result;
    }
  } else if (!leaseState->usable() ||
             leaseState->scope.operation != MailCredentialOperation::Submit ||
             leaseState->scope.serverAuthority != endpoint->authority ||
             leaseState->scope.accountScope != profile.accountScope) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP credential lease scope is invalid";
    return result;
  }
  if (!endpoint->secure && noCredentials &&
      (!profile.allowCleartextLoopbackWithoutCredentials ||
       !isLiteralLoopback(endpoint->host))) {
    result.state = SmtpTransactionState::Failed;
    result.safeDiagnostic =
        "cleartext SMTP is limited to unauthenticated loopback";
    return result;
  }
  if (profile.timeoutMilliseconds <= 0 ||
      profile.timeoutMilliseconds >
          std::chrono::duration_cast<std::chrono::milliseconds>(maxTimeout)
              .count()) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP timeout is outside its permitted bound";
    return result;
  }
#if LIBCURL_VERSION_NUM < 0x080200
  if (draft.envelopeRecipients.size() > 1) {
    result.state = SmtpTransactionState::Failed;
    result.safeDiagnostic =
        "this libcurl cannot continue after an individual RCPT rejection";
    return result;
  }
#endif

  result.recipients.reserve(draft.envelopeRecipients.size());
  for (const auto &address : draft.envelopeRecipients)
    result.recipients.push_back({address, SmtpRecipientState::Unknown, 0});

  const auto authorized = [&]() {
    if (cancellation.load(std::memory_order_relaxed) ||
        !authorityCurrent(authority))
      return false;
    return !leaseState || leaseState->usable();
  };
  if (!authorized()) {
    result.state          = cancellation.load(std::memory_order_relaxed)
                                ? SmtpTransactionState::Cancelled
                                : SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP authority expired or was revoked before I/O";
    return result;
  }

  try {
    initializeMailCurl();
  } catch (const std::exception &) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "libcurl initialization failed";
    return result;
  }
  MailSigpipeBlock sigpipe;
  CurlHandle handle(curl_easy_init());
  if (!handle) {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "libcurl could not allocate an SMTP request";
    return result;
  }

  curl_slist *rawRecipients{};
  for (const auto &recipient : draft.envelopeRecipients) {
    const auto address = "<" + recipient + ">";
    auto *next         = curl_slist_append(rawRecipients, address.c_str());
    if (!next) {
      curl_slist_free_all(rawRecipients);
      result.state          = SmtpTransactionState::Failed;
      result.safeDiagnostic = "libcurl could not allocate recipients";
      return result;
    }
    rawRecipients = next;
  }
  CurlList recipients(rawRecipients);
  ProtocolTrace trace{result.recipients};
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(profile.timeoutMilliseconds);
  CurlProgressContext progress(cancellation, deadline, authorized);
  UploadContext upload{draft.bytes, 0, cancellation, authorized};
  const auto timeout = profile.timeoutMilliseconds;
  curl_easy_setopt(handle.get(), CURLOPT_URL, endpoint->url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "smtp,smtps");
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   std::min<long>(timeout, 15000));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, timeout);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  if (!profile.caBundlePath.empty())
    curl_easy_setopt(handle.get(), CURLOPT_CAINFO,
                     profile.caBundlePath.c_str());
  if (!endpoint->secure && leaseState)
    curl_easy_setopt(handle.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
  if (leaseState) {
    auto secret = MailCredentialInternalAccess::secret(credential);
    if (!secret) {
      result.state          = SmtpTransactionState::Failed;
      result.safeDiagnostic = "SMTP credential lease expired before I/O";
      return result;
    }
    curl_easy_setopt(handle.get(), CURLOPT_USERNAME, secret->username.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_PASSWORD, secret->password.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_LOGIN_OPTIONS, "AUTH=*");
  }
  const auto envelopeFrom = "<" + draft.envelopeFrom + ">";
  curl_easy_setopt(handle.get(), CURLOPT_MAIL_FROM, envelopeFrom.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_MAIL_RCPT, recipients.get());
#if LIBCURL_VERSION_NUM >= 0x080200
  curl_easy_setopt(handle.get(), CURLOPT_MAIL_RCPT_ALLOWFAILS, 1L);
#endif
  curl_easy_setopt(handle.get(), CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, readMessage);
  curl_easy_setopt(handle.get(), CURLOPT_READDATA, &upload);
  curl_easy_setopt(handle.get(), CURLOPT_INFILESIZE_LARGE,
                   static_cast<curl_off_t>(draft.bytes.size()));
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION,
                   CurlProgressContext::callback);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_VERBOSE, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_DEBUGFUNCTION, traceProtocol);
  curl_easy_setopt(handle.get(), CURLOPT_DEBUGDATA, &trace);

  if (!authorized()) {
    result.state          = cancellation.load(std::memory_order_relaxed)
                                ? SmtpTransactionState::Cancelled
                                : SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP authority expired or was revoked before I/O";
    return result;
  }
  static_cast<void>(
      performMailCurlBounded(handle.get(), cancellation, deadline, authorized));
  result.dataStarted       = trace.dataStarted;
  result.finalResponseCode = trace.finalResponseCode != 0
                                 ? trace.finalResponseCode
                                 : trace.dataCommandResponseCode;
  if (result.finalResponseCode >= 200 && result.finalResponseCode < 300) {
    result.state          = SmtpTransactionState::Accepted;
    result.safeDiagnostic = "SMTP server accepted the submitted message";
  } else if (result.finalResponseCode >= 400 &&
             result.finalResponseCode < 600) {
    result.state          = SmtpTransactionState::Rejected;
    result.safeDiagnostic = "SMTP server rejected the submitted message";
  } else if (trace.dataStarted) {
    result.state = SmtpTransactionState::OutcomeUnknown;
    result.safeDiagnostic =
        "SMTP data was sent without a definitive final response";
  } else if (cancellation.load(std::memory_order_relaxed)) {
    result.state          = SmtpTransactionState::Cancelled;
    result.safeDiagnostic = "SMTP request was cancelled before message data";
  } else if (std::ranges::all_of(result.recipients, [](const auto &recipient) {
               return recipient.state == SmtpRecipientState::Rejected;
             })) {
    result.state          = SmtpTransactionState::Rejected;
    result.safeDiagnostic = "SMTP server rejected all envelope recipients";
  } else {
    result.state          = SmtpTransactionState::Failed;
    result.safeDiagnostic = "SMTP transfer failed before message data";
  }
  return result;
}

} // namespace xanadu::evidence
