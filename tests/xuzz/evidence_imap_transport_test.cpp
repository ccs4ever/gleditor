#include "common/xanadu/evidence/communications.hpp"
#include "common/xanadu/evidence/mail_read_job.hpp"
#include "common/xanadu/evidence/mail_transport.hpp"
#include "common/xanadu/evidence/mail_transport_internal.hpp"
#include "common/xanadu/user_permascroll.hpp"

#include <gtest/gtest.h>

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace xanadu::evidence {
namespace {
using namespace std::chrono_literals;

struct Io {
  int fd{-1};
  SSL *tls{};

  std::string line() const {
    std::string result;
    while (result.size() < 16384) {
      char value{};
      int count{};
      if (tls) {
        MailSigpipeBlock sigpipe;
        count = SSL_read(tls, &value, 1);
      } else {
        count = static_cast<int>(::recv(fd, &value, 1, 0));
      }
      if (count != 1) return {};
      result.push_back(value);
      if (result.ends_with("\r\n")) return result;
    }
    return {};
  }

  void send(std::string_view value) const {
    std::size_t sent{};
    while (sent < value.size()) {
      int count{};
      if (tls) {
        MailSigpipeBlock sigpipe;
        count = SSL_write(tls, value.data() + sent,
                          static_cast<int>(value.size() - sent));
      } else {
        count = static_cast<int>(
            ::send(fd, value.data() + sent, value.size() - sent, MSG_NOSIGNAL));
      }
      if (count <= 0) return;
      sent += static_cast<std::size_t>(count);
    }
  }
};

class LoopbackImapServer {
public:
  LoopbackImapServer() {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("loopback socket failed");
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port        = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0)
      throw std::runtime_error("loopback listener setup failed");
    socklen_t size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0)
      throw std::runtime_error("loopback port lookup failed");
    port_ = ntohs(address.sin_port);
  }

  ~LoopbackImapServer() {
    if (client_.load() >= 0) {
      (void)::shutdown(client_.load(), SHUT_RDWR);
      (void)::close(client_.load());
    }
    if (listener_ >= 0) (void)::close(listener_);
    if (thread_.joinable()) thread_.join();
  }

  std::uint16_t port() const { return port_; }

  void join() {
    if (thread_.joinable()) thread_.join();
  }

  void start(std::function<void(Io &)> handler, SSL_CTX *context = nullptr) {
    thread_ = std::thread([this, handler = std::move(handler), context] {
      pollfd listener{.fd = listener_, .events = POLLIN};
      if (::poll(&listener, 1, 2000) <= 0) return;
      client_ = ::accept(listener_, nullptr, nullptr);
      if (client_.load() < 0) return;
      timeval timeout{2, 0};
      (void)::setsockopt(client_.load(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                         sizeof(timeout));
      Io io{.fd = client_.load()};
      if (context) {
        io.tls = SSL_new(context);
        int accepted{};
        if (io.tls && SSL_set_fd(io.tls, io.fd) == 1) {
          MailSigpipeBlock sigpipe;
          accepted = SSL_accept(io.tls);
        }
        if (!io.tls || accepted != 1) {
          if (io.tls) SSL_free(io.tls);
          return;
        }
      }
      handler(io);
      if (io.tls) {
        {
          MailSigpipeBlock sigpipe;
          SSL_shutdown(io.tls);
        }
        SSL_free(io.tls);
      }
    });
  }

private:
  int listener_{-1};
  std::atomic_int client_{-1};
  std::uint16_t port_{};
  std::thread thread_;
};

struct TestCertificate {
  EVP_PKEY *key{};
  X509 *certificate{};
  SSL_CTX *context{};
  std::filesystem::path path;

  TestCertificate() {
    EVP_PKEY_CTX *rawKey = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (!rawKey) throw std::runtime_error("key context creation failed");
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
        rawKey, EVP_PKEY_CTX_free);
    if (EVP_PKEY_keygen_init(keyContext.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(),
                                               NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(keyContext.get(), &key) <= 0)
      throw std::runtime_error("test key generation failed");
    certificate = X509_new();
    if (!certificate || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), 42) != 1 ||
        !X509_gmtime_adj(X509_get_notBefore(certificate), -60) ||
        !X509_gmtime_adj(X509_get_notAfter(certificate), 3600) ||
        X509_set_pubkey(certificate, key) != 1)
      throw std::runtime_error("test certificate setup failed");
    auto *name = X509_get_subject_name(certificate);
    if (X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1,
            0) != 1 ||
        X509_set_issuer_name(certificate, name) != 1)
      throw std::runtime_error("test certificate subject failed");
    X509V3_CTX extensionContext{};
    X509V3_set_ctx(&extensionContext, certificate, certificate, nullptr,
                   nullptr, 0);
    auto *san         = X509V3_EXT_conf_nid(nullptr, &extensionContext,
                                            NID_subject_alt_name, "IP:127.0.0.1");
    auto *constraints = X509V3_EXT_conf_nid(
        nullptr, &extensionContext, NID_basic_constraints, "critical,CA:TRUE");
    if (!san || !constraints || X509_add_ext(certificate, san, -1) != 1 ||
        X509_add_ext(certificate, constraints, -1) != 1 ||
        X509_sign(certificate, key, EVP_sha256()) <= 0)
      throw std::runtime_error("test certificate signing failed");
    X509_EXTENSION_free(san);
    X509_EXTENSION_free(constraints);
    context = SSL_CTX_new(TLS_server_method());
    if (!context || SSL_CTX_use_certificate(context, certificate) != 1 ||
        SSL_CTX_use_PrivateKey(context, key) != 1)
      throw std::runtime_error("test TLS context setup failed");

    path = std::filesystem::temp_directory_path() /
           ("imap-ca-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
            ".pem");
    BIO *rawBio = BIO_new(BIO_s_mem());
    if (!rawBio || PEM_write_bio_X509(rawBio, certificate) != 1)
      throw std::runtime_error("test certificate serialization failed");
    BUF_MEM *memory{};
    BIO_get_mem_ptr(rawBio, &memory);
    {
      std::ofstream output(path, std::ios::binary);
      output.write(memory->data, static_cast<std::streamsize>(memory->length));
    }
    BIO_free(rawBio);
  }

  ~TestCertificate() {
    if (context) SSL_CTX_free(context);
    if (certificate) X509_free(certificate);
    if (key) EVP_PKEY_free(key);
    std::error_code error;
    std::filesystem::remove(path, error);
  }
};

