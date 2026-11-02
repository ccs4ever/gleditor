#include "common/xanadu/evidence/fediverse_transport.hpp"

#include <boost/json.hpp>
#include <boost/json/src.hpp>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace xanadu::evidence {
namespace {
using Json  = boost::json::value;
using Clock = std::chrono::steady_clock;

struct Origin {
  std::string scheme;
  std::string host;
  std::string port;
  std::string text;
};

void initializeCurl() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
      throw std::runtime_error("HTTP transport initialization failed");
  });
}

std::string curlPart(CURLU *url, CURLUPart part) {
  char *value{};
  const auto result = curl_url_get(url, part, &value, 0);
  if (result != CURLUE_OK) return {};
  std::string copy(value);
  curl_free(value);
  return copy;
}

bool loopbackHost(std::string host) {
  std::ranges::transform(host, host.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return host == "localhost" || host == "127.0.0.1" || host == "::1" ||
         host == "[::1]";
}

std::optional<Origin> parseOrigin(const std::string_view text,
                                  const bool allowLoopbackHttp) {
  if (text.empty() || text.find('\0') != text.npos) return {};
  initializeCurl();
  CURLU *raw = curl_url();
  if (!raw) return {};
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(
      raw, curl_url_cleanup);
  const std::string input(text);
  if (curl_url_set(url.get(), CURLUPART_URL, input.c_str(), 0) != CURLUE_OK)
    return {};
  const auto scheme   = curlPart(url.get(), CURLUPART_SCHEME);
  auto host           = curlPart(url.get(), CURLUPART_HOST);
  const auto user     = curlPart(url.get(), CURLUPART_USER);
  const auto pass     = curlPart(url.get(), CURLUPART_PASSWORD);
  const auto path     = curlPart(url.get(), CURLUPART_PATH);
  const auto query    = curlPart(url.get(), CURLUPART_QUERY);
  const auto fragment = curlPart(url.get(), CURLUPART_FRAGMENT);
  if ((!user.empty() || !pass.empty()) || (!path.empty() && path != "/") ||
      !query.empty() || !fragment.empty() || host.empty())
    return {};
  std::string normalizedScheme = scheme;
  std::string normalizedHost   = host;
  std::ranges::transform(
      normalizedScheme, normalizedScheme.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  std::ranges::transform(
      normalizedHost, normalizedHost.begin(),
      [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (normalizedScheme != "https" && normalizedScheme != "http") return {};
  if (normalizedScheme == "http" &&
      (!allowLoopbackHttp || !loopbackHost(normalizedHost)))
    return {};
  const auto port = curlPart(url.get(), CURLUPART_PORT);
  Origin result{.scheme = normalizedScheme,
                .host   = normalizedHost,
                .port   = port,
                .text   = {}};
  result.text = result.scheme + "://" + result.host;
  if (!port.empty() && !((result.scheme == "https" && port == "443") ||
                         (result.scheme == "http" && port == "80")))
    result.text += ":" + port;
  return result;
}

std::string escapedSegment(std::string_view segment);

std::optional<std::string> sameOriginUrl(const Origin &origin,
                                         const std::string_view target) {
  if (target.empty() || target.find('\0') != target.npos) return {};
  CURLU *raw = curl_url();
  if (!raw) return {};
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(
      raw, curl_url_cleanup);
  if (curl_url_set(url.get(), CURLUPART_URL, origin.text.c_str(), 0) !=
          CURLUE_OK ||
      curl_url_set(url.get(), CURLUPART_URL, std::string(target).c_str(), 0) !=
          CURLUE_OK)
    return {};
  const auto scheme   = curlPart(url.get(), CURLUPART_SCHEME);
  const auto host     = curlPart(url.get(), CURLUPART_HOST);
  const auto port     = curlPart(url.get(), CURLUPART_PORT);
  const auto user     = curlPart(url.get(), CURLUPART_USER);
  const auto password = curlPart(url.get(), CURLUPART_PASSWORD);
  const auto resolvedPort =
      port.empty() ? (scheme == "https" ? "443" : "80") : port;
  const auto originPort = origin.port.empty()
                              ? (origin.scheme == "https" ? "443" : "80")
                              : origin.port;
  if (!user.empty() || !password.empty() || scheme != origin.scheme ||
      host != origin.host || resolvedPort != originPort)
    return {};
  char *absolute{};
  if (curl_url_get(url.get(), CURLUPART_URL, &absolute, 0) != CURLUE_OK)
    return {};
  std::string result(absolute);
  curl_free(absolute);
  return result;
}

bool isStatusesEndpoint(const std::string_view url,
                        const std::string_view accountId) {
  CURLU *raw = curl_url();
  if (!raw) return false;
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
      raw, curl_url_cleanup);
  const std::string copy(url);
  if (curl_url_set(parsed.get(), CURLUPART_URL, copy.c_str(), 0) != CURLUE_OK)
    return false;
  return curlPart(parsed.get(), CURLUPART_PATH) ==
         "/api/v1/accounts/" + escapedSegment(accountId) + "/statuses";
}

std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

struct Response {
  std::string body;
  std::vector<std::string> links;
  std::size_t maxBody{};
  Clock::time_point deadline;
  const std::atomic_bool *cancel{};
  bool tooLarge{};
  bool timedOut{};
  bool preflightRefused{};
  bool storageFailed{};
  std::size_t headerBytes{};
  std::size_t maxHeaderBytes{};
  const std::function<bool()> *permission{};
};

std::size_t receiveBody(char *data, std::size_t size, std::size_t count,
                        void *context) noexcept {
  auto &response = *static_cast<Response *>(context);
  if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
  const auto bytes = size * count;
  if (bytes >
      response.maxBody - std::min(response.body.size(), response.maxBody)) {
    response.tooLarge = true;
    return 0;
  }
  try {
    response.body.append(data, bytes);
  } catch (...) {
    response.storageFailed = true;
    return 0;
  }
  return bytes;
}

std::size_t receiveHeader(char *data, std::size_t size, std::size_t count,
                          void *context) noexcept {
  auto &response = *static_cast<Response *>(context);
  if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
  const auto bytes = size * count;
  if (bytes > response.maxHeaderBytes - response.headerBytes) {
    response.tooLarge = true;
    return 0;
  }
  response.headerBytes += bytes;
  try {
    const std::string_view line(data, size * count);
    const auto colon = line.find(':');
    if (colon != line.npos &&
        lower(std::string(line.substr(0, colon))) == "link")
      response.links.emplace_back(line.substr(colon + 1));
  } catch (...) {
    response.storageFailed = true;
    return 0;
  }
  return bytes;
}

int progress(void *context, curl_off_t, curl_off_t, curl_off_t,
             curl_off_t) noexcept {
  auto &response = *static_cast<Response *>(context);
  try {
    if (response.permission && *response.permission &&
        !(*response.permission)()) {
      response.preflightRefused = true;
      return 1;
    }
  } catch (...) {
    response.storageFailed = true;
    return 1;
  }
  if (response.cancel && response.cancel->load(std::memory_order_relaxed))
    return 1;
  if (Clock::now() >= response.deadline) {
    response.timedOut = true;
    return 1;
  }
  return 0;
}

struct HeaderList {
  curl_slist *value{};
  ~HeaderList() {
    for (auto *item = value; item; item = item->next)
      if (item->data) OPENSSL_cleanse(item->data, std::strlen(item->data));
    curl_slist_free_all(value);
  }
  bool add(const std::string &header) {
    auto *next = curl_slist_append(value, header.c_str());
    if (!next) return false;
    value = next;
    return true;
  }
};

struct HandleCloser {
  void operator()(CURL *handle) const { curl_easy_cleanup(handle); }
};
using Handle = std::unique_ptr<CURL, HandleCloser>;

struct HttpResponse {
  CURLcode result{CURLE_FAILED_INIT};
  long status{};
  Response receive;
  bool started{};
};

HttpResponse perform(
    const std::string &url, const std::string *token,
    const std::string *postBody, const std::string *idempotencyKey,
    const std::size_t cap, const long timeoutMs, const std::atomic_bool &cancel,
    const Clock::time_point deadline       = Clock::time_point::max(),
    const std::function<bool()> &preflight = {},
    std::size_t maxHeaderBytes = FediverseTransportLimits{}.maxHeaderBytes) {
  HttpResponse response;
  response.receive.maxBody = cap;
  response.receive.deadline =
      deadline == Clock::time_point::max()
          ? Clock::now() + std::chrono::milliseconds(timeoutMs)
          : deadline;
  response.receive.cancel         = &cancel;
  response.receive.permission     = &preflight;
  response.receive.maxHeaderBytes = maxHeaderBytes;
  Handle handle(curl_easy_init());
  if (!handle) return response;
  HeaderList headers;
  if (!headers.add("Accept: application/json") ||
      (token && !headers.add("Authorization: Bearer " + *token)) ||
      (postBody &&
       !headers.add("Content-Type: application/x-www-form-urlencoded")) ||
      (idempotencyKey && !headers.add("Idempotency-Key: " + *idempotencyKey)))
    return response;
  curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(handle.get(), CURLOPT_PROXY, "");
  curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT_MS,
                   std::min(timeoutMs, 3000L));
  curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT_MS, timeoutMs);
  curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.value);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receiveBody);
  curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response.receive);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERFUNCTION, receiveHeader);
  curl_easy_setopt(handle.get(), CURLOPT_HEADERDATA, &response.receive);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION, progress);
  curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &response.receive);
  curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS, 0L);
  if (postBody) {
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, postBody->data());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(postBody->size()));
  }
  if (preflight && !preflight()) {
    response.receive.preflightRefused = true;
    response.result                   = CURLE_ABORTED_BY_CALLBACK;
    return response;
  }
  response.started = true;
  response.result  = curl_easy_perform(handle.get());
  curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &response.status);
  return response;
}

