#include "common/xanadu/evidence/irc_transport.hpp"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <arpa/inet.h>

#include <poll.h>
#include <sys/socket.h>

namespace xanadu::evidence {
namespace {
struct Socket {
  std::atomic_int fd{-1};
  void shutdown() const {
    const auto active = fd.load(std::memory_order_relaxed);
    if (active >= 0) static_cast<void>(::shutdown(active, SHUT_RDWR));
  }
};

void initializeCurl() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("IRC transport initialization failed");
  });
}

bool ready(int fd, short events, auto deadline,
           const std::atomic_bool &cancel) {
  while (!cancel.load()) {
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left <= std::chrono::milliseconds::zero()) return false;
    pollfd p{fd, events, 0};
    int r = ::poll(&p, 1,
                   static_cast<int>(std::min<std::int64_t>(left.count(), 100)));
    if (r > 0) return (p.revents & (events | POLLERR | POLLHUP)) != 0;
    if (r < 0 && errno != EINTR) return false;
  }
  return false;
}

std::vector<std::string> words(std::string_view text, char delimiter) {
  std::vector<std::string> result;
  while (true) {
    auto at = text.find(delimiter);
    result.emplace_back(text.substr(0, at));
    if (at == std::string_view::npos) return result;
    text.remove_prefix(at + 1);
  }
}

struct Line {
  std::map<std::string, std::string> tags;
  std::string prefix;
  std::string command;
  std::vector<std::string> args;
};

bool singleIrcToken(std::string_view value) {
  return !value.empty() &&
         value.find_first_of(" \t\r\n") == std::string_view::npos &&
         value.find('\0') == std::string_view::npos;
}

bool hasLineBreak(std::string_view value) {
  return value.find_first_of("\r\n") != std::string_view::npos ||
         value.find('\0') != std::string_view::npos;
}

bool validAccountScope(std::string_view value, bool allowEmpty = false) {
  return value.size() <= 256 && !hasLineBreak(value) &&
         (allowEmpty || !value.empty());
}

bool contains(std::string_view text, std::string_view witness) {
  return !witness.empty() && text.find(witness) != std::string_view::npos;
}

std::string canonicalAuthority(const IrcConnectionConfig &config) {
  const bool ipv6  = config.host.find(':') != std::string::npos;
  std::string host = config.host;
  if (!ipv6)
    std::ranges::transform(host, host.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
  if (ipv6) host = "[" + host + "]";
  return std::string(config.useTls ? "ircs://" : "irc://") + host + ":" +
         std::to_string(config.port);
}

bool validHost(std::string_view host) {
  in_addr ipv4{};
  in6_addr ipv6{};
  if (inet_pton(AF_INET, std::string(host).c_str(), &ipv4) == 1 ||
      inet_pton(AF_INET6, std::string(host).c_str(), &ipv6) == 1)
    return true;
  if (host.empty() || host.size() > 253 || host.front() == '.' ||
      host.back() == '.')
    return false;
  std::size_t labelLength{};
  bool labelStartsHyphen{};
  for (std::size_t index = 0; index < host.size(); ++index) {
    const char value = host[index];
    if (value == '.') {
      if (!labelLength || labelStartsHyphen || host[index - 1] == '-')
        return false;
      labelLength       = 0;
      labelStartsHyphen = false;
      continue;
    }
    const auto ch                = static_cast<unsigned char>(value);
    const bool asciiAlphaNumeric = (ch >= 'a' && ch <= 'z') ||
                                   (ch >= 'A' && ch <= 'Z') ||
                                   (ch >= '0' && ch <= '9');
    if (!asciiAlphaNumeric && value != '-') return false;
    if (!labelLength) labelStartsHyphen = value == '-';
    if (++labelLength > 63) return false;
  }
  return labelLength && !labelStartsHyphen && host.back() != '-';
}

std::optional<std::string> canonicalAuthorityInput(std::string_view authority) {
  const std::string original(authority);
  const bool tls = authority.starts_with("ircs://");
  const auto prefix =
      tls ? std::string_view("ircs://") : std::string_view("irc://");
  if (!authority.starts_with(prefix) || authority.size() > 512) return {};
  authority.remove_prefix(prefix.size());
  std::string_view host;
  std::string_view portText;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close + 1 >= authority.size() ||
        authority[close + 1] != ':')
      return {};
    host     = authority.substr(1, close - 1);
    portText = authority.substr(close + 2);
  } else {
    const auto colon = authority.rfind(':');
    if (colon == std::string_view::npos) return {};
    host     = authority.substr(0, colon);
    portText = authority.substr(colon + 1);
  }
  unsigned port{};
  const auto parsed =
      std::from_chars(portText.data(), portText.data() + portText.size(), port);
  if (!validHost(host) || parsed.ec != std::errc{} ||
      parsed.ptr != portText.data() + portText.size() || !port ||
      port > std::numeric_limits<std::uint16_t>::max())
    return {};
  IrcConnectionConfig config;
  config.host    = host;
  config.port    = static_cast<std::uint16_t>(port);
  config.useTls  = tls;
  auto canonical = canonicalAuthority(config);
  if (canonical != original) return {};
  return canonical;
}

std::optional<std::string>
connectionProblem(const IrcConnectionConfig &config) {
  if (!validHost(config.host) || !config.port || !singleIrcToken(config.nick) ||
      (!config.username.empty() && !singleIrcToken(config.username)) ||
      hasLineBreak(config.realName) || config.host.size() > 253 ||
      config.serverAuthority.size() > 512 ||
      config.networkIncarnation.size() > 256 ||
      !validAccountScope(config.accountScope, true) ||
      config.caBundlePath.size() > 4096 || config.maxLineBytes < 64 ||
      config.maxLineBytes > 65536 || config.maxTagBytes > config.maxLineBytes ||
      !config.maxBatchDepth || config.maxBatchDepth > 64 || !config.maxEvents ||
      config.maxEvents > 16384 || !config.maxReadTargets ||
      config.maxReadTargets > 4096 || !config.maxPendingReads ||
      config.maxPendingReads > 1024 || !config.maxPendingRecords ||
      config.maxPendingRecords > 65536 || !config.maxPendingBytes ||
      config.maxPendingBytes > 64 * 1024 * 1024 ||
      config.timeout <= std::chrono::milliseconds::zero() ||
      config.timeout > std::chrono::minutes(5))
    return "invalid IRC connection configuration";
  if (std::any_of(
          config.requestedCapabilities.begin(),
          config.requestedCapabilities.end(),
          [](const auto &capability) { return !singleIrcToken(capability); }))
    return "invalid IRC capability token";
  const auto authority = canonicalAuthority(config);
  if (!config.serverAuthority.empty() && config.serverAuthority != authority)
    return "IRC server authority must name the configured endpoint";
  return {};
}

class SigpipeBlock {
public:
  SigpipeBlock() {
    sigemptyset(&set_);
    sigaddset(&set_, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &set_, &previous_) == 0) {
      active_ = true;
      sigset_t pending{};
      if (sigpending(&pending) == 0)
        wasPending_ = sigismember(&pending, SIGPIPE) == 1;
    }
  }
  ~SigpipeBlock() {
    if (!active_) return;
    if (!wasPending_) {
      timespec noWait{};
      while (sigtimedwait(&set_, nullptr, &noWait) == SIGPIPE) {
      }
    }
    static_cast<void>(pthread_sigmask(SIG_SETMASK, &previous_, nullptr));
  }