std::string rawMessage(unsigned number) {
  return "From: sender" + std::to_string(number) + "@example.test\r\n" +
         "To: reader@example.test\r\n" + "Subject: fixture " +
         std::to_string(number) + "\r\n" + "Message-ID: <fixture-" +
         std::to_string(number) + "@example.test>\r\n" +
         "MIME-Version: 1.0\r\n" +
         "Content-Type: text/plain; charset=UTF-8\r\n\r\n" +
         "controlled message " + std::to_string(number) + "\r\n";
}

std::string observationTrace(const MailObservation &observation) {
  std::string trace =
      "state=" + std::to_string(static_cast<int>(observation.state));
  for (const auto &gap : observation.gaps)
    trace += "; gap=" + gap.label + ":" + gap.detail;
  return trace;
}

std::string tagOf(std::string_view line) {
  const auto end = line.find(' ');
  return std::string(line.substr(0, end));
}

std::string commandOf(std::string_view line) {
  const auto first = line.find(' ');
  if (first == std::string_view::npos) return {};
  const auto end = line.find_first_of(" \r\n", first + 1);
  return std::string(line.substr(first + 1, end - first - 1));
}

void serveReadOnlyMailbox(
    Io &io, const std::uint64_t uidValidity,
    const std::vector<std::uint32_t> &uids, std::vector<std::string> &commands,
    std::atomic_bool *holdCapability              = nullptr,
    std::atomic_bool *releaseCapability           = nullptr,
    std::optional<std::uint32_t> fetchUidOverride = {},
    bool omitFetchUid = false, bool truncateFetch = false,
    std::size_t oversizedLiteralBytes = 0, std::string echoedCredential = {},
    std::string echoedFlag = {}, bool preauthenticated = true,
    bool streamOversizedLiteralPrefix = false, bool duplicateUid = false,
    bool duplicateFlags = false, bool duplicateBody = false) {
  io.send(preauthenticated ? "* PREAUTH controlled fixture\r\n"
                           : "* OK IMAP4rev1 controlled fixture\r\n");
  for (int iteration = 0; iteration < 24; ++iteration) {
    const auto line = io.line();
    if (line.empty()) return;
    commands.push_back(line);
    const auto tag     = tagOf(line);
    const auto command = commandOf(line);
    if (command == "CAPABILITY") {
      if (holdCapability) {
        holdCapability->store(true);
        while (!releaseCapability->load()) std::this_thread::sleep_for(1ms);
      }
      io.send("* CAPABILITY IMAP4rev1 AUTH=PLAIN\r\n" + tag +
              " OK CAPABILITY completed\r\n");
    } else if (command == "EXAMINE") {
      io.send("* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)\r\n"
              "* 2 EXISTS\r\n"
              "* 0 RECENT\r\n* OK [UIDVALIDITY " +
              std::to_string(uidValidity) + "] UIDs valid\r\n" + tag +
              " OK [READ-ONLY] EXAMINE completed\r\n");
    } else if (command == "UID") {
      if (line.find("SEARCH ALL") != std::string::npos) {
        std::string response = "* SEARCH";
        for (const auto uid : uids) response += " " + std::to_string(uid);
        response += "\r\n" + tag + " OK SEARCH completed\r\n";
        io.send(response);
      } else if (line.find("FETCH") != std::string::npos) {
        std::uint32_t uid{};
        const auto fetchPosition = line.find("FETCH ");
        if (fetchPosition == std::string::npos) {
          io.send(tag + " BAD unsupported UID command\r\n");
          continue;
        }
        const auto start = fetchPosition + 6;
        const auto end   = line.find(' ', start);
        const auto [last, error] =
            std::from_chars(line.data() + start, line.data() + end, uid);
        if (error != std::errc{} || last != line.data() + end ||
            line.find("BODY.PEEK[]") == std::string::npos) {
          io.send(tag + " BAD expected BODY.PEEK\r\n");
          continue;
        }
        auto message = rawMessage(uid);
        if (!echoedCredential.empty())
          message += "\r\n" + echoedCredential + "\r\n";
        const auto literalLength =
            oversizedLiteralBytes ? oversizedLiteralBytes : message.size();
        auto metadata = std::string("* 1 FETCH (");
        if (!omitFetchUid) {
          const auto uidAttribute =
              "UID " + std::to_string(fetchUidOverride.value_or(uid)) + " ";
          metadata += uidAttribute;
          if (duplicateUid) metadata += uidAttribute;
        }
        metadata +=
            "FLAGS (" + (echoedFlag.empty() ? "\\Seen" : echoedFlag) + ") ";
        if (duplicateFlags) metadata += "FLAGS (\\Seen) ";
        metadata += "BODY[] {" + std::to_string(literalLength) + "}\r\n";
        if (oversizedLiteralBytes) {
          io.send(metadata);
          if (streamOversizedLiteralPrefix) {
            // The transport cap is 512 bytes in the adversarial fixture. The
            // parser callback must stop incrementally before retaining byte
            // 513.
            io.send(std::string(513, 'x'));
            io.send(")\r\n" + tag + " OK FETCH completed\r\n");
            return;
          }
          continue;
        }
        auto response =
            metadata +
            (truncateFetch ? message.substr(0, message.size() - 4) : message);
        if (duplicateBody)
          response +=
              " BODY[] {" + std::to_string(message.size()) + "}\r\n" + message;
        response += ")\r\n" + tag + " OK FETCH completed\r\n";
        io.send(response);
      } else {
        io.send(tag + " BAD unsupported UID command\r\n");
      }
    } else if (command == "AUTHENTICATE") {
      io.send("+ \r\n");
      const auto auth = io.line();
      commands.push_back(auth);
      io.send(tag + " OK authenticated\r\n");
    } else if (command == "LOGIN") {
      io.send(tag + " OK authenticated\r\n");
    } else if (command == "LOGOUT") {
      io.send("* BYE logout\r\n" + tag + " OK logout completed\r\n");
      return;
    } else {
      io.send(tag + " BAD unexpected command\r\n");
    }
  }
}