std::optional<std::string> stringField(const Json &value,
                                       const std::string_view field) {
  if (!value.is_object()) return {};
  const auto *member = value.as_object().if_contains(field);
  if (!member || !member->is_string()) return {};
  return std::string(member->as_string());
}

std::optional<std::string> nullableStringField(const Json &value,
                                               const std::string_view field) {
  if (!value.is_object()) return {};
  const auto *member = value.as_object().if_contains(field);
  if (!member || member->is_null()) return {};
  if (!member->is_string()) return {};
  return std::string(member->as_string());
}

bool containsCredential(const Json &value, std::string_view credential) {
  if (value.is_string())
    return std::string_view(value.as_string()).find(credential) !=
           std::string_view::npos;
  if (value.is_array())
    return std::ranges::any_of(value.as_array(), [&](const auto &child) {
      return containsCredential(child, credential);
    });
  if (value.is_object())
    return std::ranges::any_of(value.as_object(), [&](const auto &entry) {
      return std::string_view(entry.key()).find(credential) !=
                 std::string_view::npos ||
             containsCredential(entry.value(), credential);
    });
  return false;
}

bool credentialInLocator(std::string_view locator,
                         const std::string &credential) {
  if (locator.find(credential) != locator.npos) return true;
  if (locator.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max()))
    return true;
  int size{};
  char *raw = curl_easy_unescape(nullptr, locator.data(),
                                 static_cast<int>(locator.size()), &size);
  if (!raw) return true;
  const bool contains =
      std::string_view(raw, static_cast<std::size_t>(size)).find(credential) !=
      std::string_view::npos;
  curl_free(raw);
  return contains;
}

