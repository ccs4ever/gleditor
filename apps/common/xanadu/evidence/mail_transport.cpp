/**
 * @file mail_transport.cpp
 * @brief IMAP session transport behind the GLEDITOR_MAIL probe
 *        (pg_backend pattern). The credential lease machinery, endpoint
 *        canonicalization, curl helpers, validators and the draft
 *        fingerprint are library-free and compile unconditionally; only the
 *        libetpan session plumbing and readImapMailbox's body are gated, and
 *        without GLEDITOR_HAVE_MAIL the entry point returns a Failed
 *        observation carrying the rebuild instructions.
 */

#include "common/xanadu/evidence/mail_transport.hpp"

#include <curl/curl.h>
#ifdef GLEDITOR_HAVE_MAIL
#include <libetpan/charconv.h>
#include <libetpan/mailimap.h>
#include <libetpan/mailimap_types_helper.h>
#include <libetpan/mailstream.h>
#endif
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <poll.h>

#include "common/xanadu/evidence/mail_transport_internal.hpp"
#include "common/xanadu/evidence/mime.hpp"

namespace xanadu::evidence {
namespace {

void clean(std::string &value) noexcept {
  if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
  value.clear();
}

std::string curlPart(CURLU *url, CURLUPart part) {
  char *value{};
  if (curl_url_get(url, part, &value, 0) != CURLUE_OK) return {};
  std::string result(value);
  curl_free(value);
  return result;
}

bool hasUrlPart(CURLU *url, CURLUPart part) {
  char *value{};
  if (curl_url_get(url, part, &value, 0) != CURLUE_OK) return false;
  curl_free(value);
  return true;
}

#ifdef GLEDITOR_HAVE_MAIL
std::optional<std::string> curlConnectOnlyUrl(const MailEndpoint &endpoint) {
  CURLU *raw = curl_url();
  if (!raw) return {};
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(
      raw, curl_url_cleanup);
  if (curl_url_set(url.get(), CURLUPART_URL, endpoint.url.c_str(), 0) !=
          CURLUE_OK ||
      curl_url_set(url.get(), CURLUPART_SCHEME,
                   endpoint.secure ? "https" : "http", 0) != CURLUE_OK ||
      curl_url_set(url.get(), CURLUPART_PATH, "/", 0) != CURLUE_OK)
    return {};
  char *value{};
  if (curl_url_get(url.get(), CURLUPART_URL, &value, 0) != CURLUE_OK) return {};
  std::string result(value);
  curl_free(value);
  return result;
}
#endif // GLEDITOR_HAVE_MAIL

std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

} // namespace

MailCredentialLease::State::~State() {
  clean(username);
  clean(password);
}

bool MailCredentialLease::State::usable() noexcept {
  std::scoped_lock lock(mutex);
  if (revoked.load(std::memory_order_relaxed) ||
      std::chrono::steady_clock::now() >= expiresAt) {
    clean(username);
    clean(password);
    return false;
  }
  return !username.empty() && !password.empty();
}

void MailCredentialLease::State::revoke() noexcept {
  revoked.store(true, std::memory_order_relaxed);
  std::scoped_lock lock(mutex);
  clean(username);
  clean(password);
}

MailCredentialSecret::MailCredentialSecret(std::string user, std::string secret)
    : username(std::move(user)), password(std::move(secret)) {}

MailCredentialSecret::~MailCredentialSecret() {
  clean(username);
  clean(password);
}

MailCredentialSecret::MailCredentialSecret(
    MailCredentialSecret &&other) noexcept
    : username(std::move(other.username)), password(std::move(other.password)) {
  clean(other.username);
  clean(other.password);
}

MailCredentialSecret &
MailCredentialSecret::operator=(MailCredentialSecret &&other) noexcept {
  if (this == &other) return *this;
  clean(username);
  clean(password);
  username = std::move(other.username);
  password = std::move(other.password);
  clean(other.username);
  clean(other.password);
  return *this;
}

std::shared_ptr<MailCredentialLease::State>
MailCredentialInternalAccess::state(const MailCredentialLease &lease) {
  return lease.state_;
}

std::optional<MailCredentialSecret>
MailCredentialInternalAccess::secret(const MailCredentialLease &lease) {
  if (!lease.state_) return {};
  std::scoped_lock lock(lease.state_->mutex);
  if (lease.state_->revoked.load(std::memory_order_relaxed) ||
      std::chrono::steady_clock::now() >= lease.state_->expiresAt ||
      lease.state_->username.empty() || lease.state_->password.empty()) {
    clean(lease.state_->username);
    clean(lease.state_->password);
    return {};
  }
  return MailCredentialSecret{lease.state_->username, lease.state_->password};
}

MailCredentialLease::MailCredentialLease(std::shared_ptr<State> state)
    : state_(std::move(state)) {}

bool MailCredentialLease::valid() const { return state_ && state_->usable(); }

bool MailCredentialLease::matches(const MailCredentialScope &scope) const {
  if (!state_ || !state_->usable()) return false;
  std::scoped_lock lock(state_->mutex);
  return !state_->revoked.load(std::memory_order_relaxed) &&
         std::chrono::steady_clock::now() < state_->expiresAt &&
         state_->scope.serverAuthority == scope.serverAuthority &&
         state_->scope.accountScope == scope.accountScope &&
         state_->scope.operation == scope.operation;
}

std::chrono::steady_clock::time_point MailCredentialLease::expiresAt() const {
  return state_ ? state_->expiresAt : std::chrono::steady_clock::time_point{};
}

MailCredentialLease MailCredentialBroker::issue(
    MailCredentialScope scope, std::string username, std::string password,
    const std::chrono::steady_clock::duration lifetime) const {
  struct SecretGuard {
    std::string &username;
    std::string &password;
    ~SecretGuard() {
      clean(username);
      clean(password);
    }
  } guard{username, password};
  if (scope.serverAuthority.empty() || scope.accountScope.empty() ||
      !validMailToken(scope.accountScope) || username.empty() ||
      username.size() > 4096 || username.find('\0') != std::string::npos ||
      username.find_first_of("\r\n") != std::string::npos || password.empty() ||
      password.size() > 65536 || password.find('\0') != std::string::npos ||
      password.find_first_of("\r\n") != std::string::npos ||
      lifetime <= std::chrono::steady_clock::duration::zero() ||
      lifetime > std::chrono::hours(24))
    throw std::invalid_argument("invalid mail credential lease scope");
  const auto endpoint = canonicalMailEndpoint(
      scope.serverAuthority,
      scope.operation == MailCredentialOperation::Read ? "imaps" : "smtps",
      scope.operation == MailCredentialOperation::Read ? "imap" : "smtp");
  if (!endpoint || endpoint->authority != scope.serverAuthority)
    throw std::invalid_argument(
        "mail credential scope must use a canonical server authority");
  auto state       = std::make_shared<MailCredentialLease::State>();
  state->scope     = std::move(scope);
  state->username  = username;
  state->password  = password;
  state->expiresAt = std::chrono::steady_clock::now() + lifetime;
  return MailCredentialLease(std::move(state));
}

void MailCredentialBroker::revoke(const MailCredentialLease &lease) const {
  if (lease.state_) lease.state_->revoke();
}

std::optional<MailEndpoint>
canonicalMailEndpoint(const std::string_view input,
                      const std::string_view secureScheme,
                      const std::string_view clearScheme) {
  initializeMailCurl();
  CURLU *raw = curl_url();
  if (!raw) return {};
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
      raw, curl_url_cleanup);
  const std::string url(input);
  if (curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK)
    return {};
  auto scheme       = lower(curlPart(parsed.get(), CURLUPART_SCHEME));
  const bool secure = scheme == secureScheme;
  if ((!secure && scheme != clearScheme) ||
      curlPart(parsed.get(), CURLUPART_HOST).empty() ||
      hasUrlPart(parsed.get(), CURLUPART_USER) ||
      hasUrlPart(parsed.get(), CURLUPART_PASSWORD) ||
      !curlPart(parsed.get(), CURLUPART_QUERY).empty() ||
      !curlPart(parsed.get(), CURLUPART_FRAGMENT).empty())
    return {};
  const auto path = curlPart(parsed.get(), CURLUPART_PATH);
  if (!path.empty() && path != "/") return {};
  auto host = lower(curlPart(parsed.get(), CURLUPART_HOST));
  if (host.size() > 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  auto port = curlPart(parsed.get(), CURLUPART_PORT);
  if (port.empty()) {
    if (secureScheme == "imaps")
      port = secure ? "993" : "143";
    else
      port = secure ? "465" : "25";
  }
  if (curl_url_set(parsed.get(), CURLUPART_PORT, port.c_str(), 0) != CURLUE_OK)
    return {};
  char *canonical{};
  if (curl_url_get(parsed.get(), CURLUPART_URL, &canonical, 0) != CURLUE_OK)
    return {};
  const std::string canonicalUrl(canonical);
  curl_free(canonical);
  const bool ipv6          = host.find(':') != std::string::npos;
  const auto authorityHost = ipv6 ? "[" + host + "]" : host;
  return MailEndpoint{.url       = canonicalUrl,
                      .authority = scheme + "://" + authorityHost + ":" + port,
                      .scheme    = std::move(scheme),
                      .host      = host,
                      .port      = std::move(port),
                      .secure    = secure};
}

void initializeMailCurl() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("mail transport initialization failed");
    const auto *version = curl_version_info(CURLVERSION_NOW);
    if (!version || !(version->features & CURL_VERSION_ASYNCHDNS))
      throw std::runtime_error(
          "mail transport requires asynchronous DNS resolution");
  });
}