ImapReadProfile profileFor(const std::uint16_t port) {
  const auto endpoint = "imap://127.0.0.1:" + std::to_string(port);
  return {.serverUrl                                = endpoint,
          .serverAuthority                          = endpoint,
          .accountScope                             = "fixture-account",
          .mailbox                                  = "INBOX",
          .allowCleartextLoopbackWithoutCredentials = true};
}

TEST(EvidenceImapTransportTest, UsesReadOnlyExamineUidWitnessAndBodyPeek) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) { serveReadOnlyMailbox(io, 77, {1, 2}, commands); });
  std::atomic_bool cancel{};
  auto profile = profileFor(server.port());
  auto result  = readImapMailbox(profile, {}, MailCredentialLease{}, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(result));
  EXPECT_EQ(result.state, CaptureState::Complete);
  EXPECT_EQ(result.serverAuthority, profile.serverAuthority);
  EXPECT_EQ(result.accountScope, profile.accountScope);
  EXPECT_EQ(result.mailbox, "INBOX");
  EXPECT_NE(result.mailboxIncarnation.find("INBOX"), std::string::npos);
  ASSERT_EQ(result.messages.size(), 2U);
  EXPECT_EQ(result.messages[0].uid, 1U);
  EXPECT_EQ(result.messages[0].uidValidity, 77U);
  EXPECT_EQ(result.messages[0].sequenceNumber, 1U);
  EXPECT_EQ(result.messages[0].rawMessage, rawMessage(1));
  EXPECT_EQ(result.messages[0].subject, "fixture 1");
  ASSERT_TRUE(result.messages[0].flags.has_value());
  EXPECT_EQ(*result.messages[0].flags, std::vector<std::string>{"\\Seen"});
  for (const auto &command : commands) {
    EXPECT_EQ(command.find("SELECT"), std::string::npos);
    EXPECT_EQ(command.find("STORE"), std::string::npos);
    if (command.find("FETCH") != std::string::npos)
      EXPECT_NE(command.find("BODY.PEEK[]"), std::string::npos);
  }
  ASSERT_FALSE(commands.empty());
  EXPECT_NE(commands.front().find("CAPABILITY"), std::string::npos);
  EXPECT_TRUE(std::ranges::any_of(commands, [](const auto &command) {
    return command.find("EXAMINE") != std::string::npos;
  }));
}