std::pair<std::string, bool> retainedResponse(std::string_view body,
                                              const std::string *credential) {
  if (!credential || credential->empty()) return {std::string(body), false};
  bool contains = body.find(*credential) != body.npos;
  if (!contains) {
    boost::system::error_code error;
    const auto parsed = boost::json::parse(body, error);
    contains          = !error && containsCredential(parsed, *credential);
  }
  if (contains)
    return {"[Original response withheld: authentication material was present]",
            true};
  return {std::string(body), false};
}

std::string sha256(const std::string_view text) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length{};
  EVP_MD_CTX *raw = EVP_MD_CTX_new();
  if (!raw) throw std::runtime_error("request fingerprint unavailable");
  const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
      raw, EVP_MD_CTX_free);
  if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context.get(), text.data(), text.size()) != 1 ||
      EVP_DigestFinal_ex(context.get(), digest.data(), &length) != 1)
    throw std::runtime_error("request fingerprint unavailable");
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 0x0f]);
  }
  return result;
}

void appendField(std::string &out, const std::string_view field) {
  out += std::to_string(field.size());
  out.push_back(':');
  out.append(field);
  out.push_back(';');
}

void appendBounds(std::string &out, const FediverseTransportLimits &limits) {
  appendField(out, std::to_string(limits.records.maxRecords));
  appendField(out, std::to_string(limits.records.maxBytes));
  appendField(out, std::to_string(limits.records.maxStringBytes));
  appendField(out, std::to_string(limits.records.maxParts));
  appendField(out, std::to_string(limits.records.maxPartDepth));
  appendField(out, std::to_string(limits.maxPages));
  appendField(out, std::to_string(limits.timeoutMs));
  appendField(out, std::to_string(limits.maxHeaderBytes));
}

bool validLimits(const FediverseTransportLimits &limits) {
  return limits.records.maxRecords > 0 && limits.records.maxBytes > 0 &&
         limits.records.maxStringBytes > 0 && limits.records.maxParts > 0 &&
         limits.records.maxPartDepth > 0 && limits.maxPages > 0 &&
         limits.timeoutMs > 0 && limits.maxHeaderBytes > 0;
}

bool boundedTarget(std::string_view value,
                   const FediverseTransportLimits &limits) {
  return !value.empty() && value.size() <= limits.records.maxStringBytes &&
         value.find('\0') == value.npos;
}

bool authorized(const FediverseAuthority &authority,
                const FediverseCredentialLease &credential,
                const Origin &origin, const std::string_view account,
                const FediverseAction action,
                const std::string_view fingerprint,
                const std::vector<std::string> &audience,
                const std::atomic_bool &cancel) {
  return !cancel.load(std::memory_order_relaxed) &&
         !authority.revoked.load(std::memory_order_relaxed) &&
         credential.usable() && Clock::now() < authority.expiresAt &&
         authority.instanceOrigin == origin.text &&
         credential.matches(origin.text, account) &&
         authority.accountId == account && authority.action == action &&
         authority.requestFingerprint == fingerprint &&
         authority.audience == audience;
}

