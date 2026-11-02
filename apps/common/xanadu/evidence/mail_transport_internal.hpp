#ifndef XANADU_EVIDENCE_MAIL_TRANSPORT_INTERNAL_HPP
#define XANADU_EVIDENCE_MAIL_TRANSPORT_INTERNAL_HPP

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <string_view>

#include <curl/curl.h>

#include "common/xanadu/evidence/mail_transport.hpp"

namespace xanadu::evidence {

struct MailCredentialLease::State {
  MailCredentialScope scope;
  std::string username;
  std::string password;
  std::chrono::steady_clock::time_point expiresAt;
  std::atomic_bool revoked{};
  mutable std::mutex mutex;

  ~State();
  [[nodiscard]] bool usable() noexcept;
  void revoke() noexcept;
};

struct MailCredentialSecret {
  std::string username;
  std::string password;
  MailCredentialSecret() = default;
  MailCredentialSecret(std::string user, std::string secret);
  ~MailCredentialSecret();
  MailCredentialSecret(const MailCredentialSecret &)            = delete;
  MailCredentialSecret &operator=(const MailCredentialSecret &) = delete;
  MailCredentialSecret(MailCredentialSecret &&other) noexcept;
  MailCredentialSecret &operator=(MailCredentialSecret &&other) noexcept;
};

struct MailCredentialInternalAccess {
  [[nodiscard]] static std::shared_ptr<MailCredentialLease::State>
  state(const MailCredentialLease &lease);
  [[nodiscard]] static std::optional<MailCredentialSecret>
  secret(const MailCredentialLease &lease);
};

struct MailEndpoint {
  std::string url;
  std::string authority;
  std::string scheme;
  std::string host;
  std::string port;
  bool secure{};
};

[[nodiscard]] std::optional<MailEndpoint>
canonicalMailEndpoint(std::string_view url, std::string_view secureScheme,
                      std::string_view clearScheme);
void initializeMailCurl();
[[nodiscard]] CURLcode
performMailCurlBounded(CURL *easy, const std::atomic_bool &cancel,
                       std::chrono::steady_clock::time_point deadline,
                       const std::function<bool()> &preflight = {},
                       CURLM *sharedMulti = nullptr, bool keepAttached = false);
[[nodiscard]] bool isLiteralLoopback(std::string_view host);
[[nodiscard]] bool validMailToken(std::string_view value) noexcept;

class CurlProgressContext {
public:
  CurlProgressContext(const std::atomic_bool &cancel,
                      std::chrono::steady_clock::time_point deadline,
                      std::function<bool()> preflight = {});

  [[nodiscard]] static int callback(void *context, curl_off_t downloadTotal,
                                    curl_off_t downloaded,
                                    curl_off_t uploadTotal,
                                    curl_off_t uploaded) noexcept;

private:
  const std::atomic_bool &cancel_;
  std::chrono::steady_clock::time_point deadline_;
  std::function<bool()> preflight_;
};

class MailSigpipeBlock {
public:
  MailSigpipeBlock();
  ~MailSigpipeBlock();
  MailSigpipeBlock(const MailSigpipeBlock &)            = delete;
  MailSigpipeBlock &operator=(const MailSigpipeBlock &) = delete;

private:
  sigset_t set_{};
  sigset_t previous_{};
  bool active_{};
  bool wasPending_{};
};

} // namespace xanadu::evidence

#endif