private:
  sigset_t set_{};
  sigset_t previous_{};
  bool active_{};
  bool wasPending_{};
};

std::string connectionNonce() {
  std::array<unsigned char, 16> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("IRC connection nonce generation failed");
  constexpr char digits[] = "0123456789abcdef";
  std::string nonce;
  nonce.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    nonce.push_back(digits[byte >> 4]);
    nonce.push_back(digits[byte & 0x0f]);
  }
  return nonce;
}

bool isChannelTarget(std::string_view target) {
  return !target.empty() && (target.front() == '#' || target.front() == '&' ||
                             target.front() == '+' || target.front() == '!');
}

bool sameNick(std::string_view lhs, std::string_view rhs) {
  if (lhs.size() != rhs.size()) return false;
  return std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                    [](char left, char right) {
                      return std::tolower(static_cast<unsigned char>(left)) ==
                             std::tolower(static_cast<unsigned char>(right));
                    });
}

std::string foldedTarget(std::string_view target) {
  std::string result(target);
  std::ranges::transform(result, result.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return result;
}

bool messageMatchesTarget(const IrcMessageEvidence &message,
                          std::string_view target, std::string_view localNick) {
  if (message.command != "PRIVMSG" && message.command != "NOTICE") return false;
  if (sameNick(message.target, target)) return true;
  if (!sameNick(message.target, localNick)) return false;
  const auto bang = message.sender.find('!');
  return sameNick(std::string_view(message.sender).substr(0, bang), target);
}

struct CleanseString {
  std::string &value;
  ~CleanseString() { OPENSSL_cleanse(value.data(), value.size()); }
};

struct IrcSecretState {
  std::mutex mutex;
  std::string password;
  std::shared_ptr<std::atomic_bool> revoked =
      std::make_shared<std::atomic_bool>(false);
  std::chrono::steady_clock::time_point expiresAt;
  bool consumed{};
  ~IrcSecretState() { OPENSSL_cleanse(password.data(), password.size()); }
};

Line parse(std::string_view text, std::size_t tagLimit) {
  Line out;
  if (!text.empty() && text.front() == '@') {
    auto end = text.find(' ');
    if (end == std::string_view::npos || end > tagLimit)
      throw std::runtime_error("IRC tag byte limit exceeded");
    for (const auto &tag : words(text.substr(1, end - 1), ';')) {
      auto eq = tag.find('=');
      out.tags[tag.substr(0, eq)] =
          eq == std::string::npos ? "" : tag.substr(eq + 1);
    }
    text.remove_prefix(end + 1);
  }
  if (!text.empty() && text.front() == ':') {
    auto end = text.find(' ');
    if (end == std::string_view::npos)
      throw std::runtime_error("invalid IRC prefix");
    out.prefix = text.substr(1, end - 1);
    text.remove_prefix(end + 1);
  }
  while (!text.empty()) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    if (text.empty()) break;
    if (text.front() == ':') {
      out.args.emplace_back(text.substr(1));
      break;
    }
    auto end   = text.find(' ');
    auto token = text.substr(0, end);
    if (out.command.empty())
      out.command = token;
    else
      out.args.emplace_back(token);
    if (end == std::string_view::npos) break;
    text.remove_prefix(end + 1);
  }
  return out;
}

std::string decodeTag(std::string_view value) {
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] != '\\' || i + 1 == value.size()) {
      out += value[i];
      continue;
    }
    switch (value[++i]) {
    case ':':
      out += ';';
      break;
    case 's':
      out += ' ';
      break;
    case 'r':
      out += '\r';
      break;
    case 'n':
      out += '\n';
      break;
    case '\\':
      out += '\\';
      break;
    default:
      break;
    }
  }
  return out;
}
} // namespace

std::optional<std::string>
ircConnectionProblem(const IrcConnectionConfig &config) {
  return connectionProblem(config);
}

std::optional<std::string>
ircServerAuthority(const IrcConnectionConfig &config) {
  if (connectionProblem(config)) return {};
  return canonicalAuthority(config);
}

std::optional<IrcCredentialLease> IrcCredentialBroker::issue(
    std::string serverAuthority, std::string accountScope,
    std::string saslUsername, std::string password,
    const std::chrono::steady_clock::duration lifetime) const {
  struct InputWiper {
    std::string &username;
    std::string &password;
    ~InputWiper() {
      OPENSSL_cleanse(username.data(), username.size());
      OPENSSL_cleanse(password.data(), password.size());
    }
  } wiper{saslUsername, password};
  const auto canonical = canonicalAuthorityInput(serverAuthority);
  if (!canonical || !validAccountScope(accountScope) || saslUsername.empty() ||
      saslUsername.size() > 4096 ||
      saslUsername.find('\0') != std::string::npos || password.empty() ||
      password.size() > 65536 || password.find('\0') != std::string::npos ||
      lifetime <= std::chrono::steady_clock::duration::zero() ||
      lifetime > std::chrono::hours(24))
    return {};
  auto state       = std::make_shared<IrcSecretState>();
  state->password  = std::move(password);
  state->expiresAt = std::chrono::steady_clock::now() + lifetime;
  IrcCredentialLease lease;
  lease.serverAuthority = *canonical;
  lease.accountScope    = std::move(accountScope);
  lease.saslUsername    = std::move(saslUsername);
  lease.expiresAt       = state->expiresAt;
  lease.revoked         = state->revoked;
  lease.takeSecret      = [state]() -> std::optional<std::string> {
    const std::lock_guard lock(state->mutex);
    if (state->consumed || state->revoked->load() ||
        std::chrono::steady_clock::now() >= state->expiresAt)
      return {};
    state->consumed    = true;
    std::string result = std::move(state->password);
    OPENSSL_cleanse(state->password.data(), state->password.size());
    state->password.clear();
    return result;
  };
  lease.revokeSecret = [state] {
    state->revoked->store(true);
    const std::lock_guard lock(state->mutex);
    state->consumed = true;
    OPENSSL_cleanse(state->password.data(), state->password.size());
    state->password.clear();
  };
  return lease;
}

void IrcCredentialBroker::revoke(
    const IrcCredentialLease &lease) const noexcept {
  if (lease.revoked) lease.revoked->store(true);
  try {
    if (lease.revokeSecret) lease.revokeSecret();
  } catch (...) {
  }
}

struct IrcSession::Impl {
  enum class PendingKind { Live, History };
  struct PendingRead {
    std::string ticket;
    std::string key;
    PendingKind kind{PendingKind::Live};
    std::size_t cursorEnd{};
    std::size_t records{};
    std::size_t bytes{};
    std::shared_ptr<const IrcTransportResult> snapshot;
  };