std::optional<std::string> nextLink(const std::vector<std::string> &links,
                                    bool &malformed) {
  std::optional<std::string> next;
  malformed = false;
  for (const auto &line : links) {
    std::size_t start{};
    while (start < line.size()) {
      while (start < line.size() &&
             (std::isspace(static_cast<unsigned char>(line[start])) ||
              line[start] == ','))
        ++start;
      if (start == line.size()) break;
      if (line[start] != '<') {
        malformed = true;
        return {};
      }
      const auto end = line.find('>', start + 1);
      if (end == line.npos) {
        malformed = true;
        return {};
      }
      const std::string target = line.substr(start + 1, end - start - 1);
      const auto itemEnd       = line.find(',', end + 1);
      const auto params        = line.substr(
          end + 1, (itemEnd == line.npos ? line.size() : itemEnd) - end - 1);
      std::size_t p{};
      bool isNext = false;
      while (p < params.size()) {
        while (p < params.size() &&
               (std::isspace(static_cast<unsigned char>(params[p])) ||
                params[p] == ';'))
          ++p;
        const auto semicolon = params.find(';', p);
        auto parameter       = params.substr(
            p, (semicolon == params.npos ? params.size() : semicolon) - p);
        p = semicolon == params.npos ? params.size() : semicolon + 1;
        const auto equal = parameter.find('=');
        if (equal == parameter.npos) continue;
        auto name  = lower(parameter.substr(0, equal));
        auto value = parameter.substr(equal + 1);
        while (!name.empty() &&
               std::isspace(static_cast<unsigned char>(name.back())))
          name.pop_back();
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.front())))
          value.erase(value.begin());
        while (!value.empty() &&
               std::isspace(static_cast<unsigned char>(value.back())))
          value.pop_back();
        if (name == "rel") {
          if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
          std::istringstream relations(value);
          std::string relation;
          while (relations >> relation)
            if (lower(relation) == "next") isNext = true;
        }
      }
      if (isNext) {
        if (target.empty() || next) {
          malformed = true;
          return {};
        }
        next = target;
      }
      start = itemEnd == line.npos ? line.size() : itemEnd + 1;
    }
  }
  return next;
}

std::string escapedSegment(const std::string_view segment) {
  Handle handle(curl_easy_init());
  if (!handle) return {};
  char *raw = curl_easy_escape(handle.get(), segment.data(),
                               static_cast<int>(segment.size()));
  if (!raw) return {};
  std::string result(raw);
  curl_free(raw);
  return result;
}

std::string urlEncode(const std::string_view value) {
  std::string result;
  for (const auto byte : value) {
    const auto ch = static_cast<unsigned char>(byte);
    if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
      result.push_back(static_cast<char>(ch));
    else {
      static constexpr char hex[] = "0123456789ABCDEF";
      result.push_back('%');
      result.push_back(hex[ch >> 4]);
      result.push_back(hex[ch & 0x0f]);
    }
  }
  return result;
}

void addGap(FediverseTransportResult &result, std::string locator,
            std::string reason) {
  if (result.gaps.size() < result.observation.bounds.maxRecords)
    result.gaps.push_back({std::move(locator), std::move(reason)});
  if (result.observation.state != CaptureState::Cancelled)
    result.observation.state = CaptureState::Partial;
}

} // namespace

FediverseCredentialLease::FediverseCredentialLease(
    std::string instanceOrigin, std::string accountId, std::string bearerToken,
    const std::chrono::steady_clock::time_point expiresAt)
    : instanceOrigin_(std::move(instanceOrigin)),
      accountId_(std::move(accountId)), token_(std::move(bearerToken)),
      expiresAt_(expiresAt) {
  bool padding = false;
  const bool valid =
      !token_.empty() &&
      token_.size() <= FediverseTransportLimits{}.maxHeaderBytes / 2 &&
      std::ranges::all_of(token_, [&](unsigned char c) {
        if (c == '=') {
          padding = true;
          return true;
        }
        return !padding && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~' || c == '+' || c == '/');
      });
  if (!valid) {
    if (!token_.empty()) OPENSSL_cleanse(token_.data(), token_.size());
    throw std::invalid_argument("invalid bearer credential representation");
  }
}
FediverseCredentialLease::~FediverseCredentialLease() {
  if (!token_.empty()) OPENSSL_cleanse(token_.data(), token_.size());
}
FediverseCredentialLease *FediverseCredentialLease::revoke() noexcept {
  revoked_ = true;
  return this;
}
bool FediverseCredentialLease::revoked() const noexcept {
  return revoked_.load();
}
bool FediverseCredentialLease::usable() const noexcept {
  return !revoked_.load(std::memory_order_relaxed) &&
         Clock::now() < expiresAt_ && !token_.empty();
}
bool FediverseCredentialLease::matches(
    const std::string_view instanceOrigin,
    const std::string_view accountId) const noexcept {
  return instanceOrigin_ == instanceOrigin && accountId_ == accountId;
}

FediverseAuthority::FediverseAuthority(
    std::string origin, std::string account, FediverseAction requestedAction,
    std::string fingerprint, std::vector<std::string> requestedAudience,
    const std::chrono::steady_clock::time_point expiry)
    : instanceOrigin(std::move(origin)), accountId(std::move(account)),
      action(requestedAction), requestFingerprint(std::move(fingerprint)),
      audience(std::move(requestedAudience)), expiresAt(expiry) {}