CURLcode
performMailCurlBounded(CURL *easy, const std::atomic_bool &cancel,
                       const std::chrono::steady_clock::time_point deadline,
                       const std::function<bool()> &preflight,
                       CURLM *sharedMulti, const bool keepAttached) {
  if (!easy) return CURLE_BAD_FUNCTION_ARGUMENT;
  MailSigpipeBlock sigpipe;
  std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> ownedMulti(
      nullptr, curl_multi_cleanup);
  if (!sharedMulti) {
    ownedMulti.reset(curl_multi_init());
    if (!ownedMulti) return CURLE_OUT_OF_MEMORY;
    sharedMulti = ownedMulti.get();
  }
  const auto check = [&]() -> CURLcode {
    if (cancel.load(std::memory_order_relaxed))
      return CURLE_ABORTED_BY_CALLBACK;
    if (std::chrono::steady_clock::now() >= deadline)
      return CURLE_OPERATION_TIMEDOUT;
    try {
      return preflight && !preflight() ? CURLE_ABORTED_BY_CALLBACK : CURLE_OK;
    } catch (...) {
      return CURLE_ABORTED_BY_CALLBACK;
    }
  };
  if (const auto status = check(); status != CURLE_OK) return status;
  if (curl_multi_add_handle(sharedMulti, easy) != CURLM_OK)
    return CURLE_FAILED_INIT;

  const auto removeHandle = [&] {
    static_cast<void>(curl_multi_remove_handle(sharedMulti, easy));
  };
  if (const auto status = check(); status != CURLE_OK) {
    removeHandle();
    return status;
  }
  int running{};
  auto multiResult = curl_multi_perform(sharedMulti, &running);
  while (multiResult == CURLM_CALL_MULTI_PERFORM) {
    multiResult = curl_multi_perform(sharedMulti, &running);
  }
  while (multiResult == CURLM_OK && running > 0) {
    if (const auto status = check(); status != CURLE_OK) {
      removeHandle();
      return status;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now())
            .count();
    const auto waitMilliseconds =
        static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 50));
    int readyDescriptors{};
    multiResult = curl_multi_poll(sharedMulti, nullptr, 0, waitMilliseconds,
                                  &readyDescriptors);
    if (multiResult == CURLM_OK) {
      if (const auto status = check(); status != CURLE_OK) {
        removeHandle();
        return status;
      }
      multiResult = curl_multi_perform(sharedMulti, &running);
      while (multiResult == CURLM_CALL_MULTI_PERFORM) {
        multiResult = curl_multi_perform(sharedMulti, &running);
      }
    }
  }
  if (multiResult != CURLM_OK) {
    removeHandle();
    return CURLE_RECV_ERROR;
  }
  int messagesLeft{};
  CURLMsg *message{};
  CURLcode result = CURLE_FAILED_INIT;
  while ((message = curl_multi_info_read(sharedMulti, &messagesLeft))) {
    if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
      result = message->data.result;
      break;
    }
  }
  if (!keepAttached || result != CURLE_OK) removeHandle();
  return result;
}

bool isLiteralLoopback(const std::string_view host) {
  in_addr ipv4{};
  if (inet_pton(AF_INET, std::string(host).c_str(), &ipv4) == 1)
    return (ntohl(ipv4.s_addr) >> 24U) == 127U;
  in6_addr ipv6{};
  if (inet_pton(AF_INET6, std::string(host).c_str(), &ipv6) != 1) return false;
  return IN6_IS_ADDR_LOOPBACK(&ipv6);
}

bool validMailToken(const std::string_view value) noexcept {
  return !value.empty() && value.size() <= 4096 &&
         value.find_first_of("\r\n") == std::string_view::npos &&
         value.find('\0') == std::string_view::npos;
}

CurlProgressContext::CurlProgressContext(
    const std::atomic_bool &cancel,
    const std::chrono::steady_clock::time_point deadline,
    std::function<bool()> preflight)
    : cancel_(cancel), deadline_(deadline), preflight_(std::move(preflight)) {}