  explicit Impl(IrcConnectionConfig c) : config(std::move(c)) {
    if (const auto problem = connectionProblem(config))
      throw std::invalid_argument(*problem);
    const auto endpointAuthority = canonicalAuthority(config);
    if (!config.serverAuthority.empty() &&
        config.serverAuthority != endpointAuthority)
      throw std::invalid_argument(
          "IRC server authority must name the configured endpoint");
    config.serverAuthority            = endpointAuthority;
    observation.serverAuthority       = endpointAuthority;
    observation.networkIncarnation    = config.networkIncarnation;
    observation.connectionIncarnation = connectionNonce();
    observation.state                 = CaptureState::Partial;
    observation.bounds.maxRecords     = config.maxEvents;
    observation.bounds.maxStringBytes = config.maxLineBytes;
    if (config.sasl) {
      const auto &lease = *config.sasl;
      if (!config.useTls || lease.serverAuthority != endpointAuthority ||
          config.accountScope.empty() ||
          lease.accountScope != config.accountScope || !lease.revoked ||
          lease.revoked->load() ||
          std::chrono::steady_clock::now() >= lease.expiresAt ||
          lease.saslUsername.empty() || lease.saslUsername.size() > 4096 ||
          lease.saslUsername.find('\0') != std::string::npos ||
          !lease.takeSecret || !lease.revokeSecret)
        throw std::invalid_argument(
            "IRC SASL lease must match a live TLS profile and account scope");
    }
    if (config.sasl && std::find(config.requestedCapabilities.begin(),
                                 config.requestedCapabilities.end(),
                                 "sasl") == config.requestedCapabilities.end())
      config.requestedCapabilities.emplace_back("sasl");
  }
  IrcConnectionConfig config;
  Socket socket;
  CURL *curl{};
  const std::atomic_bool *activeCancel{};
  std::atomic_bool cancelRequested{};
  std::timed_mutex operationGate;
  mutable std::mutex pendingMutex;
  std::chrono::steady_clock::time_point connectDeadline;
  std::string input;
  std::string credentialEchoPassword;
  std::string credentialEchoPlain;
  IrcObservation observation;
  std::map<std::string, std::string> batches;
  std::vector<std::string> joinedChannels;
  std::map<std::string, std::size_t> readCursors;
  std::map<std::string, std::string> pendingLiveByTarget;
  std::map<std::string, std::string> pendingHistoryByRequest;
  std::map<std::string, PendingRead> pendingReads;
  std::size_t pendingRecordCount{};
  std::size_t pendingByteCount{};
  std::optional<std::string> pendingJoin;
  std::size_t retainedBytes{};
  bool eventBudgetGapRecorded{};
  bool connected{};
  bool connectStarted{};
  bool echo{};
  bool historyCap{};
  bool capLsSeen{};
  bool capReplySeen{};
  std::vector<std::string> advertisedCapabilities;
  bool saslInProgress{};
  bool saslComplete{};
  bool saslRejected{};
  std::string saslMechanisms;
  std::vector<std::string> requestedCapsQueue;
  std::size_t capQueuePosition{};
  bool historyBatchSeen{};
  bool historyBatchClosed{};
  bool wantEcho{};
  std::string expectedEchoTarget;
  std::string expectedEchoPayload;
  std::optional<std::string> echoId;

  ~Impl() {
    OPENSSL_cleanse(credentialEchoPassword.data(),
                    credentialEchoPassword.size());
    OPENSSL_cleanse(credentialEchoPlain.data(), credentialEchoPlain.size());
    if (curl) {
      SigpipeBlock block;
      curl_easy_cleanup(curl);
    }
  }
  auto deadline() const {
    return std::chrono::steady_clock::now() + config.timeout;
  }

  std::unique_lock<std::timed_mutex>
  lockOperation(auto end, const std::atomic_bool &cancel) {
    std::unique_lock<std::timed_mutex> lock(operationGate, std::defer_lock);
    while (!cancel.load() && !cancelRequested.load() &&
           std::chrono::steady_clock::now() < end) {
      if (lock.try_lock_for(std::chrono::milliseconds(25))) return lock;
    }
    return lock;
  }

  IrcTransportResult deltaResult(
      std::size_t messageStart, std::size_t gapStart,
      std::optional<std::vector<IrcMessageEvidence>> selected = {}) const {
    auto snapshot = observation;
    if (selected)
      snapshot.messages = std::move(*selected);
    else if (messageStart <= snapshot.messages.size())
      snapshot.messages.erase(snapshot.messages.begin(),
                              snapshot.messages.begin() +
                                  static_cast<std::ptrdiff_t>(messageStart));
    if (gapStart <= snapshot.gaps.size())
      snapshot.gaps.erase(snapshot.gaps.begin(),
                          snapshot.gaps.begin() +
                              static_cast<std::ptrdiff_t>(gapStart));
    return {std::move(snapshot), connected, {}};
  }

  IrcTransportResult waitingResult(std::string reason,
                                   CaptureState state) const {
    IrcObservation snapshot;
    snapshot.serverAuthority       = config.serverAuthority;
    snapshot.networkIncarnation    = config.networkIncarnation;
    snapshot.connectionIncarnation = observation.connectionIncarnation;
    snapshot.bounds                = observation.bounds;
    snapshot.state                 = state;
    snapshot.gaps.push_back({{}, {}, std::move(reason)});
    return {std::move(snapshot), false, {}};
  }

  std::shared_ptr<const IrcTransportResult>
  pendingSnapshot(PendingKind kind, std::string_view key) const {
    const std::lock_guard lock(pendingMutex);
    const auto &index = kind == PendingKind::Live ? pendingLiveByTarget
                                                  : pendingHistoryByRequest;
    const auto found  = index.find(std::string(key));
    if (found == index.end()) return {};
    const auto pending = pendingReads.find(found->second);
    if (pending == pendingReads.end()) return {};
    return pending->second.snapshot;
  }

  static std::size_t addSize(std::size_t total, std::size_t addition) {
    return addition > std::numeric_limits<std::size_t>::max() - total
               ? std::numeric_limits<std::size_t>::max()
               : total + addition;
  }

  static std::pair<std::size_t, std::size_t>
  pendingSize(const IrcTransportResult &result) {
    const auto &observation = result.observation;
    std::size_t bytes{};
    std::size_t records =
        addSize(observation.messages.size(), observation.gaps.size());
    auto add = [&](std::string_view value) {
      bytes = addSize(bytes, value.size());
    };
    add(observation.serverAuthority);
    add(observation.networkIncarnation);
    add(observation.connectionIncarnation);
    for (const auto &capability : observation.capabilities) add(capability);
    for (const auto &message : observation.messages) {
      add(message.target);
      add(message.sender);
      add(message.command);
      add(message.text);
      add(message.serverTime);
      if (message.messageId) add(*message.messageId);
      for (const auto &[key, value] : message.tags) {
        add(key);
        add(value);
      }
    }
    for (const auto &gap : observation.gaps) {
      add(gap.fromCursor);
      add(gap.toCursor);
      add(gap.reason);
    }
    return {records, bytes};
  }

  bool storePending(PendingRead pending, IrcTransportResult result,
                    IrcTransportResult &returned) {
    const auto [records, resultBytes] = pendingSize(result);
    const auto bytes = addSize(addSize(resultBytes, pending.key.size()), 32);
    if (records > config.maxPendingRecords || bytes > config.maxPendingBytes) {
      observation.state = CaptureState::Partial;
      observation.gaps.push_back(
          {{}, {}, "IRC pending read exceeds configured ticket budget"});
      return false;
    }
    std::unique_lock lock(pendingMutex);
    if (pendingReads.size() >=
            std::min(config.maxPendingReads, config.maxReadTargets) ||
        records > config.maxPendingRecords - pendingRecordCount ||
        bytes > config.maxPendingBytes - pendingByteCount) {
      observation.state = CaptureState::Partial;
      observation.gaps.push_back(
          {{}, {}, "unacknowledged IRC read ticket budget exceeded"});
      return false;
    }
    do {
      pending.ticket = connectionNonce();
    } while (pendingReads.contains(pending.ticket));
    const auto ticket = pending.ticket;
    const auto key    = pending.key;
    pending.records   = records;
    pending.bytes     = bytes;
    result.readTicket = ticket;
    pending.snapshot =
        std::make_shared<const IrcTransportResult>(std::move(result));
    const auto snapshot = pending.snapshot;
    const auto [entry, inserted] =
        pendingReads.emplace(ticket, std::move(pending));
    if (!inserted) return false;
    if (entry->second.kind == PendingKind::Live)
      pendingLiveByTarget[key] = ticket;
    else
      pendingHistoryByRequest[key] = ticket;
    pendingRecordCount += records;
    pendingByteCount += bytes;
    lock.unlock();
    returned = *snapshot;
    return true;
  }