std::optional<std::string>
fediverseReadProblem(const FediverseReadRequest &request,
                     const FediverseTransportPolicy policy) {
  if (!parseOrigin(request.instanceOrigin, policy.allowLoopbackHttpForTests) ||
      !boundedTarget(request.accountId, request.limits) ||
      !boundedTarget(request.accountActorUri, request.limits) ||
      request.accountId.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      !validLimits(request.limits))
    return "invalid Fediverse read target or limits";
  initializeCurl();
  CURLU *raw = curl_url();
  if (!raw) return "actor URI validation unavailable";
  const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> actor(
      raw, curl_url_cleanup);
  if (curl_url_set(actor.get(), CURLUPART_URL, request.accountActorUri.c_str(),
                   0) != CURLUE_OK ||
      curlPart(actor.get(), CURLUPART_SCHEME) != "https" ||
      curlPart(actor.get(), CURLUPART_HOST).empty() ||
      !curlPart(actor.get(), CURLUPART_USER).empty() ||
      !curlPart(actor.get(), CURLUPART_PASSWORD).empty() ||
      !curlPart(actor.get(), CURLUPART_FRAGMENT).empty())
    return "actor identity must be an absolute HTTPS URI without credentials "
           "or fragment";
  return {};
}

std::optional<std::string>
fediversePostProblem(const FediversePostRequest &request,
                     const FediverseTransportPolicy policy) {
  if (const auto problem =
          fediverseReadProblem({.instanceOrigin  = request.instanceOrigin,
                                .accountId       = request.accountId,
                                .accountActorUri = request.accountActorUri,
                                .limits          = request.limits},
                               policy))
    return problem;
  if (!boundedTarget(request.accountId, request.limits) ||
      !boundedTarget(request.accountActorUri, request.limits) ||
      (request.replyToRestId &&
       !boundedTarget(*request.replyToRestId, request.limits)) ||
      request.text.empty() || request.idempotencyKey.empty() ||
      request.idempotencyKey.size() > 256 ||
      request.idempotencyKey.find_first_of("\r\n") != std::string::npos ||
      request.idempotencyKey.find('\0') != std::string::npos ||
      (request.visibility != "public" && request.visibility != "unlisted" &&
       request.visibility != "private" && request.visibility != "direct") ||
      !validLimits(request.limits)) {
    return "invalid post request";
  }
  // Public/unlisted admission explicitly includes arbitrary readers. Restricted
  // delivery needs resolved mentions/followers scope before it can be admitted.
  if ((request.visibility != "public" && request.visibility != "unlisted") ||
      request.audience != std::vector<std::string>{
                              "https://www.w3.org/ns/activitystreams#Public"}) {
    return "restricted audience requires a resolved recipient binding";
  }
  if (request.text.size() > request.limits.records.maxStringBytes ||
      request.contentWarning.size() > request.limits.records.maxStringBytes ||
      request.text.size() > request.limits.records.maxBytes ||
      request.contentWarning.size() >
          request.limits.records.maxBytes -
              std::min(request.text.size(), request.limits.records.maxBytes)) {
    return "post payload exceeds configured limits";
  }
  for (const auto &target : request.audience)
    if (target.empty() ||
        target.size() > request.limits.records.maxStringBytes) {
      return "invalid declared post audience";
    }
  for (const auto ch : request.idempotencyKey)
    if (std::iscntrl(static_cast<unsigned char>(ch))) {
      return "invalid idempotency key";
    }
  return {};
}

std::string fediverseRequestFingerprint(const FediverseReadRequest &request) {
  std::string canonical;
  appendField(canonical, "read-statuses");
  appendField(canonical, request.instanceOrigin);
  appendField(canonical, request.accountId);
  appendField(canonical, request.accountActorUri);
  appendField(canonical, request.accountHandle);
  appendField(canonical, request.authenticated ? "authenticated" : "public");
  appendBounds(canonical, request.limits);
  return sha256(canonical);
}

std::string fediverseRequestFingerprint(const FediversePostRequest &request) {
  std::string canonical;
  appendField(canonical, "publish-status");
  appendField(canonical, request.instanceOrigin);
  appendField(canonical, request.accountId);
  appendField(canonical, request.accountActorUri);
  appendField(canonical, request.text);
  appendField(canonical, request.contentWarning);
  appendField(canonical, request.visibility);
  appendField(canonical, request.replyToRestId.value_or(""));
  appendField(canonical, request.idempotencyKey);
  appendField(canonical, std::to_string(request.audience.size()));
  for (const auto &target : request.audience) appendField(canonical, target);
  appendBounds(canonical, request.limits);
  return sha256(canonical);
}

FediverseTransport::FediverseTransport(FediverseTransportPolicy policy)
    : policy_(policy) {}