TEST(EvidenceImapTransportTest,
     TwoMessagesCanConsumeTheExactAggregateByteBudget) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) { serveReadOnlyMailbox(io, 77, {1, 2}, commands); });
  auto profile = profileFor(server.port());
  ImapReadRequest request;
  request.maxTotalBytes = rawMessage(1).size() + rawMessage(2).size();
  std::atomic_bool cancel{};
  const auto observation =
      readImapMailbox(profile, request, MailCredentialLease{}, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Complete);
  ASSERT_EQ(observation.messages.size(), 2U);
  EXPECT_EQ(observation.messages[0].rawMessage, rawMessage(1));
  EXPECT_EQ(observation.messages[1].rawMessage, rawMessage(2));
}

TEST(EvidenceImapTransportTest, RejectsDuplicateCriticalFetchAttributes) {
  const auto expectRejected = [](const bool duplicateUid,
                                 const bool duplicateFlags,
                                 const bool duplicateBody) {
    LoopbackImapServer server;
    std::vector<std::string> commands;
    server.start([&](Io &io) {
      serveReadOnlyMailbox(io, 77, {1}, commands, nullptr, nullptr, {}, false,
                           false, 0, {}, {}, true, false, duplicateUid,
                           duplicateFlags, duplicateBody);
    });
    auto profile = profileFor(server.port());
    std::atomic_bool cancel{};
    const auto observation =
        readImapMailbox(profile, {}, MailCredentialLease{}, cancel);
    server.join();
    SCOPED_TRACE(observationTrace(observation));
    EXPECT_EQ(observation.state, CaptureState::Partial);
    EXPECT_TRUE(observation.messages.empty());
    EXPECT_TRUE(std::ranges::any_of(observation.gaps, [](const MailGap &gap) {
      return gap.label == "FETCH";
    }));
    if (duplicateUid || duplicateFlags) {
      ASSERT_FALSE(observation.gaps.empty());
      EXPECT_NE(
          observation.gaps.front().detail.find("duplicate critical FETCH"),
          std::string::npos);
    }
  };

  expectRejected(true, false, false);
  expectRejected(false, true, false);
  expectRejected(false, false, true);
}

TEST(EvidenceImapTransportTest, RejectsUnmatchedOrIncompleteFetchFrames) {
  const auto expectRejected = [](const std::optional<std::uint32_t> uid,
                                 const bool omitUid, const bool truncate) {
    LoopbackImapServer server;
    std::vector<std::string> commands;
    server.start([&](Io &io) {
      serveReadOnlyMailbox(io, 77, {1}, commands, nullptr, nullptr, uid,
                           omitUid, truncate);
    });
    auto profile = profileFor(server.port());
    std::atomic_bool cancel{};
    const auto observation =
        readImapMailbox(profile, {}, MailCredentialLease{}, cancel);
    server.join();
    SCOPED_TRACE(observationTrace(observation));
    EXPECT_EQ(observation.state, CaptureState::Partial);
    EXPECT_TRUE(observation.messages.empty());
    EXPECT_TRUE(std::ranges::any_of(observation.gaps, [](const MailGap &gap) {
      return gap.label == "FETCH";
    }));
  };

  expectRejected(2, false, false);
  expectRejected({}, true, false);
  expectRejected({}, false, true);
}

TEST(EvidenceImapTransportTest,
     RejectsStreamedLiteralWhenIncrementalByteCapIsExceeded) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) {
    serveReadOnlyMailbox(io, 77, {1}, commands, nullptr, nullptr, {}, false,
                         false, 513, {}, {}, true, true);
  });
  auto profile = profileFor(server.port());
  ImapReadRequest request;
  request.maxMessageBytes                = 512;
  request.maxTotalBytes                  = 4096;
  request.timeoutMilliseconds            = 1500;
  request.retentionBounds.maxStringBytes = 4096;
  request.retentionBounds.maxBytes       = 8192;
  std::atomic_bool cancel{};
  const auto started = std::chrono::steady_clock::now();
  const auto observation =
      readImapMailbox(profile, request, MailCredentialLease{}, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Partial);
  EXPECT_TRUE(observation.messages.empty());
  EXPECT_TRUE(std::ranges::any_of(observation.gaps, [](const MailGap &gap) {
    return gap.label == "FETCH" &&
           gap.detail.find("exceeded its configured bounds") !=
               std::string::npos;
  }));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
}