  bool joined(std::string_view target) const {
    return std::any_of(
        joinedChannels.begin(), joinedChannels.end(),
        [target](const auto &channel) { return sameNick(channel, target); });
  }

  void addJoined(std::string target) {
    if (!joined(target)) joinedChannels.push_back(std::move(target));
  }

  void removeJoined(std::string_view target) {
    std::erase_if(joinedChannels, [target](const auto &channel) {
      return sameNick(channel, target);
    });
  }

  bool requestNextCapability(auto end, const std::atomic_bool &cancel) {
    if (capQueuePosition >= requestedCapsQueue.size()) {
      capReplySeen = true;
      return true;
    }
    capReplySeen = false;
    return write("CAP REQ :" + requestedCapsQueue[capQueuePosition] + "\r\n",
                 end, cancel);
  }

  void open(const std::atomic_bool &cancel) {
    initializeCurl();
    const auto *version = curl_version_info(CURLVERSION_NOW);
    if (!version || !(version->features & CURL_VERSION_ASYNCHDNS))
      throw std::runtime_error("bounded IRC DNS resolution is unavailable");
    curl = curl_easy_init();
    if (!curl) throw std::runtime_error("IRC transport allocation failed");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(),
                                                            curl_url_cleanup);
    const std::string scheme = config.useTls ? "https" : "http";
    const auto port          = std::to_string(config.port);
    if (!url ||
        curl_url_set(url.get(), CURLUPART_SCHEME, scheme.c_str(), 0) !=
            CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_HOST, config.host.c_str(), 0) !=
            CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_PORT, port.c_str(), 0) != CURLUE_OK ||
        curl_url_set(url.get(), CURLUPART_PATH, "/", 0) != CURLUE_OK)
      throw std::runtime_error("IRC endpoint URL is invalid");
    char *urlData{};
    if (curl_url_get(url.get(), CURLUPART_URL, &urlData, 0) != CURLUE_OK)
      throw std::runtime_error("IRC endpoint URL is unavailable");
    const std::string endpoint(urlData);
    curl_free(urlData);
    activeCancel         = &cancel;
    const auto timeoutMs = static_cast<long>(config.timeout.count());
    curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!config.caBundlePath.empty())
      curl_easy_setopt(curl, CURLOPT_CAINFO, config.caBundlePath.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_ENABLE_ALPN, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    CURLcode result{};
    {
      SigpipeBlock block;
      result = curl_easy_perform(curl);
    }
    activeCancel = nullptr;
    if (result != CURLE_OK || cancel.load() || cancelRequested.load())
      throw std::runtime_error("IRC connection or TLS verification failed");
    curl_socket_t active{};
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &active) != CURLE_OK ||
        active == CURL_SOCKET_BAD)
      throw std::runtime_error("IRC active socket unavailable");
    socket.fd.store(static_cast<int>(active), std::memory_order_relaxed);
  }

  static int transferProgress(void *context, curl_off_t, curl_off_t, curl_off_t,
                              curl_off_t) noexcept {
    const auto &self = *static_cast<Impl *>(context);
    return self.cancelRequested.load(std::memory_order_relaxed) ||
                   (self.activeCancel && self.activeCancel->load()) ||
                   std::chrono::steady_clock::now() >= self.connectDeadline
               ? 1
               : 0;
  }

  bool write(std::string_view data, auto end, const std::atomic_bool &cancel,
             std::size_t *written = nullptr) {
    std::size_t done = 0;
    while (done < data.size() && !cancel.load()) {
      const auto fd = socket.fd.load(std::memory_order_relaxed);
      if (fd < 0 || cancelRequested.load() ||
          !ready(fd, POLLOUT, end, cancel)) {
        if (written) *written = done;
        return false;
      }
      std::size_t n{};
      CURLcode result{};
      {
        SigpipeBlock block;
        result =
            curl_easy_send(curl, data.data() + done, data.size() - done, &n);
      }
      if (result == CURLE_AGAIN) continue;
      if (result != CURLE_OK || n == 0) {
        if (written) *written = done;
        return false;
      }
      done += n;
    }
    if (written) *written = done;
    return done == data.size();
  }
  bool fill(auto end, const std::atomic_bool &cancel) {
    const auto fd = socket.fd.load(std::memory_order_relaxed);
    std::array<char, 2048> buffer{};
    while (fd >= 0 && !cancelRequested.load() && !cancel.load()) {
      std::size_t n{};
      CURLcode result{};
      {
        SigpipeBlock block;
        result = curl_easy_recv(curl, buffer.data(), buffer.size(), &n);
      }
      if (result == CURLE_OK && n) {
        input.append(buffer.data(), n);
        return true;
      }
      if (result != CURLE_AGAIN || !ready(fd, POLLIN, end, cancel))
        return false;
    }
    return false;
  }

  bool credentialEcho(std::string_view raw, const Line &message) const {
    if (contains(raw, credentialEchoPassword) ||
        contains(raw, credentialEchoPlain))
      return true;
    auto matches = [&](std::string_view value) {
      return contains(value, credentialEchoPassword) ||
             contains(value, credentialEchoPlain);
    };
    if (matches(message.prefix) || matches(message.command)) return true;
    for (const auto &argument : message.args)
      if (matches(argument)) return true;
    for (const auto &[key, value] : message.tags)
      if (matches(key) || matches(value) || matches(decodeTag(value)))
        return true;
    return false;
  }
  bool line(std::string &out, auto end, const std::atomic_bool &cancel) {
    for (;;) {
      auto at = input.find("\r\n");
      if (at != std::string::npos) {
        if (at > config.maxLineBytes)
          throw std::runtime_error("IRC line byte limit exceeded");
        out = input.substr(0, at);
        input.erase(0, at + 2);
        return true;
      }
      if (input.size() > config.maxLineBytes)
        throw std::runtime_error("IRC line byte limit exceeded");
      if (!fill(end, cancel)) return false;
    }
  }
  bool readOne(auto end, const std::atomic_bool &cancel,
               std::optional<std::string> *serverError = nullptr) {
    std::string raw;
    if (!line(raw, end, cancel)) return false;
    auto p = parse(raw, config.maxTagBytes);
    if (credentialEcho(raw, p)) {
      observation.state = CaptureState::Partial;
      constexpr std::string_view reason =
          "IRC message withheld because it echoed a credential";
      observation.gaps.push_back({{}, {}, std::string{reason}});
      if (serverError) *serverError = reason;
      return true;
    }
    if (p.command == "PING" && !p.args.empty())
      return write("PONG :" + p.args.back() + "\r\n", end, cancel);
    if (p.command == "CAP" && p.args.size() >= 2) {
      const auto &verb = p.args[1];
      if (verb == "LS") {
        capLsSeen = true;
        for (const auto &cap : words(p.args.back(), ' ')) {
          auto eq   = cap.find('=');
          auto name = cap.substr(0, eq);
          advertisedCapabilities.push_back(name);
          if (name == "sasl" && eq != std::string::npos)
            saslMechanisms = cap.substr(eq + 1);
        }
        const bool continuation = p.args.size() > 2 && p.args[2] == "*";
        if (!continuation) {
          for (const auto &name : advertisedCapabilities) {
            if (std::find(config.requestedCapabilities.begin(),
                          config.requestedCapabilities.end(),
                          name) == config.requestedCapabilities.end())
              continue;
            if (std::find(requestedCapsQueue.begin(), requestedCapsQueue.end(),
                          name) == requestedCapsQueue.end())
              requestedCapsQueue.push_back(name);
          }
          if (config.sasl && std::find(advertisedCapabilities.begin(),
                                       advertisedCapabilities.end(),
                                       "sasl") == advertisedCapabilities.end())
            saslRejected = true;
          if (!requestNextCapability(end, cancel)) return false;
        }
      } else if (verb == "ACK") {
        bool saslAcked      = false;
        bool expectedAcked  = false;
        const auto expected = capQueuePosition < requestedCapsQueue.size()
                                  ? requestedCapsQueue[capQueuePosition]
                                  : std::string{};
        for (const auto &cap : words(p.args.back(), ' ')) {
          auto name = cap.substr(0, cap.find('='));
          if (name != expected || (!name.empty() && name.front() == '-'))
            continue;
          expectedAcked = true;
          observation.capabilities.push_back(name);
          if (name == "echo-message") echo = true;
          if (name == "draft/chathistory") historyCap = true;
          if (name == "sasl" && config.sasl) {
            const auto mechanisms = words(saslMechanisms, ',');
            if (std::find(mechanisms.begin(), mechanisms.end(), "PLAIN") ==
                mechanisms.end())
              throw std::runtime_error("IRC server did not offer SASL PLAIN");
            const auto &lease = *config.sasl;
            if (!config.useTls || !lease.revoked || lease.revoked->load() ||
                std::chrono::steady_clock::now() >= lease.expiresAt ||
                lease.serverAuthority != observation.serverAuthority ||
                lease.accountScope != config.accountScope || !lease.takeSecret)
              throw std::runtime_error("IRC SASL credential lease unavailable");
            if (!write("AUTHENTICATE PLAIN\r\n", end, cancel)) return false;
            saslInProgress = true;
            saslAcked      = true;
          }
        }
        if (!expectedAcked)
          observation.gaps.push_back(
              {{}, {}, "server ACK did not match requested IRC capability"});
        if (!saslAcked) {
          ++capQueuePosition;
          if (!requestNextCapability(end, cancel)) return false;
        }
      } else if (verb == "NAK") {
        if (capQueuePosition < requestedCapsQueue.size() &&
            requestedCapsQueue[capQueuePosition] == "sasl")
          saslRejected = true;
        observation.gaps.push_back(
            {{}, {}, "server rejected requested IRC capability"});
        ++capQueuePosition;
        if (!requestNextCapability(end, cancel)) return false;
      }
    }
    if (serverError && p.command.size() == 3 && p.command[0] >= '4' &&
        p.command[0] <= '5')
      *serverError = raw;
    if (saslInProgress && p.command == "AUTHENTICATE" && !p.args.empty() &&
        p.args[0] == "+") {
      const auto &lease = *config.sasl;
      if (lease.revoked->load() ||
          std::chrono::steady_clock::now() >= lease.expiresAt)
        throw std::runtime_error("IRC SASL credential lease expired");
      auto secret = lease.takeSecret();
      if (!secret)
        throw std::runtime_error("IRC SASL credential lease is empty");
      CleanseString secretWiper{*secret};
      const auto maxPlainBytes = ((config.maxLineBytes - 15) / 4) * 3;
      if (lease.saslUsername.size() + secret->size() + 2 > maxPlainBytes)
        throw std::runtime_error("IRC SASL credential exceeds byte limit");
      std::string plain;
      CleanseString plainWiper{plain};
      plain.reserve(lease.saslUsername.size() + secret->size() + 2);
      plain.push_back('\0');
      plain += lease.saslUsername;
      plain.push_back('\0');
      plain += *secret;
      std::string encoded(4 * ((plain.size() + 2) / 3), '\0');
      CleanseString encodedWiper{encoded};
      int encodedBytes =
          EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                          reinterpret_cast<const unsigned char *>(plain.data()),
                          static_cast<int>(plain.size()));
      encoded.resize(static_cast<std::size_t>(encodedBytes));
      if (encoded.size() + 15 > config.maxLineBytes)
        throw std::runtime_error("IRC SASL credential exceeds byte limit");
      std::string authLine = "AUTHENTICATE " + encoded + "\r\n";
      CleanseString authWiper{authLine};
      credentialEchoPassword = *secret;
      credentialEchoPlain    = encoded;
      if (lease.revoked->load() ||
          std::chrono::steady_clock::now() >= lease.expiresAt)
        throw std::runtime_error(
            "IRC SASL credential lease revoked before write");
      bool sent = write(authLine, end, cancel);
      if (!sent) throw std::runtime_error("IRC SASL exchange failed");
      saslInProgress = false;
    }
    if (p.command == "903") {
      saslComplete   = true;
      saslInProgress = false;
      ++capQueuePosition;
      if (!requestNextCapability(end, cancel)) return false;
    }
    if (p.command == "904" || p.command == "905" || p.command == "906" ||
        p.command == "907")
      throw std::runtime_error("IRC SASL authentication rejected");
    if (p.command == "BATCH" && !p.args.empty()) {
      auto id = p.args[0];
      if (!id.empty() && id[0] == '+') {
        if (batches.size() >= config.maxBatchDepth)
          throw std::runtime_error("IRC BATCH depth exceeded");
        const auto type       = p.args.size() > 1 ? p.args[1] : "";
        batches[id.substr(1)] = type;
        if (type == "chathistory" || type == "draft/chathistory")
          historyBatchSeen = true;
      } else if (!id.empty() && id[0] == '-') {
        auto found = batches.find(id.substr(1));
        if (found != batches.end() && (found->second == "chathistory" ||
                                       found->second == "draft/chathistory"))
          historyBatchClosed = true;
        batches.erase(id.substr(1));
      }
      return true;
    }
    if (p.command == "001") connected = true;
    const auto prefixBang          = p.prefix.find('!');
    const auto senderNick          = p.prefix.substr(0, prefixBang);
    const bool selfSender          = sameNick(senderNick, config.nick);
    const bool confirmsPendingJoin = p.command == "JOIN" && selfSender &&
                                     !p.args.empty() && pendingJoin &&
                                     sameNick(p.args.front(), *pendingJoin);
    if (confirmsPendingJoin) addJoined(p.args.front());
    // IRC echo-message has no universal request id, so match both fields and
    // report an observed echo without claiming recipient delivery.
    if (wantEcho && p.command == "PRIVMSG" && selfSender && p.args.size() > 1 &&
        sameNick(p.args.front(), expectedEchoTarget) &&
        p.args.back() == expectedEchoPayload) {
      echoId   = p.tags.contains("msgid") ? p.tags["msgid"] : "";
      wantEcho = false;
      return true;
    }
    if (p.command == "PRIVMSG" || p.command == "NOTICE" ||
        p.command == "JOIN" || p.command == "PART" || p.command == "TOPIC" ||
        p.command == "KICK") {
      const std::string_view target =
          p.args.empty() ? std::string_view{} : p.args.front();
      const bool isChannel = isChannelTarget(target);
      const bool selfPart  = p.command == "PART" && selfSender;
      const bool selfKick  = p.command == "KICK" && p.args.size() > 1 &&
                            sameNick(p.args[1], config.nick);
      const bool addressedToUs =
          (p.command == "PRIVMSG" || p.command == "NOTICE") &&
          sameNick(target, config.nick);
      const bool allowed = !isChannel || joined(target) ||
                           (p.command == "JOIN" && confirmsPendingJoin);
      std::size_t eventBytes = p.prefix.size();
      for (const auto &arg : p.args) eventBytes += arg.size();
      for (const auto &[key, value] : p.tags)
        eventBytes += key.size() + value.size();
      if (allowed && (isChannel || addressedToUs) &&
          observation.messages.size() < config.maxEvents &&
          retainedBytes <= observation.bounds.maxBytes &&
          eventBytes <= observation.bounds.maxBytes - retainedBytes) {
        IrcMessageEvidence m;
        m.localSequence = observation.messages.size() + 1;
        m.sender        = p.prefix;
        m.command       = p.command;
        if (!p.args.empty()) m.target = p.args.front();
        if (p.args.size() > 1) m.text = p.args.back();
        for (auto [key, value] : p.tags) {
          m.tags[key] = decodeTag(value);
          if (key == "msgid") m.messageId = value;
          if (key == "time") m.serverTime = value;
        }
        observation.messages.push_back(std::move(m));
        retainedBytes += eventBytes;
      } else if (allowed && (isChannel || addressedToUs)) {
        observation.state = CaptureState::Partial;
        if (!eventBudgetGapRecorded) {
          observation.gaps.push_back(
              {{}, {}, "IRC observation event or byte budget exceeded"});
          eventBudgetGapRecorded = true;
        }
      }
      if (selfPart || selfKick) removeJoined(target);
    }
    return true;
  }
  IrcTransportResult result(std::string detail = {}) const {
    return {observation, connected, std::move(detail)};
  }
};