FediverseTransportResult FediverseTransport::read(
    const FediverseReadRequest &request,
    const std::shared_ptr<FediverseCredentialLease> &credential,
    const std::shared_ptr<FediverseAuthority> &authority,
    const std::atomic_bool &cancel) const {
  FediverseTransportResult result;
  result.observation.state             = CaptureState::Failed;
  result.observation.instanceAuthority = request.instanceOrigin;
  result.observation.accountId         = request.accountId;
  result.observation.accountActorUri   = request.accountActorUri;
  result.observation.bounds            = request.limits.records;
  const auto origin =
      parseOrigin(request.instanceOrigin, policy_.allowLoopbackHttpForTests);
  if (const auto problem = fediverseReadProblem(request, policy_)) {
    result.error = *problem;
    return result;
  }
  result.observation.collectionUri = origin->text + "/api/v1/accounts/" +
                                     escapedSegment(request.accountId) +
                                     "/statuses";
  const auto fingerprint = fediverseRequestFingerprint(request);
  if (request.authenticated &&
      (!credential || !authority ||
       !authorized(*authority, *credential, *origin, request.accountId,
                   FediverseAction::ReadStatuses, fingerprint, {}, cancel))) {
    result.error = "read authority or credential lease is unavailable";
    return result;
  }
  if (!request.authenticated && (credential || authority)) {
    result.error = "public read does not accept credential material";
    return result;
  }
  if (cancel.load()) {
    result.observation.state = CaptureState::Cancelled;
    result.error             = "read cancelled before request";
    return result;
  }

  const auto deadline =
      Clock::now() + std::chrono::milliseconds(request.limits.timeoutMs);
  std::size_t totalBytes{};
  std::size_t pages{};
  std::size_t objectCount{};
  std::size_t actorCount{};
  std::string next = result.observation.collectionUri + "?limit=" +
                     std::to_string(std::min<std::size_t>(
                         40, request.limits.records.maxRecords));
  std::set<std::string> seen;
  bool outstanding         = true;
  result.observation.state = CaptureState::Complete;
  while (pages < request.limits.maxPages) {
    if (Clock::now() >= deadline || cancel.load()) {
      result.observation.state =
          cancel.load() ? CaptureState::Cancelled : CaptureState::Partial;
      result.error =
          cancel.load() ? "read cancelled" : "read deadline exceeded";
      addGap(result, next, result.error);
      break;
    }
    const auto resolved = sameOriginUrl(*origin, next);
    if (!resolved || !isStatusesEndpoint(*resolved, request.accountId)) {
      addGap(result, next,
             "pagination target escaped the account statuses endpoint");
      result.error = "refused unsafe pagination target";
      break;
    }
    if (!seen.insert(*resolved).second) {
      addGap(result, *resolved,
             "pagination repeated a previously fetched page");
      result.error = "pagination cycle detected";
      break;
    }
    const auto remaining =
        request.limits.records.maxBytes -
        std::min(totalBytes, request.limits.records.maxBytes);
    const auto timeout =
        std::max<long>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - Clock::now())
                              .count());
    auto response = perform(
        *resolved, request.authenticated ? &credential->token_ : nullptr,
        nullptr, nullptr,
        std::min(remaining, request.limits.records.maxStringBytes), timeout,
        cancel, deadline,
        request.authenticated ? std::function<bool()>([&] {
          return authorized(*authority, *credential, *origin, request.accountId,
                            FediverseAction::ReadStatuses, fingerprint, {},
                            cancel);
        })
                              : std::function<bool()>{},
        request.limits.maxHeaderBytes);
    pages++;
    totalBytes += response.receive.body.size();
    const auto retained =
        retainedResponse(response.receive.body,
                         request.authenticated ? &credential->token_ : nullptr);
    result.pages.push_back(
        Page{.locator = *resolved,
             .status  = response.status,
             .body    = retained.first,
             .state   = response.result == CURLE_OK && response.status >= 200 &&
                              response.status < 300
                            ? CaptureState::Complete
                            : CaptureState::Failed,
             .error   = retained.second
                            ? "Original response withheld by credential policy"
                            : "",
             .redacted = retained.second});
    if (cancel.load()) {
      result.observation.state = CaptureState::Cancelled;
      result.error             = "read cancelled";
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.receive.timedOut || Clock::now() >= deadline) {
      result.error = "read deadline exceeded";
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.receive.tooLarge) {
      result.error = "response exceeded the aggregate byte budget";
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.receive.preflightRefused) {
      result.error =
          "read authority or credential lease expired before request";
      result.observation.state = CaptureState::Partial;
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.result != CURLE_OK) {
      result.error = "HTTP transport failed";
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.status >= 300 && response.status < 400) {
      result.error = "HTTP redirects are refused";
      addGap(result, *resolved, result.error);
      break;
    }
    if (response.status < 200 || response.status >= 300) {
      result.error =
          "Fediverse read returned HTTP " + std::to_string(response.status);
      addGap(result, *resolved, result.error);
      break;
    }
    boost::system::error_code parseError;
    const auto document = boost::json::parse(retained.first, parseError);
    if (parseError || !document.is_array()) {
      result.error = "Fediverse response is not a valid status array";
      addGap(result, *resolved, result.error);
      break;
    }
    for (const auto &status : document.as_array()) {
      if (!status.is_object()) {
        addGap(result, *resolved, "status entry is not an object");
        continue;
      }
      const auto restId     = stringField(status, "id");
      const auto objectUri  = stringField(status, "uri");
      const auto content    = stringField(status, "content");
      const auto visibility = stringField(status, "visibility");
      const auto account    = status.as_object().if_contains("account");
      if (!restId || restId->empty() || !objectUri || objectUri->empty() ||
          !content || !visibility || !account || !account->is_object()) {
        addGap(result, *resolved,
               "status lacks REST/object/account identity fields");
        continue;
      }
      const auto actorRestId = stringField(*account, "id");
      if (!actorRestId || *actorRestId != request.accountId) {
        addGap(result, *resolved,
               "status author has no configured ActivityPub identity binding");
        continue;
      }
      const auto actorUri = std::optional<std::string>{request.accountActorUri};
      if (const auto *boost = status.as_object().if_contains("reblog");
          boost && !boost->is_null()) {
        addGap(result, *resolved,
               "boost relationship retained in source page; Note projection "
               "refused");
        continue;
      }
      const auto warning   = stringField(status, "spoiler_text").value_or("");
      const auto editedAt  = nullableStringField(status, "edited_at");
      const auto createdAt = stringField(status, "created_at").value_or("");
      std::string revision = editedAt.value_or(createdAt);
      revision.push_back('\0');
      revision += *content;
      revision.push_back('\0');
      revision += *visibility;
      revision.push_back('\0');
      revision += warning;
      FediverseObjectEvidence object{.objectUri      = *objectUri,
                                     .restId         = *restId,
                                     .actorUri       = *actorUri,
                                     .type           = "Note",
                                     .content        = *content,
                                     .visibility     = *visibility,
                                     .contentWarning = warning,
                                     .inReplyToUri   = {},
                                     .revision       = sha256(revision),
                                     .audience       = {}};
      if (const auto replyUri = stringField(status, "in_reply_to_uri"))
        object.inReplyToUri = *replyUri;
      // Mention URLs in the REST entity are web profiles, not asserted
      // ActivityPub recipients. Exact mention evidence remains in the source
      // page.
      if (const auto *reply = status.as_object().if_contains("in_reply_to_id");
          reply && !reply->is_null() && !object.inReplyToUri)
        addGap(result, *resolved,
               "origin-local reply ID retained; global reply URI unresolved");
      const auto handle = stringField(*account, "acct").value_or("");
      const auto displayName =
          stringField(*account, "display_name").value_or("");
      const auto profileUrl = stringField(*account, "url").value_or("");
      const auto oversized  = [&](std::string_view field) {
        return field.size() > request.limits.records.maxStringBytes;
      };
      if (oversized(*restId) || oversized(*actorRestId) || oversized(warning) ||
          oversized(handle) || oversized(displayName) ||
          oversized(profileUrl) ||
          (object.inReplyToUri && oversized(*object.inReplyToUri)) ||
          object.audience.size() > request.limits.records.maxRecords ||
          std::ranges::any_of(object.audience, oversized)) {
        addGap(result, *resolved, "status metadata exceeds configured limits");
        break;
      }
      const auto actorExists = std::ranges::any_of(
          result.observation.actors,
          [&](const auto &existing) { return existing.actorUri == *actorUri; });
      if (1 + (actorCount + !actorExists) * 2 + (objectCount + 1) * 3 >
          request.limits.records.maxRecords) {
        result.error = "status record limit reached";
        addGap(result, *resolved, result.error);
        break;
      }
      if (object.content.size() > request.limits.records.maxStringBytes ||
          object.objectUri.size() > request.limits.records.maxStringBytes) {
        result.error = "status field exceeds string budget";
        addGap(result, *resolved, result.error);
        break;
      }
      if (!actorExists) {
        result.observation.actors.push_back(
            {.actorUri      = *actorUri,
             .accountId     = *actorRestId,
             .handle        = handle,
             .displayName   = displayName,
             .identityBasis = "configured-account-correspondence",
             .profileUrl    = profileUrl});
        ++actorCount;
      }
      result.observation.objects.push_back(std::move(object));
      ++objectCount;
    }
    if (result.observation.state == CaptureState::Partial) break;
    bool malformed{};
    const auto candidate = nextLink(response.receive.links, malformed);
    if (malformed) {
      result.error = "malformed pagination Link header";
      addGap(result, *resolved, result.error);
      break;
    }
    if (!candidate) {
      outstanding = false;
      break;
    }
    if (request.authenticated &&
        credentialInLocator(*candidate, credential->token_)) {
      result.error = "pagination target withheld by credential policy";
      addGap(result, *resolved, result.error);
      break;
    }
    const auto safeNext = sameOriginUrl(*origin, *candidate);
    if (!safeNext || !isStatusesEndpoint(*safeNext, request.accountId)) {
      result.error = "refused unsafe pagination target";
      addGap(result, *resolved, result.error);
      break;
    }
    next = *candidate;
  }
  if (pages >= request.limits.maxPages && outstanding &&
      result.observation.state == CaptureState::Complete) {
    // A next link may remain; at the page cap coverage is never claimed
    // complete.
    result.observation.state = CaptureState::Partial;
    result.error = "page limit reached; additional source coverage is unknown";
    result.gaps.push_back({next, result.error});
  }
  result.observation.coverage = std::to_string(pages) + " page(s); " +
                                std::to_string(objectCount) +
                                " status record(s) retained";
  return result;
}