TEST(EvidenceImapTransportTest,
     MarkerOnlyLiteralStopsAtDeadlineWithoutReturningPartialBytes) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) {
    serveReadOnlyMailbox(io, 77, {1}, commands, nullptr, nullptr, {}, false,
                         false, 16 * 1024 * 1024);
  });
  auto profile = profileFor(server.port());
  ImapReadRequest request;
  request.maxMessageBytes                = 512;
  request.maxTotalBytes                  = 4096;
  request.timeoutMilliseconds            = 350;
  request.retentionBounds.maxStringBytes = 4096;
  request.retentionBounds.maxBytes       = 8192;
  std::atomic_bool cancel{};
  const auto started = std::chrono::steady_clock::now();
  const auto observation =
      readImapMailbox(profile, request, MailCredentialLease{}, cancel);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Partial);
  EXPECT_TRUE(observation.messages.empty());
  EXPECT_TRUE(std::ranges::any_of(commands, [](const std::string &command) {
    return command.find(" FETCH ") != std::string::npos;
  }));
  EXPECT_TRUE(std::ranges::any_of(observation.gaps, [](const MailGap &gap) {
    return gap.label == "FETCH";
  }));
  EXPECT_LT(elapsed, 750ms);
}

TEST(EvidenceImapTransportTest, AsyncReadOwnsResultAndRefusesSecondStart) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) { serveReadOnlyMailbox(io, 77, {1}, commands); });
  auto profile = profileFor(server.port());
  MailReadJob job;
  EXPECT_TRUE(job.start(profile, {}, MailCredentialLease{}));
  EXPECT_FALSE(job.start(profile, {}, MailCredentialLease{}));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (job.busy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  EXPECT_FALSE(job.busy());
  EXPECT_FALSE(job.start(profile, {}, MailCredentialLease{}));
  auto completion = job.poll();
  server.join();
  ASSERT_TRUE(completion.has_value());
  EXPECT_FALSE(completion->cancelled);
  EXPECT_EQ(completion->observation.state, CaptureState::Complete);
  ASSERT_EQ(completion->observation.messages.size(), 1U);
  EXPECT_EQ(completion->observation.messages.front().rawMessage, rawMessage(1));
}