int CurlProgressContext::callback(void *context, curl_off_t, curl_off_t,
                                  curl_off_t, curl_off_t) noexcept {
  const auto &self = *static_cast<CurlProgressContext *>(context);
  if (self.cancel_.load(std::memory_order_relaxed) ||
      std::chrono::steady_clock::now() >= self.deadline_)
    return 1;
  try {
    return self.preflight_ && !self.preflight_() ? 1 : 0;
  } catch (...) {
    return 1;
  }
}

MailSigpipeBlock::MailSigpipeBlock() {
  sigemptyset(&set_);
  sigaddset(&set_, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0) {
    active_ = true;
    sigset_t pending{};
    if (sigpending(&pending) == 0)
      wasPending_ = sigismember(&pending, SIGPIPE) == 1;
  }
}

MailSigpipeBlock::~MailSigpipeBlock() {
  if (!active_) return;
  if (!wasPending_) {
    timespec noWait{};
    while (sigtimedwait(&set_, nullptr, &noWait) == SIGPIPE) {
    }
  }
  static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
}

namespace {

constexpr std::size_t maxImapMessages     = 4096;
constexpr std::size_t maxImapMessageBytes = 32 * 1024 * 1024;
constexpr std::size_t maxImapTotalBytes   = 128 * 1024 * 1024;
constexpr long maxImapTimeoutMilliseconds = 300000;

#ifdef GLEDITOR_HAVE_MAIL
constexpr std::size_t maxImapControlBytes = 1024 * 1024;

struct CurlCloser {
  void operator()(CURL *handle) const {
    if (!handle) return;
    MailSigpipeBlock block;
    curl_easy_cleanup(handle);
  }
};

struct CurlMultiCloser {
  void operator()(CURLM *multi) const {
    if (!multi) return;
    MailSigpipeBlock block;
    curl_multi_cleanup(multi);
  }
};

struct CurlMailStreamContext {
  CURL *easy{};
  curl_socket_t socket{CURL_SOCKET_BAD};
  const std::atomic_bool *cancel{};
  std::chrono::steady_clock::time_point deadline;
  std::function<bool()> preflight;
  std::size_t wireBytes{};
  std::size_t wireByteLimit{};
  std::atomic_bool cancelled{};
  bool expired{};

  [[nodiscard]] bool permitted() noexcept {
    if (cancelled.load(std::memory_order_relaxed) ||
        (cancel && cancel->load(std::memory_order_relaxed)))
      return false;
    if (std::chrono::steady_clock::now() >= deadline) {
      expired = true;
      return false;
    }
    try {
      return !preflight || preflight();
    } catch (...) {
      return false;
    }
  }
};

bool waitCurlSocket(CurlMailStreamContext &context, const short events) {
  while (context.permitted()) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            context.deadline - std::chrono::steady_clock::now())
            .count();
    if (remaining <= 0) {
      context.expired = true;
      return false;
    }
    pollfd descriptor{.fd = static_cast<int>(context.socket), .events = events};
    const auto waitMilliseconds =
        static_cast<int>(std::clamp<std::int64_t>(remaining, 1, 50));
    const auto ready = ::poll(&descriptor, 1, waitMilliseconds);
    if (ready < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ready == 0) continue;
    if (descriptor.revents & (POLLERR | POLLNVAL)) return false;
    if ((descriptor.revents & events) ||
        ((events & POLLIN) && (descriptor.revents & POLLHUP)))
      return true;
    if (descriptor.revents & POLLHUP) return false;
  }
  return false;
}

ssize_t curlMailStreamRead(mailstream_low *low, void *buffer,
                           const std::size_t size) {
  auto &context = *static_cast<CurlMailStreamContext *>(low->data);
  if (!context.permitted() ||
      size > context.wireByteLimit -
                 std::min(context.wireBytes, context.wireByteLimit))
    return -1;
  while (context.permitted()) {
    std::size_t received{};
    CURLcode result{};
    {
      MailSigpipeBlock sigpipe;
      result = curl_easy_recv(context.easy, buffer, size, &received);
    }
    if (result == CURLE_OK) {
      if (received > context.wireByteLimit -
                         std::min(context.wireBytes, context.wireByteLimit))
        return -1;
      context.wireBytes += received;
      return static_cast<ssize_t>(received);
    }
    if (result != CURLE_AGAIN || !waitCurlSocket(context, POLLIN)) return -1;
  }
  return -1;
}

ssize_t curlMailStreamWrite(mailstream_low *low, const void *buffer,
                            const std::size_t size) {
  auto &context = *static_cast<CurlMailStreamContext *>(low->data);
  std::size_t sent{};
  const auto *bytes = static_cast<const char *>(buffer);
  while (sent < size && context.permitted()) {
    std::size_t written{};
    CURLcode result{};
    {
      MailSigpipeBlock sigpipe;
      result =
          curl_easy_send(context.easy, bytes + sent, size - sent, &written);
    }
    if (result == CURLE_OK) {
      if (!written) return -1;
      sent += written;
      continue;
    }
    if (result != CURLE_AGAIN || !waitCurlSocket(context, POLLOUT)) return -1;
  }
  return sent == size ? static_cast<ssize_t>(sent) : -1;
}

int curlMailStreamClose(mailstream_low *) { return 0; }

int curlMailStreamFd(mailstream_low *low) {
  const auto &context = *static_cast<CurlMailStreamContext *>(low->data);
  return static_cast<int>(context.socket);
}

void curlMailStreamFree(mailstream_low *low) {
  delete static_cast<CurlMailStreamContext *>(low->data);
}

void curlMailStreamCancel(mailstream_low *low) {
  auto &context = *static_cast<CurlMailStreamContext *>(low->data);
  context.cancelled.store(true, std::memory_order_relaxed);
}

struct mailstream_cancel *curlMailStreamGetCancel(mailstream_low *) {
  return nullptr;
}

mailstream_low_driver curlMailStreamDriver{curlMailStreamRead,
                                           curlMailStreamWrite,
                                           curlMailStreamClose,
                                           curlMailStreamFd,
                                           curlMailStreamFree,
                                           curlMailStreamCancel,
                                           curlMailStreamGetCancel,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr};
#endif // GLEDITOR_HAVE_MAIL

std::optional<std::string> quoteMailbox(const std::string_view mailbox) {
  if (mailbox.empty() || mailbox.size() > 1024 ||
      std::ranges::any_of(
          mailbox, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }))
    return {};
  std::string result{"\""};
  for (const char value : mailbox) {
    if (value == '"' || value == '\\') result.push_back('\\');
    result.push_back(value);
  }
  result.push_back('"');
  return result;
}

#ifdef GLEDITOR_HAVE_MAIL
std::string
framedIdentity(const std::initializer_list<std::string_view> components) {
  std::string result;
  for (const auto component : components) {
    result += std::to_string(component.size());
    result.push_back(':');
    result.append(component);
  }
  return result;
}