FediversePostResult FediverseTransport::post(
    const FediversePostRequest &request,
    const std::shared_ptr<FediverseCredentialLease> &credential,
    const std::shared_ptr<FediverseAuthority> &authority,
    const std::atomic_bool &cancel) const {
  FediversePostResult result{.state          = CaptureState::Failed,
                             .httpStatus     = 0,
                             .accepted       = false,
                             .restId         = {},
                             .objectUri      = {},
                             .actorUri       = {},
                             .activityUri    = {},
                             .idempotencyKey = request.idempotencyKey,
                             .visibility     = request.visibility,
                             .audience       = request.audience,
                             .error          = {},
                             .federationDeliveryConfirmed = false};
  const auto origin =
      parseOrigin(request.instanceOrigin, policy_.allowLoopbackHttpForTests);
  if (const auto problem = fediversePostProblem(request, policy_)) {
    result.error = *problem;
    return result;
  }
  if (!credential || !authority) {
    result.error = "missing post authority or credential lease";
    return result;
  }
  const auto fingerprint = fediverseRequestFingerprint(request);
  if (!authorized(*authority, *credential, *origin, request.accountId,
                  FediverseAction::PublishStatus, fingerprint, request.audience,
                  cancel)) {
    result.state =
        cancel.load() ? CaptureState::Cancelled : CaptureState::Failed;
    result.error = "post authority or credential lease is unavailable";
    return result;
  }
  const std::string endpoint = origin->text + "/api/v1/statuses";
  std::string body           = "status=" + urlEncode(request.text) +
                     "&visibility=" + urlEncode(request.visibility) +
                     "&spoiler_text=" + urlEncode(request.contentWarning);
  if (request.replyToRestId)
    body += "&in_reply_to_id=" + urlEncode(*request.replyToRestId);
  const auto remaining = std::min(request.limits.records.maxBytes,
                                  request.limits.records.maxStringBytes);
  const auto response  = perform(
      endpoint, &credential->token_, &body, &request.idempotencyKey, remaining,
      request.limits.timeoutMs, cancel, Clock::time_point::max(),
      [&] {
        return authorized(*authority, *credential, *origin, request.accountId,
                           FediverseAction::PublishStatus, fingerprint,
                           request.audience, cancel);
      },
      request.limits.maxHeaderBytes);
  result.httpStatus      = response.status;
  result.mayHaveExecuted = response.started;
  auto retained = retainedResponse(response.receive.body, &credential->token_);
  result.responseBody     = std::move(retained.first);
  result.responseRedacted = retained.second;
  if (response.receive.preflightRefused) {
    result.state =
        response.started ? CaptureState::Partial : CaptureState::Failed;
    result.error =
        response.started
            ? "post authority revoked; server acceptance unknown"
            : "post authority or credential lease expired before request";
    return result;
  }
  if (response.result != CURLE_OK) {
    if (cancel.load()) {
      result.state = CaptureState::Cancelled;
      result.error = "post cancelled; server acceptance is unknown";
    } else if (response.receive.timedOut) {
      result.state = CaptureState::Partial;
      result.error = "post deadline exceeded; server acceptance is unknown";
    } else if (response.receive.tooLarge) {
      result.state = CaptureState::Partial;
      result.error =
          "post response exceeded its byte budget; acceptance unknown";
    } else {
      result.state = CaptureState::Failed;
      result.error = "post transport failed; server acceptance is unknown";
    }
    return result;
  }
  if (response.status >= 300 && response.status < 400) {
    result.state = CaptureState::Failed;
    result.error = "HTTP redirects are refused; server acceptance unknown";
    return result;
  }
  if (response.status < 200 || response.status >= 300) {
    result.state = CaptureState::Failed;
    result.error =
        "Fediverse post returned HTTP " + std::to_string(response.status);
    return result;
  }
  result.accepted                    = true;
  result.state                       = CaptureState::Complete;
  result.federationDeliveryConfirmed = false;
  boost::system::error_code parseError;
  const auto document = boost::json::parse(result.responseBody, parseError);
  if (parseError || !document.is_object()) {
    result.state = CaptureState::Partial;
    result.error =
        "REST accepted the request, but the status response was not parseable";
    return result;
  }
  result.restId    = stringField(document, "id").value_or("");
  result.objectUri = stringField(document, "uri");
  if (const auto *account = document.as_object().if_contains("account");
      account && account->is_object() &&
      stringField(*account, "id") == request.accountId) {
    result.actorUri           = request.accountActorUri;
    result.actorIdentityBasis = "configured-account-correspondence";
  }
  if (result.restId.empty() || !result.objectUri || !result.actorUri) {
    result.state = CaptureState::Partial;
    result.error =
        "REST accepted the request but returned incomplete status identity";
  }
  // Mastodon status responses identify the REST status and ActivityPub object,
  // not a separately addressable Create activity or remote delivery receipts.
  return result;
}

} // namespace xanadu::evidence