IrcSession::IrcSession(IrcConnectionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
IrcSession::~IrcSession()                                 = default;
IrcSession::IrcSession(IrcSession &&) noexcept            = default;
IrcSession &IrcSession::operator=(IrcSession &&) noexcept = default;

IrcTransportResult IrcSession::connect(const std::atomic_bool &cancel) {
  const auto end = impl_->deadline();
  auto operation = impl_->lockOperation(end, cancel);
  if (!operation.owns_lock()) {
    return impl_->waitingResult(
        "IRC connect cancelled while waiting for session",
        cancel.load() ? CaptureState::Cancelled : CaptureState::Partial);
  }
  if (impl_->connectStarted) {
    auto repeated      = impl_->result("IRC session connection is one-shot");
    repeated.connected = false;
    repeated.observation.state = CaptureState::Failed;
    repeated.observation.gaps.push_back(
        {{}, {}, "repeated connect refused for existing incarnation"});
    return repeated;
  }
  impl_->connectStarted = true;
  const auto gapStart   = impl_->observation.gaps.size();
  try {
    impl_->connectDeadline = end;
    impl_->open(cancel);
    if (!impl_->write("CAP LS 302\r\n", end, cancel))
      throw std::runtime_error("IRC CAP request failed");
    bool registered = false;
    while (!cancel.load() && std::chrono::steady_clock::now() < end &&
           !impl_->connected) {
      if (!impl_->readOne(end, cancel)) break;
      if (impl_->config.sasl && impl_->saslRejected)
        throw std::runtime_error(
            "IRC server rejected requested SASL capability");
      if (!registered && impl_->capLsSeen && impl_->capReplySeen &&
          (!impl_->config.sasl || impl_->saslComplete)) {
        if (!impl_->write(
                "CAP END\r\nNICK " + impl_->config.nick + "\r\nUSER " +
                    (impl_->config.username.empty() ? impl_->config.nick
                                                    : impl_->config.username) +
                    " 0 * :" +
                    (impl_->config.realName.empty() ? impl_->config.nick
                                                    : impl_->config.realName) +
                    "\r\n",
                end, cancel))
          throw std::runtime_error("IRC registration failed");
        registered = true;
      }
    }
    if (!impl_->connected)
      throw std::runtime_error("IRC registration deadline or cancellation");
    impl_->observation.state = CaptureState::Complete;
    return impl_->deltaResult(0, gapStart);
  } catch (const std::exception &e) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "IRC connection or history gap"});
    auto result   = impl_->deltaResult(0, gapStart);
    result.detail = e.what();
    return result;
  }
}