std::size_t retainedMailMessageRecordCount(const MailMessageEvidence &message) {
  std::size_t records = 2 +
                        static_cast<std::size_t>(!message.rawMessage.empty()) +
                        message.headers.size() + message.recipients.size() +
                        (message.flags ? message.flags->size() : 0);
  for (const auto &part : message.parts) {
    records += 1 + static_cast<std::size_t>(!part.rawBytes.empty()) +
               static_cast<std::size_t>(part.decodedBytes.has_value()) +
               static_cast<std::size_t>(part.decodedText.has_value()) +
               part.headers.size() + part.embeddedMessageHeaders.size();
  }
  return records;
}

bool retainedMailObservationFits(const MailObservation &observation,
                                 const MailMessageEvidence *extraMessage) {
  std::size_t bytes{};
  const auto charge = [&](const std::string_view value) {
    if (value.size() > observation.bounds.maxStringBytes ||
        value.size() > observation.bounds.maxBytes -
                           std::min(bytes, observation.bounds.maxBytes))
      return false;
    bytes += value.size();
    return true;
  };
  const auto chargeMessage = [&](const MailMessageEvidence &message) {
    if (!charge(message.messageId) || !charge(message.subject) ||
        !charge(message.rawMessage))
      return false;
    for (const auto &header : message.headers)
      if (!charge(header.name) || !charge(header.value) ||
          !charge(header.rawValue))
        return false;
    for (const auto &recipient : message.recipients)
      if (!charge(recipient.role) || !charge(recipient.address)) return false;
    if (message.flags)
      for (const auto &flag : *message.flags)
        if (!charge(flag)) return false;
    for (const auto &part : message.parts) {
      if (!charge(part.key) || !charge(part.contentType) ||
          !charge(part.disposition) || !charge(part.charset) ||
          !charge(part.transferEncoding) || !charge(part.filename) ||
          !charge(part.contentId) || !charge(part.contentLocation) ||
          !charge(part.contentDescription) ||
          !charge(part.charsetConversionStatus) || !charge(part.rawBytes) ||
          (part.decodedBytes && !charge(*part.decodedBytes)) ||
          (part.decodedText && !charge(*part.decodedText)))
        return false;
      for (const auto &header : part.headers)
        if (!charge(header.name) || !charge(header.value) ||
            !charge(header.rawValue))
          return false;
      for (const auto &header : part.embeddedMessageHeaders)
        if (!charge(header.name) || !charge(header.value) ||
            !charge(header.rawValue))
          return false;
    }
    return true;
  };

  constexpr std::string_view maximumCoverageText =
      "UIDVALIDITY 18446744073709551615; 4096 message(s) retained";
  if (!charge(observation.serverAuthority) ||
      !charge(observation.accountScope) || !charge(observation.mailbox) ||
      !charge(observation.mailboxIncarnation) || !charge(maximumCoverageText))
    return false;
  for (const auto &message : observation.messages)
    if (!chargeMessage(message)) return false;
  if (extraMessage && !chargeMessage(*extraMessage)) return false;
  for (const auto &gap : observation.gaps)
    if (!charge(gap.label) || !charge(gap.detail) || !charge(gap.cursor))
      return false;
  return true;
}

struct ImapBodyCapture {
  std::string bytes;
  std::optional<std::size_t> advertisedLength;
  std::size_t messageLimit{};
  std::size_t totalLimit{};
  std::size_t &totalBytes;
  std::size_t totalBytesAtStart{};
  bool callbackObserved{};
  bool exceeded{};
  bool malformed{};

  ~ImapBodyCapture() { clean(bytes); }
};

struct SecretWitness {
  std::string value;
  ~SecretWitness() { clean(value); }
};

void escapedImapPassword(std::string &escaped,
                         const std::string_view password) {
  escaped.clear();
  escaped.reserve(password.size());
  for (const char value : password) {
    if (value == '\\' || value == '"') escaped.push_back('\\');
    escaped.push_back(value);
  }
}

bool echoesCredential(const std::string_view value,
                      const MailCredentialSecret *secret,
                      const std::string_view escapedPassword) {
  if (!secret) return false;
  return value.find(secret->password) != std::string_view::npos ||
         (!escapedPassword.empty() &&
          value.find(escapedPassword) != std::string_view::npos);
}

bool captureImapBody(const int attributeType,
                     mailimap_msg_att_body_section *section, const char *bytes,
                     const std::size_t length, void *context) noexcept {
  if (!context) return false;
  auto &capture = *static_cast<ImapBodyCapture *>(context);
  if (attributeType != MAILIMAP_MSG_ATT_BODY_SECTION || !section ||
      (!bytes && length)) {
    capture.malformed = true;
    return false;
  }
  capture.callbackObserved = true;
  if (section->sec_length && !capture.advertisedLength) {
    if (section->sec_length > capture.messageLimit ||
        section->sec_length >
            capture.totalLimit -
                std::min(capture.totalBytes, capture.totalLimit)) {
      capture.exceeded = true;
      return false;
    }
    capture.advertisedLength = section->sec_length;
  } else if (section->sec_length && capture.advertisedLength &&
             *capture.advertisedLength != section->sec_length) {
    capture.malformed = true;
    return false;
  }
  if (length > capture.messageLimit -
                   std::min(capture.bytes.size(), capture.messageLimit) ||
      length > capture.totalLimit -
                   std::min(capture.totalBytes, capture.totalLimit)) {
    capture.exceeded = true;
    return false;
  }
  try {
    capture.bytes.append(bytes, length);
  } catch (...) {
    capture.exceeded = true;
    return false;
  }
  capture.totalBytes += length;
  return true;
}
#endif // GLEDITOR_HAVE_MAIL

template <typename State>
bool validLeaseFor(State &state, const MailEndpoint &endpoint,
                   const std::string_view accountScope) {
  return state.usable() &&
         state.scope.operation == MailCredentialOperation::Read &&
         state.scope.serverAuthority == endpoint.authority &&
         state.scope.accountScope == accountScope;
}

std::optional<std::string>
imapProblem(const ImapReadProfile &profile, const ImapReadRequest &request,
            std::optional<MailEndpoint> *resolvedEndpoint = nullptr) {
  auto endpoint = canonicalMailEndpoint(profile.serverUrl, "imaps", "imap");
  if (!endpoint || profile.serverAuthority != endpoint->authority ||
      !validMailToken(profile.accountScope) || !quoteMailbox(profile.mailbox) ||
      profile.serverUrl.size() > 4096 ||
      profile.serverUrl.size() > request.retentionBounds.maxStringBytes ||
      profile.caBundlePath.size() > 4096 || !request.maxMessages ||
      request.maxMessages > maxImapMessages ||
      request.retentionBounds.maxRecords < 8 ||
      request.retentionBounds.maxRecords > maxImapMessages ||
      !request.retentionBounds.maxBytes ||
      request.retentionBounds.maxBytes > 8 * 1024 * 1024 ||
      request.retentionBounds.maxStringBytes < 64 ||
      request.retentionBounds.maxStringBytes > 1024 * 1024 ||
      !request.retentionBounds.maxParts ||
      request.retentionBounds.maxParts > 256 ||
      !request.retentionBounds.maxPartDepth ||
      request.retentionBounds.maxPartDepth > 24 || !request.maxMessageBytes ||
      request.maxMessageBytes > maxImapMessageBytes || !request.maxTotalBytes ||
      request.maxTotalBytes > maxImapTotalBytes ||
      request.maxMessageBytes > request.retentionBounds.maxStringBytes ||
      request.maxTotalBytes > request.retentionBounds.maxBytes ||
      profile.serverAuthority.size() > request.retentionBounds.maxStringBytes ||
      profile.accountScope.size() > request.retentionBounds.maxStringBytes ||
      profile.mailbox.size() > request.retentionBounds.maxStringBytes ||
      request.timeoutMilliseconds <= 0 ||
      request.timeoutMilliseconds > maxImapTimeoutMilliseconds)
    return "invalid IMAP endpoint, mailbox or capture bounds";
  if (!endpoint->secure && (!profile.allowCleartextLoopbackWithoutCredentials ||
                            !isLiteralLoopback(endpoint->host)))
    return "cleartext IMAP is restricted to unauthenticated loopback tests";
  if (resolvedEndpoint) *resolvedEndpoint = std::move(endpoint);
  return {};
}

} // namespace