TEST(EvidenceImapTransportTest, AsyncReadCanBeCancelledAndPolled) {
  LoopbackImapServer server;
  std::atomic_bool waiting{};
  std::atomic_bool release{};
  std::vector<std::string> commands;
  server.start([&](Io &io) {
    serveReadOnlyMailbox(io, 77, {1}, commands, &waiting, &release);
  });
  auto profile = profileFor(server.port());
  MailReadJob job;
  const bool started = job.start(profile, {}, MailCredentialLease{});
  for (int i = 0; i < 500 && !waiting.load(); ++i)
    std::this_thread::sleep_for(1ms);
  const bool observedWaiting = waiting.load();
  job.cancel();
  const auto deadline = std::chrono::steady_clock::now() + 500ms;
  while (job.busy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  auto completion = job.poll();
  release.store(true);
  server.join();
  EXPECT_TRUE(started);
  EXPECT_TRUE(observedWaiting);
  EXPECT_FALSE(job.busy());
  ASSERT_TRUE(completion.has_value());
  EXPECT_TRUE(completion->cancelled);
  EXPECT_NE(completion->observation.state, CaptureState::Complete);
}

TEST(EvidenceImapTransportTest, AsyncReadCancelAndDestructorJoinAreBounded) {
  LoopbackImapServer server;
  std::atomic_bool waiting{};
  std::atomic_bool release{};
  std::vector<std::string> commands;
  server.start([&](Io &io) {
    serveReadOnlyMailbox(io, 77, {1}, commands, &waiting, &release);
  });
  auto profile       = profileFor(server.port());
  const auto started = std::chrono::steady_clock::now();
  {
    MailReadJob job;
    EXPECT_TRUE(job.start(profile, {}, MailCredentialLease{}));
    for (int i = 0; i < 500 && !waiting.load(); ++i)
      std::this_thread::sleep_for(1ms);
    job.cancel();
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  release.store(true);
  server.join();
  EXPECT_TRUE(waiting.load());
  EXPECT_LT(elapsed, 500ms);
}

TEST(EvidenceImapTransportTest,
     NativeReadCanBeRetainedAnnotatedAndReopenedWithinBounds) {
  LoopbackImapServer server;
  std::vector<std::string> commands;
  server.start([&](Io &io) { serveReadOnlyMailbox(io, 77, {1}, commands); });
  auto profile = profileFor(server.port());
  std::atomic_bool cancel{};
  auto observation =
      readImapMailbox(profile, {}, MailCredentialLease{}, cancel);
  server.join();
  ASSERT_EQ(observation.state, CaptureState::Complete);
  ASSERT_EQ(observation.messages.size(), 1U);

  const auto directory =
      std::filesystem::temp_directory_path() /
      ("imap-retain-" + xanadu::UserPermascroll::generateUniqueDeviceId());
  std::filesystem::create_directories(directory);
  auto scroll = std::make_shared<xanadu::UserPermascroll>();
  EvidenceCase evidence(scroll);
  evidence.create(directory / "case", "IMAP retained evidence");
  const auto retained = retainMailObservation(evidence, observation);
  const auto message  = retained.records.at("mail:77:1");
  const auto raw      = retained.records.at("mail:77:1:raw");
  const auto note = evidence.annotate(message, "Review this retained message");
  evidence.durableSave();
  evidence.close();
  evidence.open(directory / "case");
  EXPECT_EQ(evidence.rawText(raw), rawMessage(1));
  EXPECT_EQ(evidence.inspect(note).text, "Review this retained message");
  EXPECT_EQ(evidence.inspect(message).attributes.at("mailbox-incarnation"),
            observation.mailboxIncarnation);
  evidence.close();
  std::filesystem::remove_all(directory);
}

TEST(EvidenceImapTransportTest, RetentionBoundsAreIndependentOfMessageCount) {
  LoopbackImapServer server;
  std::atomic_bool accepted{};
  server.start([&](Io &) { accepted.store(true); });
  auto profile = profileFor(server.port());
  ImapReadRequest request;
  request.maxMessages                = 1;
  request.retentionBounds.maxRecords = 4;
  std::atomic_bool cancel{};
  const auto observation =
      readImapMailbox(profile, request, MailCredentialLease{}, cancel);
  EXPECT_EQ(observation.state, CaptureState::Failed);
  EXPECT_FALSE(accepted.load());
}

TEST(EvidenceImapTransportTest, CanonicalAuthorityPreservesIpv6Brackets) {
  EXPECT_EQ(imapServerAuthority("imaps://[::1]:993"),
            std::optional<std::string>{"imaps://[::1]:993"});
  EXPECT_EQ(imapServerAuthority("imap://[::1]:143"),
            std::optional<std::string>{"imap://[::1]:143"});
}

TEST(EvidenceImapTransportTest, CredentialLeaseRejectsImapLineBreaks) {
  MailCredentialBroker broker;
  const MailCredentialScope scope{.serverAuthority = "imaps://mail.example:993",
                                  .accountScope    = "mail-account",
                                  .operation = MailCredentialOperation::Read};
  EXPECT_THROW(
      static_cast<void>(broker.issue(scope, "alice\r\nLOGOUT", "secret", 1min)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(broker.issue(scope, "alice", "secret\r\nLOGOUT", 1min)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(broker.issue(scope, "alice\nB", "secret", 1min)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(broker.issue(scope, "alice", "secret\nB", 1min)),
      std::invalid_argument);
}

TEST(EvidenceImapTransportTest, CursorRequiresUidValidityAndResetsOnNewEpoch) {
  LoopbackImapServer unboundServer;
  std::vector<std::string> unboundCommands;
  unboundServer.start(
      [&](Io &io) { serveReadOnlyMailbox(io, 77, {1, 2}, unboundCommands); });
  auto profile = profileFor(unboundServer.port());
  std::atomic_bool cancel{};
  ImapReadRequest unbound{.afterUid = 2};
  auto rejected =
      readImapMailbox(profile, unbound, MailCredentialLease{}, cancel);
  unboundServer.join();
  EXPECT_EQ(rejected.state, CaptureState::Partial);
  EXPECT_TRUE(rejected.messages.empty());
  ASSERT_FALSE(rejected.gaps.empty());
  EXPECT_NE(rejected.gaps.back().detail.find("UIDVALIDITY witness"),
            std::string::npos);

  LoopbackImapServer oldEpoch;
  std::vector<std::string> oldCommands;
  oldEpoch.start(
      [&](Io &io) { serveReadOnlyMailbox(io, 77, {1, 2}, oldCommands); });
  auto oldProfile = profileFor(oldEpoch.port());
  auto old = readImapMailbox(oldProfile, {}, MailCredentialLease{}, cancel);
  oldEpoch.join();
  ASSERT_EQ(old.state, CaptureState::Complete);

  LoopbackImapServer newEpoch;
  std::vector<std::string> newCommands;
  newEpoch.start(
      [&](Io &io) { serveReadOnlyMailbox(io, 78, {1, 2}, newCommands); });
  auto newProfile = profileFor(newEpoch.port());
  auto request    = ImapReadRequest{.afterUid = 2, .afterUidValidity = 77};
  auto next =
      readImapMailbox(newProfile, request, MailCredentialLease{}, cancel);
  newEpoch.join();
  EXPECT_NE(old.mailboxIncarnation, next.mailboxIncarnation);
  ASSERT_FALSE(next.gaps.empty());
  EXPECT_NE(next.gaps.front().detail.find("incarnation changed"),
            std::string::npos);
  ASSERT_EQ(next.messages.size(), 2U);
  EXPECT_EQ(next.messages.front().uid, 1U);
}

TEST(EvidenceImapTransportTest, CleartextCredentialsAndUserInfoAreRefused) {
  LoopbackImapServer server;
  std::atomic_bool accepted{};
  server.start([&](Io &) { accepted = true; });
  auto profile = profileFor(server.port());
  MailCredentialBroker broker;
  const auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                                   .accountScope    = profile.accountScope,
                                   .operation = MailCredentialOperation::Read},
                                  "alice", "test-secret", 1min);
  std::atomic_bool cancel{};
  auto withCredential = readImapMailbox(profile, {}, lease, cancel);
  EXPECT_EQ(withCredential.state, CaptureState::Failed);
  EXPECT_FALSE(accepted.load());

  profile.serverUrl =
      "imap://user:secret@127.0.0.1:" + std::to_string(server.port());
  profile.serverAuthority.clear();
  auto userInfo = readImapMailbox(profile, {}, MailCredentialLease{}, cancel);
  EXPECT_EQ(userInfo.state, CaptureState::Failed);
  EXPECT_FALSE(accepted.load());
}

TEST(EvidenceImapTransportTest, CancellationDuringGreetingIsBounded) {
  LoopbackImapServer server;
  server.start([](Io &) { std::this_thread::sleep_for(500ms); });
  auto profile = profileFor(server.port());
  ImapReadRequest request{.timeoutMilliseconds = 2000};
  std::atomic_bool cancel{};
  std::thread canceller([&] {
    std::this_thread::sleep_for(50ms);
    cancel.store(true);
  });
  const auto start = std::chrono::steady_clock::now();
  auto result =
      readImapMailbox(profile, request, MailCredentialLease{}, cancel);
  canceller.join();
  EXPECT_EQ(result.state, CaptureState::Cancelled);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 400ms);
}

TEST(EvidenceImapTransportTest,
     UntrustedTlsPeerNeverReceivesCredentialsAndCustomCaWorks) {
  TestCertificate certificate;
  MailCredentialBroker broker;
  std::atomic_bool cancel{};
  std::vector<std::string> rejectedCommands;
  LoopbackImapServer untrusted;
  untrusted.start(
      [&](Io &io) { serveReadOnlyMailbox(io, 80, {1}, rejectedCommands); },
      certificate.context);
  auto profile      = profileFor(untrusted.port());
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(untrusted.port());
  profile.serverAuthority                          = profile.serverUrl;
  profile.allowCleartextLoopbackWithoutCredentials = false;
  const auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                                   .accountScope    = profile.accountScope,
                                   .operation = MailCredentialOperation::Read},
                                  "alice", "test-secret", 1min);
  auto refused     = readImapMailbox(profile, {}, lease, cancel);
  untrusted.join();
  EXPECT_NE(refused.state, CaptureState::Complete);
  EXPECT_TRUE(rejectedCommands.empty());
  EXPECT_EQ(refused.coverage.find("test-secret"), std::string::npos);
  for (const auto &gap : refused.gaps)
    EXPECT_EQ(gap.detail.find("test-secret"), std::string::npos);

  std::vector<std::string> acceptedCommands;
  LoopbackImapServer trusted;
  trusted.start(
      [&](Io &io) {
        serveReadOnlyMailbox(io, 81, {1}, acceptedCommands, nullptr, nullptr,
                             {}, false, false, 0, {}, {}, false);
      },
      certificate.context);
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(trusted.port());
  profile.serverAuthority = profile.serverUrl;
  profile.caBundlePath    = certificate.path.string();
  const auto trustedLease =
      broker.issue({.serverAuthority = profile.serverAuthority,
                    .accountScope    = profile.accountScope,
                    .operation       = MailCredentialOperation::Read},
                   "alice", "test-secret", 1min);
  auto accepted = readImapMailbox(profile, {}, trustedLease, cancel);
  trusted.join();
  SCOPED_TRACE(observationTrace(accepted));
  EXPECT_EQ(accepted.state, CaptureState::Complete);
  ASSERT_EQ(accepted.messages.size(), 1U);
  EXPECT_EQ(accepted.messages.front().rawMessage, rawMessage(1));
  EXPECT_EQ(accepted.coverage.find("test-secret"), std::string::npos);
}