IrcConnectionIdentity IrcSession::identity() const {
  return {impl_->observation.serverAuthority,
          impl_->observation.networkIncarnation,
          impl_->observation.connectionIncarnation};
}

IrcPendingReadBounds IrcSession::pendingReadBounds() const noexcept {
  return {impl_->config.maxPendingReads, impl_->config.maxPendingRecords,
          impl_->config.maxPendingBytes};
}

IrcTransportResult IrcSession::readLive(std::size_t maxMessages,
                                        const std::atomic_bool &cancel) {
  return readLive({}, maxMessages, cancel);
}

IrcTransportResult IrcSession::readLive(std::string_view target,
                                        std::size_t maxMessages,
                                        const std::atomic_bool &cancel) {
  auto result = pendingLive(target, maxMessages, cancel);
  if (result.readTicket && acknowledgeRead(*result.readTicket))
    result.readTicket.reset();
  return result;
}

IrcTransportResult IrcSession::pendingLive(std::string_view target,
                                           std::size_t maxMessages,
                                           const std::atomic_bool &cancel) {
  if ((!target.empty() && (target.size() > 512 || !singleIrcToken(target))) ||
      maxMessages > impl_->config.maxEvents) {
    return impl_->waitingResult(
        "live read target or message limit exceeds configured bound",
        CaptureState::Partial);
  }
  const std::string key = foldedTarget(target);
  if (const auto cached = impl_->pendingSnapshot(Impl::PendingKind::Live, key))
    return *cached;
  const auto end = impl_->deadline();
  auto operation = impl_->lockOperation(end, cancel);
  if (!operation.owns_lock()) {
    return impl_->waitingResult("live read cancelled while waiting for session",
                                cancel.load() ? CaptureState::Cancelled
                                              : CaptureState::Partial);
  }
  const auto gapStart = impl_->observation.gaps.size();
  if (const auto cached = impl_->pendingSnapshot(Impl::PendingKind::Live, key))
    return *cached;
  std::size_t scanCursor{};
  bool cursorBudgetExceeded{};
  bool pendingBudgetExceeded{};
  {
    const std::lock_guard lock(impl_->pendingMutex);
    pendingBudgetExceeded =
        impl_->pendingReads.size() >=
        std::min(impl_->config.maxPendingReads, impl_->config.maxReadTargets);
    const auto cursor = impl_->readCursors.find(key);
    cursorBudgetExceeded =
        cursor == impl_->readCursors.end() &&
        impl_->readCursors.size() >= impl_->config.maxReadTargets;
    if (cursor != impl_->readCursors.end()) scanCursor = cursor->second;
  }
  if (pendingBudgetExceeded) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "unacknowledged IRC read ticket budget exceeded"});
    return impl_->deltaResult(impl_->observation.messages.size(), gapStart);
  }
  if (!target.empty() && isChannelTarget(target) && !impl_->joined(target)) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "target-scoped read requires confirmed channel membership"});
    return impl_->deltaResult(impl_->observation.messages.size(), gapStart);
  }
  if (cursorBudgetExceeded) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "live read target cursor budget exceeded"});
    return impl_->deltaResult(impl_->observation.messages.size(), gapStart);
  }
  const auto messageBegin = scanCursor;
  std::vector<IrcMessageEvidence> selected;
  auto collect = [&] {
    while (scanCursor < impl_->observation.messages.size() &&
           selected.size() < maxMessages) {
      const auto &message = impl_->observation.messages[scanCursor++];
      const bool matches =
          target.empty() ||
          messageMatchesTarget(message, target, impl_->config.nick);
      if (matches) selected.push_back(message);
    }
  };
  collect();
  while (!cancel.load() && selected.size() < maxMessages) {
    try {
      if (!impl_->readOne(end, cancel)) {
        impl_->observation.state = CaptureState::Partial;
        impl_->observation.gaps.push_back(
            {{}, {}, "live stream prefix ended at deadline or disconnect"});
        break;
      }
      collect();
    } catch (const std::exception &e) {
      impl_->observation.state = CaptureState::Partial;
      impl_->observation.gaps.push_back(
          {{}, {}, std::string("live stream ended: ") + e.what()});
      break;
    }
  }
  if (maxMessages == 0) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back({{}, {}, "live read limit is zero"});
  } else if (selected.size() >= maxMessages) {
    impl_->observation.state = CaptureState::Partial;
    const auto &last         = selected.back();
    impl_->observation.gaps.push_back(
        {last.messageId.value_or(std::string{}),
         {},
         "live stream observation is a bounded prefix"});
  }
  if (cancel.load()) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "live stream observation was cancelled"});
  }
  IrcTransportResult result;
  result = impl_->deltaResult(messageBegin, gapStart, std::move(selected));
  if (cancel.load()) result.observation.state = CaptureState::Cancelled;
  Impl::PendingRead pending;
  pending.key       = key;
  pending.kind      = Impl::PendingKind::Live;
  pending.cursorEnd = scanCursor;
  IrcTransportResult retained;
  if (!impl_->storePending(std::move(pending), std::move(result), retained))
    return impl_->deltaResult(impl_->observation.messages.size(), gapStart);
  return retained;
}