std::optional<std::string> imapReadProblem(const ImapReadProfile &profile,
                                           const ImapReadRequest &request) {
  return imapProblem(profile, request);
}

std::optional<std::string>
imapServerAuthority(const std::string_view serverUrl) {
  const auto endpoint = canonicalMailEndpoint(serverUrl, "imaps", "imap");
  if (!endpoint) return {};
  return endpoint->authority;
}

#ifdef GLEDITOR_HAVE_MAIL
MailObservation readImapMailbox(const ImapReadProfile &profile,
                                const ImapReadRequest &request,
                                const MailCredentialLease &credential,
                                const std::atomic_bool &cancellation) {
  MailObservation observation;
  observation.state    = CaptureState::Failed;
  observation.bounds   = request.retentionBounds;
  observation.coverage = "No mailbox coverage established";
  const auto maximumGaps =
      std::min<std::size_t>(8, request.retentionBounds.maxRecords / 4);
  const auto addGap = [&](std::string label, std::string detail,
                          std::string cursor = {}) {
    if (observation.gaps.size() < maximumGaps)
      observation.gaps.push_back(
          {std::move(label), std::move(detail), std::move(cursor)});
  };
  std::optional<MailEndpoint> resolvedEndpoint;
  if (const auto problem = imapProblem(profile, request, &resolvedEndpoint)) {
    addGap("profile", *problem);
    return observation;
  }
  const auto &endpoint        = *resolvedEndpoint;
  observation.serverAuthority = endpoint.authority;
  observation.accountScope    = profile.accountScope;
  observation.mailbox         = profile.mailbox;

  auto lease               = MailCredentialInternalAccess::state(credential);
  const bool hasCredential = static_cast<bool>(lease);
  if (hasCredential && !validLeaseFor(*lease, endpoint, profile.accountScope)) {
    addGap("credentials", "mail credential lease is invalid or mismatched");
    return observation;
  }
  if (!endpoint.secure &&
      (!profile.allowCleartextLoopbackWithoutCredentials ||
       !isLiteralLoopback(endpoint.host) || hasCredential)) {
    addGap("transport",
           "cleartext IMAP is restricted to unauthenticated loopback tests");
    return observation;
  }
  if (cancellation.load(std::memory_order_relaxed)) {
    observation.state = CaptureState::Cancelled;
    addGap("cancelled", "IMAP read cancelled before connection");
    return observation;
  }

  try {
    initializeMailCurl();
  } catch (const std::exception &) {
    addGap("transport", "mail transport initialization failed");
    return observation;
  }
  CURL *rawHandle = curl_easy_init();
  if (!rawHandle) {
    addGap("transport", "IMAP connection allocation failed");
    return observation;
  }
  const std::unique_ptr<CURL, CurlCloser> handle(rawHandle);
  CURLM *rawMulti = curl_multi_init();
  if (!rawMulti) {
    addGap("transport", "IMAP connection allocation failed");
    return observation;
  }
  const std::unique_ptr<CURLM, CurlMultiCloser> multi(rawMulti);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(request.timeoutMilliseconds);
  const auto connectUrl = curlConnectOnlyUrl(endpoint);
  if (!connectUrl) {
    addGap("transport", "IMAP connection URL could not be prepared");
    return observation;
  }
  const auto preflight = [&]() {
    if (cancellation.load(std::memory_order_relaxed)) return false;
    return !lease || validLeaseFor(*lease, endpoint, profile.accountScope);
  };

  curl_easy_setopt(handle.get(), CURLOPT_URL, connectUrl->c_str());
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_NETRC, CURL_NETRC_IGNORED);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   std::min<long>(request.timeoutMilliseconds, 15000));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS,
                   request.timeoutMilliseconds);
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  if (endpoint.secure && !profile.caBundlePath.empty())
    curl_easy_setopt(handle.get(), CURLOPT_CAINFO,
                     profile.caBundlePath.c_str());
  const auto connectResult = performMailCurlBounded(
      handle.get(), cancellation, deadline, preflight, multi.get(), true);
  if (connectResult != CURLE_OK) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Failed;
    addGap("transport", cancellation.load(std::memory_order_relaxed)
                            ? "IMAP connection cancelled"
                        : std::chrono::steady_clock::now() >= deadline
                            ? "IMAP connection deadline elapsed"
                            : "IMAP connection or verified TLS failed");
    return observation;
  }
  curl_socket_t activeSocket{CURL_SOCKET_BAD};
  if (curl_easy_getinfo(handle.get(), CURLINFO_ACTIVESOCKET, &activeSocket) !=
          CURLE_OK ||
      activeSocket == CURL_SOCKET_BAD) {
    addGap("transport", "IMAP connection did not expose an active socket");
    return observation;
  }

  auto streamContext           = std::make_unique<CurlMailStreamContext>();
  streamContext->easy          = handle.get();
  streamContext->socket        = activeSocket;
  streamContext->cancel        = &cancellation;
  streamContext->deadline      = deadline;
  streamContext->preflight     = preflight;
  streamContext->wireByteLimit = request.maxTotalBytes + maxImapControlBytes;
  auto *low = mailstream_low_new(streamContext.get(), &curlMailStreamDriver);
  if (!low) {
    addGap("transport", "IMAP parser stream allocation failed");
    return observation;
  }
  streamContext.release();
  mailstream *stream = mailstream_new(low, 4096);
  if (!stream) {
    mailstream_low_free(low);
    addGap("transport", "IMAP parser stream allocation failed");
    return observation;
  }
  mailimap *rawSession = mailimap_new(0, nullptr);
  if (!rawSession) {
    mailstream_close(stream);
    addGap("transport", "IMAP parser session allocation failed");
    return observation;
  }
  const std::unique_ptr<mailimap, decltype(&mailimap_free)> session(
      rawSession, mailimap_free);
  const auto connectStatus = mailimap_connect(session.get(), stream);
  if (session->imap_stream != stream) mailstream_close(stream);
  if (connectStatus != MAILIMAP_NO_ERROR &&
      connectStatus != MAILIMAP_NO_ERROR_AUTHENTICATED &&
      connectStatus != MAILIMAP_NO_ERROR_NON_AUTHENTICATED) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Failed;
    addGap("greeting", cancellation.load(std::memory_order_relaxed)
                           ? "IMAP greeting cancelled"
                       : std::chrono::steady_clock::now() >= deadline
                           ? "IMAP greeting deadline elapsed"
                           : "LibEtPan rejected the IMAP greeting");
    return observation;
  }
  mailimap_set_timeout(
      session.get(),
      std::max<time_t>(1, (request.timeoutMilliseconds + 999) / 1000));
  observation.state = CaptureState::Partial;

  if (!preflight()) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Partial;
    addGap("CAPABILITY", "IMAP authority expired before capability query");
    return observation;
  }
  mailimap_capability_data *rawCapabilities{};
  const auto capabilityStatus =
      mailimap_capability(session.get(), &rawCapabilities);
  const std::unique_ptr<mailimap_capability_data,
                        decltype(&mailimap_capability_data_free)>
      capabilities(rawCapabilities, mailimap_capability_data_free);
  if (capabilityStatus != MAILIMAP_NO_ERROR || !capabilities ||
      !capabilities->cap_list) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Partial;
    addGap("CAPABILITY", cancellation.load(std::memory_order_relaxed)
                             ? "IMAP capability query cancelled"
                             : "LibEtPan could not read IMAP capabilities");
    return observation;
  }
  bool loginDisabled{};
  for (auto *iterator = clist_begin(capabilities->cap_list); iterator;
       iterator       = clist_next(iterator)) {
    const auto *item = static_cast<const struct mailimap_capability *>(
        clist_content(iterator));
    if (item && item->cap_type == MAILIMAP_CAPABILITY_NAME &&
        item->cap_data.cap_name &&
        lower(item->cap_data.cap_name) == "logindisabled") {
      loginDisabled = true;
      break;
    }
  }
  if (loginDisabled && hasCredential) {
    addGap("login", "server disabled LOGIN; no credential was sent");
    return observation;
  }

  std::optional<MailCredentialSecret> secret;
  SecretWitness escapedPassword;
  if (hasCredential) {
    if (!preflight()) {
      observation.state = cancellation.load(std::memory_order_relaxed)
                              ? CaptureState::Cancelled
                              : CaptureState::Failed;
      addGap("credentials", "mail credential lease expired before login");
      return observation;
    }
    secret = MailCredentialInternalAccess::secret(credential);
    if (!secret) {
      addGap("credentials", "mail credential lease expired before login");
      return observation;
    }
    if (mailimap_login(session.get(), secret->username.c_str(),
                       secret->password.c_str()) != MAILIMAP_NO_ERROR) {
      observation.state = cancellation.load(std::memory_order_relaxed)
                              ? CaptureState::Cancelled
                              : CaptureState::Failed;
      addGap("login", cancellation.load(std::memory_order_relaxed)
                          ? "IMAP login cancelled"
                          : "IMAP server rejected or failed login");
      return observation;
    }
    escapedImapPassword(escapedPassword.value, secret->password);
  }

  if (!preflight()) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Failed;
    addGap("EXAMINE", "IMAP request authority expired before mailbox access");
    return observation;
  }
  std::unique_ptr<char, decltype(&std::free)> encodedMailbox(
      charconv_encode_mutf7(profile.mailbox.c_str()), std::free);
  if (!encodedMailbox ||
      mailimap_examine(session.get(), encodedMailbox.get()) !=
          MAILIMAP_NO_ERROR) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Partial;
    addGap("EXAMINE", cancellation.load(std::memory_order_relaxed)
                          ? "IMAP mailbox examination cancelled"
                          : "IMAP read-only mailbox examination failed");
    return observation;
  }
  const auto *selection           = session->imap_selection_info;
  const std::uint64_t uidValidity = selection ? selection->sel_uidvalidity : 0;
  if (!uidValidity) {
    addGap("UIDVALIDITY", "EXAMINE response omitted a valid UIDVALIDITY");
    return observation;
  }
  const auto uidValidityText = std::to_string(uidValidity);
  observation.mailboxIncarnation =
      framedIdentity({endpoint.authority, profile.accountScope, profile.mailbox,
                      uidValidityText});
  observation.coverage =
      "UIDVALIDITY " + uidValidityText + "; 0 messages retained";
  if (observation.mailboxIncarnation.size() >
          observation.bounds.maxStringBytes ||
      observation.coverage.size() > observation.bounds.maxStringBytes) {
    addGap("UIDVALIDITY", "mailbox identity exceeds retention bounds");
    return observation;
  }
  if (request.afterUid && !request.afterUidValidity) {
    addGap("cursor", "UID cursor requires its UIDVALIDITY witness",
           std::to_string(request.afterUid));
    return observation;
  }
  std::uint32_t afterUid = request.afterUid;
  if (request.afterUidValidity && *request.afterUidValidity != uidValidity) {
    addGap("UIDVALIDITY", "mailbox incarnation changed; cursor reset to UID 1",
           std::to_string(request.afterUid));
    afterUid = 0;
  }

  clist *rawSearchResults{};
  auto *searchKey = mailimap_search_key_new_all();
  if (!searchKey) {
    addGap("UID SEARCH", "LibEtPan could not allocate UID search");
    return observation;
  }
  const auto searchStatus =
      mailimap_uid_search(session.get(), nullptr, searchKey, &rawSearchResults);
  mailimap_search_key_free(searchKey);
  const std::unique_ptr<clist, decltype(&mailimap_search_result_free)>
      searchResults(rawSearchResults, mailimap_search_result_free);
  if (searchStatus != MAILIMAP_NO_ERROR || !searchResults) {
    observation.state = cancellation.load(std::memory_order_relaxed)
                            ? CaptureState::Cancelled
                            : CaptureState::Partial;
    addGap("UID SEARCH", cancellation.load(std::memory_order_relaxed)
                             ? "UID search cancelled"
                             : "LibEtPan could not complete UID SEARCH ALL");
    return observation;
  }

  std::vector<std::uint32_t> requested;
  requested.reserve(std::min<std::size_t>(
      static_cast<std::size_t>(clist_count(searchResults.get())),
      request.maxMessages));
  std::uint32_t previousUid{};
  std::size_t laterMessageCount{};
  bool invalidSearchResults{};
  for (auto *iterator = clist_begin(searchResults.get()); iterator;
       iterator       = clist_next(iterator)) {
    const auto *value =
        static_cast<const std::uint32_t *>(clist_content(iterator));
    if (!value || !*value || *value <= previousUid) {
      invalidSearchResults = true;
      break;
    }
    previousUid = *value;
    if (*value > afterUid) {
      ++laterMessageCount;
      if (requested.size() < request.maxMessages) requested.push_back(*value);
    }
  }
  if (invalidSearchResults) {
    addGap("UID SEARCH", "UID search returned an invalid or unordered UID set");
    return observation;
  }
  const bool messageLimitReached = laterMessageCount > request.maxMessages;

  auto *fetchType = mailimap_fetch_type_new_fetch_att_list_empty();
  if (!fetchType) {
    addGap("FETCH", "LibEtPan could not allocate the FETCH request");
    return observation;
  }
  const std::unique_ptr<mailimap_fetch_type,
                        decltype(&mailimap_fetch_type_free)>
      ownedFetchType(fetchType, mailimap_fetch_type_free);
  const auto addFetchAttribute = [&](mailimap_fetch_att *attribute) {
    if (!attribute) return false;
    if (mailimap_fetch_type_new_fetch_att_list_add(fetchType, attribute) ==
        MAILIMAP_NO_ERROR)
      return true;
    mailimap_fetch_att_free(attribute);
    return false;
  };
  if (!addFetchAttribute(mailimap_fetch_att_new_uid()) ||
      !addFetchAttribute(mailimap_fetch_att_new_flags())) {
    addGap("FETCH", "LibEtPan could not allocate FETCH metadata attributes");
    return observation;
  }
  auto *section = mailimap_section_new(nullptr);
  if (!section ||
      !addFetchAttribute(mailimap_fetch_att_new_body_peek_section(section))) {
    if (section) mailimap_section_free(section);
    addGap("FETCH", "LibEtPan could not allocate BODY.PEEK[]");
    return observation;
  }

  std::size_t downloadedRawBytes{};
  for (const auto uid : requested) {
    if (!preflight()) {
      observation.state = cancellation.load(std::memory_order_relaxed)
                              ? CaptureState::Cancelled
                              : CaptureState::Partial;
      addGap("FETCH",
             cancellation.load(std::memory_order_relaxed)
                 ? "IMAP read cancelled"
                 : "mail credential lease expired during IMAP read",
             std::to_string(uid));
      break;
    }
    auto *set = mailimap_set_new_single(uid);
    if (!set) {
      addGap("FETCH", "LibEtPan could not allocate a UID set",
             std::to_string(uid));
      break;
    }
    const std::unique_ptr<mailimap_set, decltype(&mailimap_set_free)> uidSet(
        set, mailimap_set_free);
    ImapBodyCapture body{.messageLimit      = request.maxMessageBytes,
                         .totalLimit        = request.maxTotalBytes,
                         .totalBytes        = downloadedRawBytes,
                         .totalBytesAtStart = downloadedRawBytes};
    mailimap_set_msg_body_handler(session.get(), captureImapBody, &body);
    clist *rawFetchResults{};
    const auto fetchStatus = mailimap_uid_fetch(session.get(), uidSet.get(),
                                                fetchType, &rawFetchResults);
    mailimap_set_msg_body_handler(session.get(), nullptr, nullptr);
    const std::unique_ptr<clist, decltype(&mailimap_fetch_list_free)>
        fetchResults(rawFetchResults, mailimap_fetch_list_free);
    if (body.exceeded || body.malformed) {
      addGap("FETCH",
             body.exceeded ? "message literal exceeded its configured bounds"
                           : "LibEtPan returned malformed message literal data",
             std::to_string(uid));
      break;
    }
    if (fetchStatus != MAILIMAP_NO_ERROR || !fetchResults ||
        clist_count(fetchResults.get()) != 1) {
      addGap("FETCH",
             cancellation.load(std::memory_order_relaxed)
                 ? "IMAP message fetch cancelled"
                 : "LibEtPan did not return one message for the requested UID",
             std::to_string(uid));
      if (cancellation.load(std::memory_order_relaxed))
        observation.state = CaptureState::Cancelled;
      break;
    }
    auto *attributes = static_cast<mailimap_msg_att *>(
        clist_content(clist_begin(fetchResults.get())));
    if (!attributes || !attributes->att_number) {
      addGap("FETCH", "FETCH result omitted the message sequence number",
             std::to_string(uid));
      break;
    }
    std::optional<std::uint32_t> returnedUid;
    std::optional<std::vector<std::string>> flags;
    bool hasBodyAttribute{};
    bool duplicateCriticalAttribute{};
    bool invalidFlag{};
    for (auto *itemIterator = clist_begin(attributes->att_list); itemIterator;
         itemIterator       = clist_next(itemIterator)) {
      const auto *item = static_cast<const mailimap_msg_att_item *>(
          clist_content(itemIterator));
      if (!item) continue;
      if (item->att_type == MAILIMAP_MSG_ATT_ITEM_STATIC &&
          item->att_data.att_static) {
        const auto *value = item->att_data.att_static;
        if (value->att_type == MAILIMAP_MSG_ATT_UID) {
          if (returnedUid) {
            duplicateCriticalAttribute = true;
          } else {
            returnedUid = value->att_data.att_uid;
          }
        } else if (value->att_type == MAILIMAP_MSG_ATT_BODY_SECTION) {
          if (hasBodyAttribute) {
            duplicateCriticalAttribute = true;
            continue;
          }
          hasBodyAttribute = true;
          if (value->att_data.att_body_section) {
            const auto parsedLength =
                value->att_data.att_body_section->sec_length;
            // LibEtPan streams literal chunks through the body handler and
            // may leave sec_length at zero in the final section object. A
            // successful FETCH response plus the complete callback byte count
            // is then the parser's exact consumed literal length.
            body.advertisedLength = parsedLength || !body.callbackObserved
                                        ? parsedLength
                                        : body.bytes.size();
            if (*body.advertisedLength > body.messageLimit ||
                *body.advertisedLength >
                    body.totalLimit -
                        std::min(body.totalBytesAtStart, body.totalLimit))
              body.exceeded = true;
          } else {
            body.malformed = true;
          }
        }
      } else if (item->att_type == MAILIMAP_MSG_ATT_ITEM_DYNAMIC &&
                 item->att_data.att_dyn) {
        if (flags) {
          duplicateCriticalAttribute = true;
          continue;
        }
        flags.emplace();
        const auto *list = item->att_data.att_dyn->att_list;
        for (auto *flagIterator = clist_begin(list); flagIterator;
             flagIterator       = clist_next(flagIterator)) {
          const auto *fetchedFlag = static_cast<const mailimap_flag_fetch *>(
              clist_content(flagIterator));
          if (!fetchedFlag) {
            invalidFlag = true;
            break;
          }
          if (fetchedFlag->fl_type == MAILIMAP_FLAG_FETCH_RECENT) {
            flags->emplace_back("\\Recent");
            continue;
          }
          const auto *flag = fetchedFlag->fl_flag;
          if (!flag) {
            invalidFlag = true;
            break;
          }
          switch (flag->fl_type) {
          case MAILIMAP_FLAG_ANSWERED:
            flags->emplace_back("\\Answered");
            break;
          case MAILIMAP_FLAG_FLAGGED:
            flags->emplace_back("\\Flagged");
            break;
          case MAILIMAP_FLAG_DELETED:
            flags->emplace_back("\\Deleted");
            break;
          case MAILIMAP_FLAG_SEEN:
            flags->emplace_back("\\Seen");
            break;
          case MAILIMAP_FLAG_DRAFT:
            flags->emplace_back("\\Draft");
            break;
          case MAILIMAP_FLAG_KEYWORD:
            if (!flag->fl_data.fl_keyword) {
              invalidFlag = true;
              break;
            }
            flags->emplace_back(flag->fl_data.fl_keyword);
            break;
          case MAILIMAP_FLAG_EXTENSION:
            if (!flag->fl_data.fl_extension) {
              invalidFlag = true;
              break;
            }
            flags->emplace_back(flag->fl_data.fl_extension);
            break;
          default:
            invalidFlag = true;
            break;
          }
          if (invalidFlag) break;
        }
      }
    }
    if (duplicateCriticalAttribute) {
      if (flags)
        for (auto &flag : *flags) clean(flag);
      clean(body.bytes);
      addGap("FETCH", "duplicate critical FETCH attribute",
             std::to_string(uid));
      break;
    }
    if (!returnedUid || *returnedUid != uid) {
      addGap("FETCH", "FETCH response UID did not match the requested UID",
             std::to_string(uid));
      break;
    }
    if (body.exceeded || body.malformed) {
      addGap("FETCH",
             body.exceeded ? "message literal exceeded its configured bounds"
                           : "LibEtPan returned malformed message literal data",
             std::to_string(uid));
      break;
    }
    if (!body.advertisedLength || body.bytes.size() != *body.advertisedLength ||
        !hasBodyAttribute) {
      addGap("FETCH", "FETCH response omitted or truncated BODY.PEEK[]",
             std::to_string(uid));
      break;
    }
    if (invalidFlag) {
      addGap("FLAGS", "FETCH returned malformed message flags",
             std::to_string(uid));
      flags.reset();
    } else if (!flags) {
      addGap("FLAGS", "FETCH response omitted FLAGS metadata",
             std::to_string(uid));
    }

    const auto *secretWitness = secret ? &*secret : nullptr;
    bool echoedCredential =
        echoesCredential(body.bytes, secretWitness, escapedPassword.value);
    if (flags) {
      echoedCredential =
          echoedCredential ||
          std::ranges::any_of(*flags, [&](const auto &flag) {
            return echoesCredential(flag, secretWitness, escapedPassword.value);
          });
    }
    if (echoedCredential) {
      if (flags)
        for (auto &flag : *flags) clean(flag);
      flags.reset();
      clean(body.bytes);
      addGap("FETCH", "message withheld because it echoed an active credential",
             std::to_string(uid));
      continue;
    }

    MimeParseLimits mimeLimits;
    mimeLimits.maxMessageBytes = request.maxMessageBytes;
    mimeLimits.maxParts        = request.retentionBounds.maxParts;
    mimeLimits.maxDepth        = request.retentionBounds.maxPartDepth;
    mimeLimits.maxDecodedBytes = request.retentionBounds.maxStringBytes;
    mimeLimits.maxHeaders      = 4096;
    mimeLimits.maxRecipients   = request.retentionBounds.maxRecords;
    try {
      auto message  = parseMimeMessage(body.bytes, uidValidity, uid,
                                       attributes->att_number, mimeLimits);
      message.flags = std::move(flags);
      std::size_t recordsUsed = 1 + observation.gaps.size();
      for (const auto &prior : observation.messages)
        recordsUsed += retainedMailMessageRecordCount(prior);
      const auto messageRecords = retainedMailMessageRecordCount(message);
      if (messageRecords > request.retentionBounds.maxRecords -
                               std::min(recordsUsed + 1,
                                        request.retentionBounds.maxRecords) ||
          !retainedMailObservationFits(observation, &message)) {
        addGap("retention", "record budget left this message unretained",
               std::to_string(uid));
        break;
      }
      observation.messages.push_back(std::move(message));
    } catch (const std::exception &) {
      addGap("MIME", "fetched message could not be parsed within limits",
             std::to_string(uid));
    }
  }
  if (messageLimitReached)
    addGap("UID SEARCH", "message count limit left later UIDs unobserved",
           requested.empty() ? std::to_string(afterUid)
                             : std::to_string(requested.back()));
  if (observation.gaps.empty())
    observation.state = CaptureState::Complete;
  else if (observation.state != CaptureState::Cancelled)
    observation.state = CaptureState::Partial;
  observation.coverage = "UIDVALIDITY " + std::to_string(uidValidity) + "; " +
                         std::to_string(observation.messages.size()) +
                         " message(s) retained";
  if (preflight()) static_cast<void>(mailimap_logout(session.get()));
  return observation;
}