TEST(EvidenceImapTransportTest, WithholdsMessageEchoingActivePassword) {
  TestCertificate certificate;
  LoopbackImapServer server;
  std::vector<std::string> commands;
  constexpr std::string_view password = "echo-sensitive-pass";
  server.start(
      [&](Io &io) {
        serveReadOnlyMailbox(io, 82, {1}, commands, nullptr, nullptr, {}, false,
                             false, 0, std::string(password), {}, false);
      },
      certificate.context);
  auto profile      = profileFor(server.port());
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(server.port());
  profile.serverAuthority                          = profile.serverUrl;
  profile.caBundlePath                             = certificate.path.string();
  profile.allowCleartextLoopbackWithoutCredentials = false;
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                             .accountScope    = profile.accountScope,
                             .operation       = MailCredentialOperation::Read},
                            "alice", std::string(password), 1min);
  std::atomic_bool cancel{};
  const auto observation = readImapMailbox(profile, {}, lease, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Partial);
  EXPECT_TRUE(observation.messages.empty());
  ASSERT_EQ(observation.gaps.size(), 1U);
  EXPECT_EQ(observation.gaps.front().cursor, "1");
  EXPECT_NE(observation.gaps.front().detail.find("echoed an active credential"),
            std::string::npos);
  EXPECT_EQ(observation.gaps.front().detail.find(password), std::string::npos);
  EXPECT_EQ(observation.coverage.find(password), std::string::npos);
}