IrcTransportResult IrcSession::history(const IrcHistoryRequest &request,
                                       const std::atomic_bool &cancel) {
  auto result = pendingHistory(request, cancel);
  if (result.readTicket && acknowledgeRead(*result.readTicket))
    result.readTicket.reset();
  return result;
}

IrcTransportResult IrcSession::pendingHistory(const IrcHistoryRequest &request,
                                              const std::atomic_bool &cancel) {
  if (!singleIrcToken(request.target) || request.target.size() > 512 ||
      request.afterMessageId.size() > 512 ||
      (!request.afterMessageId.empty() &&
       !singleIrcToken(request.afterMessageId)) ||
      !request.limit) {
    auto result = impl_->waitingResult(
        "CHATHISTORY target or cursor exceeds configured bound",
        CaptureState::Partial);
    result.observation.gaps.front().fromCursor =
        request.afterMessageId.substr(0, 512);
    return result;
  }
  const std::string historyKey =
      "H" + std::to_string(request.target.size()) + ":" + request.target +
      std::to_string(request.afterMessageId.size()) + ":" +
      request.afterMessageId + ":" + std::to_string(request.limit);
  if (const auto cached =
          impl_->pendingSnapshot(Impl::PendingKind::History, historyKey))
    return *cached;
  auto end       = impl_->deadline();
  auto operation = impl_->lockOperation(end, cancel);
  if (!operation.owns_lock()) {
    auto result = impl_->waitingResult(
        "history cancelled while waiting for session",
        cancel.load() ? CaptureState::Cancelled : CaptureState::Partial);
    result.observation.gaps.front().fromCursor = request.afterMessageId;
    return result;
  }
  if (const auto cached =
          impl_->pendingSnapshot(Impl::PendingKind::History, historyKey))
    return *cached;
  const auto messageStart = impl_->observation.messages.size();
  const auto gapStart     = impl_->observation.gaps.size();
  if (!impl_->connected || !impl_->historyCap ||
      (isChannelTarget(request.target) && !impl_->joined(request.target)) ||
      !request.limit) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {request.afterMessageId.substr(0, 512),
         {},
         "CHATHISTORY unavailable or cursor missing"});
    return impl_->deltaResult(messageStart, gapStart);
  }
  bool pendingBudgetExceeded{};
  {
    const std::lock_guard lock(impl_->pendingMutex);
    pendingBudgetExceeded =
        impl_->pendingReads.size() >=
        std::min(impl_->config.maxPendingReads, impl_->config.maxReadTargets);
  }
  if (pendingBudgetExceeded) {
    impl_->observation.state = CaptureState::Partial;
    impl_->observation.gaps.push_back(
        {{}, {}, "unacknowledged IRC read ticket budget exceeded"});
    return impl_->deltaResult(messageStart, gapStart);
  }
  auto limit                = std::min(request.limit, impl_->config.maxEvents);
  impl_->historyBatchSeen   = false;
  impl_->historyBatchClosed = false;
  std::string command       = request.afterMessageId.empty()
                                  ? "CHATHISTORY LATEST " + request.target + " * " +
                                  std::to_string(limit)
                                  : "CHATHISTORY AFTER " + request.target + " " +
                                  request.afterMessageId + " " +
                                  std::to_string(limit);
  if (command.size() + 2 > impl_->config.maxLineBytes ||
      !impl_->write(command + "\r\n", end, cancel)) {
    impl_->observation.gaps.push_back(
        {request.afterMessageId, {}, "CHATHISTORY request failed"});
    impl_->observation.state = CaptureState::Partial;
    return impl_->deltaResult(messageStart, gapStart);
  }
  while (!cancel.load() && std::chrono::steady_clock::now() < end &&
         !impl_->historyBatchClosed) {
    try {
      if (!impl_->readOne(end, cancel)) break;
    } catch (const std::exception &error) {
      impl_->observation.gaps.push_back(
          {request.afterMessageId,
           {},
           std::string("CHATHISTORY response failed: ") + error.what()});
      impl_->observation.state = CaptureState::Partial;
      break;
    }
  }
  if (!impl_->historyBatchSeen || !impl_->historyBatchClosed) {
    impl_->observation.gaps.push_back(
        {request.afterMessageId,
         {},
         "CHATHISTORY response lacked a closed BATCH boundary"});
    impl_->observation.state = CaptureState::Partial;
  }
  if (cancel.load()) impl_->observation.state = CaptureState::Partial;
  std::vector<IrcMessageEvidence> selected;
  for (std::size_t index = messageStart;
       index < impl_->observation.messages.size(); ++index) {
    const auto &message = impl_->observation.messages[index];
    if (messageMatchesTarget(message, request.target, impl_->config.nick))
      selected.push_back(message);
  }
  auto result = impl_->deltaResult(messageStart, gapStart, std::move(selected));
  if (cancel.load()) result.observation.state = CaptureState::Cancelled;
  Impl::PendingRead pending;
  pending.key  = historyKey;
  pending.kind = Impl::PendingKind::History;
  IrcTransportResult retained;
  if (!impl_->storePending(std::move(pending), std::move(result), retained))
    return impl_->deltaResult(messageStart, gapStart);
  return retained;
}