#else // !GLEDITOR_HAVE_MAIL -- the refusing stub

MailObservation readImapMailbox(const ImapReadProfile &profile,
                                const ImapReadRequest &request,
                                const MailCredentialLease & /*credential*/,
                                const std::atomic_bool & /*cancellation*/) {
  MailObservation observation;
  observation.state    = CaptureState::Failed;
  observation.bounds   = request.retentionBounds;
  observation.coverage = "No mailbox coverage established";
  std::optional<MailEndpoint> resolvedEndpoint;
  if (const auto problem = imapProblem(profile, request, &resolvedEndpoint)) {
    observation.gaps.push_back({"profile", *problem, {}});
    return observation;
  }
  // Identity fields mirror the real path: a refused read still names the
  // mailbox it would have covered.
  observation.serverAuthority = resolvedEndpoint->authority;
  observation.accountScope    = profile.accountScope;
  observation.mailbox         = profile.mailbox;
  // The read path needs both halves: etpan for the session, gmime to
  // parse what it fetches -- the Makefile lights GLEDITOR_HAVE_MAIL only
  // when both probe, so reaching here names exactly that absence.
  observation.gaps.push_back(
      {"transport",
       "this build has no IMAP transport -- rebuild with GLEDITOR_MAIL=1 "
       "(the Makefile probes pkg-config libetpan and gmime-3.0)",
       {}});
  return observation;
}

#endif // GLEDITOR_HAVE_MAIL

std::string mailDraftFingerprint(const SerializedMailDraft &draft) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  EVP_MD_CTX *raw = EVP_MD_CTX_new();
  if (!raw) throw std::runtime_error("mail draft fingerprint unavailable");
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      raw, EVP_MD_CTX_free);
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), draft.bytes.data(), draft.bytes.size()) !=
          1 ||
      EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    throw std::runtime_error("mail draft fingerprint unavailable");
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    result.push_back(digits[digest[i] >> 4]);
    result.push_back(digits[digest[i] & 0x0f]);
  }
  return result;
}

} // namespace xanadu::evidence