TEST(EvidenceImapTransportTest, WithholdsFlagsEchoingActivePassword) {
  TestCertificate certificate;
  LoopbackImapServer server;
  std::vector<std::string> commands;
  constexpr std::string_view password = "echo-sensitive-pass";
  server.start(
      [&](Io &io) {
        serveReadOnlyMailbox(io, 83, {1}, commands, nullptr, nullptr, {}, false,
                             false, 0, {}, std::string(password), false);
      },
      certificate.context);
  auto profile      = profileFor(server.port());
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(server.port());
  profile.serverAuthority                          = profile.serverUrl;
  profile.caBundlePath                             = certificate.path.string();
  profile.allowCleartextLoopbackWithoutCredentials = false;
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                             .accountScope    = profile.accountScope,
                             .operation       = MailCredentialOperation::Read},
                            "alice", std::string(password), 1min);
  std::atomic_bool cancel{};
  const auto observation = readImapMailbox(profile, {}, lease, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Partial);
  EXPECT_TRUE(observation.messages.empty());
  ASSERT_EQ(observation.gaps.size(), 1U);
  EXPECT_EQ(observation.gaps.front().cursor, "1");
  EXPECT_NE(observation.gaps.front().detail.find("echoed an active credential"),
            std::string::npos);
  EXPECT_EQ(observation.gaps.front().detail.find(password), std::string::npos);
  EXPECT_EQ(observation.coverage.find(password), std::string::npos);
}

TEST(EvidenceImapTransportTest, DuplicateFlagsCannotLaunderAnEchoedCredential) {
  TestCertificate certificate;
  LoopbackImapServer server;
  std::vector<std::string> commands;
  constexpr std::string_view password = "echo-sensitive-pass";
  server.start(
      [&](Io &io) {
        serveReadOnlyMailbox(io, 84, {1}, commands, nullptr, nullptr, {}, false,
                             false, 0, {}, std::string(password), false, false,
                             false, true, false);
      },
      certificate.context);
  auto profile      = profileFor(server.port());
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(server.port());
  profile.serverAuthority                          = profile.serverUrl;
  profile.caBundlePath                             = certificate.path.string();
  profile.allowCleartextLoopbackWithoutCredentials = false;
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                             .accountScope    = profile.accountScope,
                             .operation       = MailCredentialOperation::Read},
                            "alice", std::string(password), 1min);
  std::atomic_bool cancel{};
  const auto observation = readImapMailbox(profile, {}, lease, cancel);
  server.join();
  SCOPED_TRACE(observationTrace(observation));
  EXPECT_EQ(observation.state, CaptureState::Partial);
  EXPECT_TRUE(observation.messages.empty());
  ASSERT_EQ(observation.gaps.size(), 1U);
  EXPECT_NE(observation.gaps.front().detail.find("duplicate critical FETCH"),
            std::string::npos);
  EXPECT_EQ(observation.gaps.front().detail.find(password), std::string::npos);
}

TEST(EvidenceImapTransportTest, RevokedLeaseStopsBeforeAuthentication) {
  TestCertificate certificate;
  LoopbackImapServer server;
  std::vector<std::string> commands;
  std::atomic_bool waiting{};
  std::atomic_bool release{};
  server.start(
      [&](Io &io) {
        serveReadOnlyMailbox(io, 90, {1}, commands, &waiting, &release, {},
                             false, false, 0, {}, {}, false);
      },
      certificate.context);
  auto profile      = profileFor(server.port());
  profile.serverUrl = "imaps://127.0.0.1:" + std::to_string(server.port());
  profile.serverAuthority                          = profile.serverUrl;
  profile.caBundlePath                             = certificate.path.string();
  profile.allowCleartextLoopbackWithoutCredentials = false;
  MailCredentialBroker broker;
  auto lease = broker.issue({.serverAuthority = profile.serverAuthority,
                             .accountScope    = profile.accountScope,
                             .operation       = MailCredentialOperation::Read},
                            "alice", "test-secret", 1min);
  std::atomic_bool cancel{};
  MailObservation result;
  std::thread reader(
      [&] { result = readImapMailbox(profile, {}, lease, cancel); });
  for (int i = 0; i < 500 && !waiting.load(); ++i)
    std::this_thread::sleep_for(1ms);
  const bool observedWaiting = waiting.load();
  broker.revoke(lease);
  release.store(true);
  reader.join();
  server.join();
  ASSERT_TRUE(observedWaiting);
  EXPECT_FALSE(lease.valid());
  EXPECT_EQ(std::ranges::any_of(commands,
                                [](const auto &command) {
                                  return command.find("AUTHENTICATE") !=
                                             std::string::npos ||
                                         command.find("LOGIN") !=
                                             std::string::npos;
                                }),
            false);
  EXPECT_NE(result.state, CaptureState::Complete);
}

} // namespace
} // namespace xanadu::evidence