bool IrcSession::acknowledgeRead(std::string_view ticket) {
  if (ticket.size() != 32) return false;
  std::shared_ptr<const IrcTransportResult> releasedSnapshot;
  {
    const std::lock_guard lock(impl_->pendingMutex);
    const auto found = impl_->pendingReads.find(std::string(ticket));
    if (found == impl_->pendingReads.end()) return false;
    const auto &pending = found->second;
    if (pending.kind == Impl::PendingKind::Live) {
      impl_->readCursors[pending.key] = pending.cursorEnd;
      impl_->pendingLiveByTarget.erase(pending.key);
    } else {
      impl_->pendingHistoryByRequest.erase(pending.key);
    }
    impl_->pendingRecordCount -= pending.records;
    impl_->pendingByteCount -= pending.bytes;
    releasedSnapshot = pending.snapshot;
    impl_->pendingReads.erase(found);
  }
  static_cast<void>(releasedSnapshot);
  return true;
}

IrcJoinReceipt IrcSession::join(const IrcJoinAuthority &authority,
                                const std::atomic_bool &cancel) {
  IrcJoinReceipt receipt;
  const auto end = impl_->deadline();
  auto operation = impl_->lockOperation(end, cancel);
  if (!operation.owns_lock()) {
    receipt.detail = "IRC JOIN cancelled while waiting for session";
    return receipt;
  }
  const auto messageStart = impl_->observation.messages.size();
  const auto gapStart     = impl_->observation.gaps.size();
  auto finish             = [&]() {
    receipt.observation =
        impl_->deltaResult(messageStart, gapStart).observation;
    return std::move(receipt);
  };
  if (!impl_->connected ||
      authority.serverAuthority != impl_->observation.serverAuthority ||
      authority.networkIncarnation != impl_->observation.networkIncarnation ||
      !isChannelTarget(authority.target) || !singleIrcToken(authority.target) ||
      authority.audience.size() != 1 ||
      authority.audience.front() != authority.target || !authority.revoked ||
      authority.revoked->load() ||
      std::chrono::steady_clock::now() >= authority.expiresAt) {
    receipt.detail =
        "IRC join authority is absent, expired, revoked, or mismatched";
    return finish();
  }
  if (impl_->joined(authority.target)) {
    receipt.joined = true;
    return finish();
  }
  if (cancel.load() || authority.revoked->load()) {
    receipt.detail = "IRC join cancelled before write";
    return finish();
  }
  if (std::chrono::steady_clock::now() >= authority.expiresAt) {
    receipt.detail = "IRC join authority expired before write";
    return finish();
  }
  auto command       = "JOIN " + authority.target + "\r\n";
  impl_->pendingJoin = authority.target;
  struct PendingJoinReset {
    std::optional<std::string> &target;
    ~PendingJoinReset() { target.reset(); }
  } pendingJoinReset{impl_->pendingJoin};
  if (command.size() > impl_->config.maxLineBytes ||
      !impl_->write(command, end, cancel)) {
    impl_->pendingJoin.reset();
    receipt.detail = "IRC JOIN write failed";
    return finish();
  }
  while (!cancel.load() && std::chrono::steady_clock::now() < end &&
         !impl_->joined(authority.target)) {
    std::optional<std::string> serverError;
    if (!impl_->readOne(end, cancel, &serverError)) break;
    if (serverError) {
      receipt.serverReply = std::move(*serverError);
      receipt.detail      = "IRC server rejected channel membership";
      impl_->pendingJoin.reset();
      return finish();
    }
  }
  impl_->pendingJoin.reset();
  receipt.joined = impl_->joined(authority.target);
  if (!receipt.joined)
    receipt.detail = cancel.load() ? "IRC join cancelled"
                                   : "IRC join confirmation deadline elapsed";
  return finish();
}

IrcSendReceipt IrcSession::send(const IrcSendAuthority &authority,
                                const std::atomic_bool &cancel) {
  IrcSendReceipt receipt;
  const auto end = impl_->deadline();
  auto operation = impl_->lockOperation(end, cancel);
  if (!operation.owns_lock()) {
    receipt.detail = "IRC send cancelled while waiting for session";
    return receipt;
  }
  if (!impl_->connected ||
      authority.serverAuthority != impl_->observation.serverAuthority ||
      authority.networkIncarnation != impl_->observation.networkIncarnation ||
      authority.connectionIncarnation !=
          impl_->observation.connectionIncarnation ||
      authority.target.empty() || authority.payload.empty() ||
      authority.audience.size() != 1 ||
      authority.audience.front() != authority.target || !authority.revoked ||
      authority.revoked->load() ||
      std::chrono::steady_clock::now() >= authority.expiresAt) {
    receipt.detail =
        "IRC send authority is absent, expired, revoked, or mismatched";
    return receipt;
  }
  auto prefix = "PRIVMSG " + authority.target + " :";
  if (!singleIrcToken(authority.target) || hasLineBreak(authority.payload)) {
    receipt.detail = "IRC target or payload contains a forbidden line byte";
    return receipt;
  }
  if (isChannelTarget(authority.target) && !impl_->joined(authority.target)) {
    receipt.detail = "IRC channel send requires confirmed membership";
    return receipt;
  }
  if (prefix.size() + authority.payload.size() + 2 >
      impl_->config.maxLineBytes) {
    receipt.detail = "IRC payload exceeds protocol line byte limit";
    return receipt;
  }
  if (cancel.load() || authority.revoked->load()) {
    receipt.detail = "cancelled before send";
    return receipt;
  }
  impl_->echoId.reset();
  impl_->wantEcho            = impl_->echo;
  impl_->expectedEchoTarget  = authority.target;
  impl_->expectedEchoPayload = authority.payload;
  struct EchoExpectationReset {
    Impl &session;
    ~EchoExpectationReset() {
      session.wantEcho = false;
      session.expectedEchoTarget.clear();
      session.expectedEchoPayload.clear();
      session.echoId.reset();
    }
  } echoExpectationReset{*impl_};
  std::size_t written{};
  if (!impl_->write(prefix + authority.payload + "\r\n", end, cancel,
                    &written)) {
    receipt.state =
        written == 0 ? IrcSendState::Rejected : IrcSendState::WrittenUnknown;
    receipt.detail =
        written == 0
            ? "send stopped before any bytes were written"
            : "partial write may have reached server; acknowledgment lost";
    return receipt;
  }
  if (!impl_->echo) {
    receipt.state  = IrcSendState::WrittenUnknown;
    receipt.detail = "bytes written; echo-message was not negotiated";
    return receipt;
  }
  std::optional<std::string> error;
  while (!cancel.load() && std::chrono::steady_clock::now() < end &&
         !impl_->echoId)
    if (!impl_->readOne(end, cancel, &error)) break;
  if (error) {
    receipt.state       = IrcSendState::Rejected;
    receipt.serverReply = *error;
  } else if (impl_->echoId) {
    receipt.state = IrcSendState::EchoAcknowledged;
    receipt.detail =
        "matching server echo observed; recipient delivery is not verified";
    if (!impl_->echoId->empty()) receipt.echoMessageId = impl_->echoId;
  } else {
    receipt.state  = IrcSendState::WrittenUnknown;
    receipt.detail = "bytes written; server echo deadline elapsed";
  }
  return receipt;
}

void IrcSession::cancel() noexcept {
  if (!impl_) return;
  impl_->cancelRequested.store(true, std::memory_order_relaxed);
  impl_->socket.shutdown();
}
} // namespace xanadu::evidence
