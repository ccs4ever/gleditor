#include "common/xanadu/evidence/evidence_controller.hpp"

#include "common/xanadu/evidence/effect_runtime.hpp"
#include "common/xanadu/evidence/irc_read_job.hpp"
#include "common/xanadu/evidence/irc_transport.hpp"
#include "common/xanadu/evidence/mail_read_job.hpp"
#include "common/xanadu/evidence/query_job.hpp"
#include "common/xanadu/evidence/window_composition.hpp"
#include "common/xanadu/evidence/window_job.hpp"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace xanadu::evidence {
namespace {

std::vector<std::string> arguments(std::string_view command) {
  std::istringstream input{std::string(command)};
  std::vector<std::string> result;
  std::string value;
  while (input >> std::ws && !input.eof()) {
    if (!(input >> std::quoted(value))) {
      throw std::invalid_argument("Unterminated quoted argument");
    }
    result.push_back(value);
  }
  if (!result.empty() && result.front() == ":evidence") {
    result.erase(result.begin());
  }
  return result;
}

template <class T> T number(std::string_view text) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size()) {
    throw std::invalid_argument("Expected an unsigned record ID or byte count");
  }
  return value;
}

void require(const std::vector<std::string> &args, std::size_t size,
             std::string_view usage) {
  if (args.size() != size) {
    throw std::invalid_argument(std::string(usage));
  }
}

std::string summary(const Record &record) {
  return "#" + std::to_string(record.id) + " " + record.kind + " " +
         record.label;
}

std::string escapedBytes(std::string_view bytes) {
  std::ostringstream out;
  for (const unsigned char byte : bytes) {
    if (byte >= 32 && byte < 127 && byte != '\\')
      out << static_cast<char>(byte);
    else
      out << "\\x" << std::hex << std::setfill('0') << std::setw(2)
          << static_cast<unsigned>(byte);
  }
  return out.str();
}

Value typedArgument(std::string_view text) {
  if (text == "n:") return std::monostate{};
  if (text.starts_with("t:")) return std::string(text.substr(2));
  if (text.starts_with("i:")) return number<std::int64_t>(text.substr(2));
  if (text.starts_with("r:")) {
    double value{};
    const auto bytes = text.substr(2);
    const auto [end, error] =
        std::from_chars(bytes.data(), bytes.data() + bytes.size(), value);
    if (error != std::errc{} || end != bytes.data() + bytes.size() ||
        !std::isfinite(value))
      throw std::invalid_argument("invalid finite real parameter");
    return value;
  }
  if (text.starts_with("b:")) {
    const auto hex = text.substr(2);
    if (hex.size() % 2)
      throw std::invalid_argument(
          "blob parameter must contain complete hex bytes");
    std::string bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
      unsigned value{};
      const auto [end, error] =
          std::from_chars(hex.data() + i, hex.data() + i + 2, value, 16);
      if (error != std::errc{} || end != hex.data() + i + 2)
        throw std::invalid_argument("invalid blob hex parameter");
      bytes.push_back(static_cast<char>(value));
    }
    return Binary{std::move(bytes)};
  }
  throw std::invalid_argument(
      "typed parameter requires n:, i:, r:, t: or b: prefix");
}

std::vector<std::string> recipientLines(std::string_view text) {
  std::vector<std::string> result;
  std::size_t begin{};
  while (begin < text.size()) {
    const auto end = text.find('\n', begin);
    auto line      = text.substr(begin, end == std::string_view::npos
                                            ? std::string_view::npos
                                            : end - begin);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!line.empty()) result.emplace_back(line);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  if (text.ends_with('\n') && text.size() == 1) return result;
  return result;
}

std::string newDraftCorrelation() {
  static std::atomic_uint64_t next{};
  const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
  return "xuzz-" + std::to_string(tick) + "-" +
         std::to_string(next.fetch_add(1, std::memory_order_relaxed));
}

} // namespace

struct EvidenceController::Impl {
  explicit Impl(std::shared_ptr<UserPermascroll> permascroll,
                FediverseTransportPolicy readPolicy)
      : evidence(permascroll), scroll(std::move(permascroll)),
        communicationRead(readPolicy), communicationPolicy(readPolicy) {}

  EvidenceCase evidence;
  std::shared_ptr<UserPermascroll> scroll;
  struct WorkSession {
    std::string document;
    std::filesystem::path path;
    std::unique_ptr<EffectRuntime> runtime;
  };
  std::vector<WorkSession> workSessions;
  EffectRuntime &work() {
    if (!evidence.isOpen())
      throw std::invalid_argument("open a private case first");
    const auto document = evidence.store().documentId().str();
    for (auto &session : workSessions)
      if (session.document == document &&
          session.path == std::filesystem::canonical(evidence.path()))
        return *session.runtime;
    auto runtime = std::make_unique<EffectRuntime>(evidence, scroll);
    workSessions.push_back({document,
                            std::filesystem::canonical(evidence.path()),
                            std::move(runtime)});
    return *workSessions.back().runtime;
  }
  bool workBusy() const {
    return std::ranges::any_of(workSessions, [](const auto &session) {
      return session.runtime->busy();
    });
  }
  CaptureJob capture;
  SqliteJoinJob join;
  struct PendingJoin {
    PreparedSqliteObservationJoin prepared;
    std::uint64_t generation;
  };
  std::optional<PendingJoin> pendingJoin;
  CommunicationReadJob communicationRead;
  IrcReadJob ircRead;
  MailReadJob mailRead;
  ObservationWindowJob observationWindow;
  struct PendingWindow {
    PreparedObservationWindow prepared;
    std::uint64_t generation{};
    std::string document;
  };
  std::optional<PendingWindow> pendingWindow;
  MailCredentialBroker mailBroker;
  std::map<RecordId, MailCredentialLease> mailReadLeases;
  std::map<RecordId, std::shared_ptr<MailCredentialLease>> mailSubmitLeases;
  FediverseTransportPolicy communicationPolicy;
  struct Pending {
    RecordId source;
    std::uint64_t generation;
  };
  std::optional<Pending> pending;
  std::optional<Pending> communicationPending;
  std::optional<Pending> mailPending;
  struct PendingIrc {
    RecordId connection{};
    std::uint64_t generation{};
    std::string document;
    std::shared_ptr<IrcSession> session;
    enum class Kind { Connect, Join, Live, History } kind{Kind::Connect};
  };
  std::optional<PendingIrc> pendingIrc;
  struct IrcCommit {
    PendingIrc pending;
    IrcReadCompletion completion;
    bool retained{};
    bool linked{};
    bool acknowledged{};
    RecordId observation{};
  };
  std::optional<IrcCommit> ircCommit;
  IrcCredentialBroker ircBroker;
  std::map<RecordId, IrcCredentialLease> ircSaslLeases;
  std::map<RecordId, std::shared_ptr<IrcSession>> ircSessions;
  std::map<RecordId, bool> ircConnected;
  std::uint64_t generation{};
  std::optional<RecordId> selected;
  std::optional<SpanSelection> span;
  std::optional<RecordId> spanTarget;
  std::string status{"Create or open a private evidence case"};
  std::vector<std::string> details;

  Record mailConnection(std::string_view name) const {
    for (const auto &record : evidence.records())
      if (record.kind == "communication-connection" && record.label == name &&
          record.attributes.contains("protocol") &&
          record.attributes.at("protocol") == "imap")
        return record;
    throw std::invalid_argument("Unknown IMAP connection");
  }

  Record smtpConnection(std::string_view name) const {
    for (const auto &record : evidence.records())
      if (record.kind == "communication-connection" && record.label == name &&
          record.attributes.contains("protocol") &&
          record.attributes.at("protocol") == "smtp-submission")
        return record;
    throw std::invalid_argument("Unknown SMTP submission connection");
  }

  Record ircConnection(std::string_view name) const {
    for (const auto &record : evidence.records())
      if (record.kind == "communication-connection" && record.label == name &&
          record.attributes.contains("protocol") &&
          record.attributes.at("protocol") == "irc")
        return record;
    throw std::invalid_argument("Unknown IRC connection");
  }

  Record ircConnection(RecordId id) const {
    const auto record = evidence.inspect(id);
    if (record.kind != "communication-connection" ||
        !record.attributes.contains("protocol") ||
        record.attributes.at("protocol") != "irc")
      throw std::invalid_argument("Record is not an IRC connection");
    return record;
  }

  static IrcConnectionConfig ircConfig(const Record &record) {
    const auto &attributes = record.attributes;
    IrcConnectionConfig config;
    config.host               = attributes.at("host");
    config.port               = number<std::uint16_t>(attributes.at("port"));
    config.serverAuthority    = attributes.at("server-authority");
    config.networkIncarnation = attributes.at("network-incarnation");
    config.nick               = attributes.at("nick");
    config.username           = attributes.at("username");
    config.realName           = attributes.at("real-name");
    config.accountScope       = attributes.at("account-scope");
    config.caBundlePath       = attributes.at("ca-bundle-path");
    config.useTls             = true;
    config.timeout =
        std::chrono::milliseconds(number<long>(attributes.at("timeout-ms")));
    config.maxLineBytes = number<std::size_t>(attributes.at("line-budget"));
    config.maxTagBytes  = number<std::size_t>(attributes.at("tag-budget"));
    config.maxBatchDepth =
        number<std::size_t>(attributes.at("batch-depth-budget"));
    config.maxEvents = number<std::size_t>(attributes.at("event-budget"));
    config.maxReadTargets =
        number<std::size_t>(attributes.at("read-target-budget"));
    config.maxPendingReads =
        number<std::size_t>(attributes.at("pending-read-budget"));
    config.maxPendingRecords =
        number<std::size_t>(attributes.at("pending-record-budget"));
    config.maxPendingBytes =
        number<std::size_t>(attributes.at("pending-byte-budget"));
    return config;
  }

  void revokeIrcCredentials() {
    for (const auto &[id, lease] : ircSaslLeases) {
      static_cast<void>(id);
      ircBroker.revoke(lease);
    }
    ircSaslLeases.clear();
  }

  void closeIrcSessions() {
    ircRead.cancel();
    for (auto &[id, session] : ircSessions) {
      static_cast<void>(id);
      if (session) session->cancel();
    }
    ircSessions.clear();
    ircConnected.clear();
    revokeIrcCredentials();
    pendingIrc.reset();
    ircCommit.reset();
  }

  static SmtpSubmissionProfile smtpProfile(const Record &record) {
    return {.serverUrl       = record.attributes.at("server-url"),
            .serverAuthority = record.attributes.at("server-authority"),
            .accountScope    = record.attributes.at("account-scope"),
            .caBundlePath    = record.attributes.at("ca-bundle-path"),
            .timeoutMilliseconds =
                number<long>(record.attributes.at("timeout-ms"))};
  }

  static ImapReadProfile mailProfile(const Record &record) {
    return {.serverUrl       = record.attributes.at("server-url"),
            .serverAuthority = record.attributes.at("server-authority"),
            .accountScope    = record.attributes.at("account-scope"),
            .mailbox         = record.attributes.at("mailbox"),
            .caBundlePath    = record.attributes.at("ca-bundle-path")};
  }

  static ImapReadRequest mailRequest(const Record &record) {
    const auto &attributes = record.attributes;
    ImapReadRequest request;
    request.maxMessages = number<std::size_t>(attributes.at("message-budget"));
    request.maxMessageBytes =
        number<std::size_t>(attributes.at("message-byte-budget"));
    request.maxTotalBytes =
        number<std::size_t>(attributes.at("total-byte-budget"));
    request.timeoutMilliseconds = number<long>(attributes.at("timeout-ms"));
    request.retentionBounds.maxRecords =
        number<std::size_t>(attributes.at("record-budget"));
    request.retentionBounds.maxBytes =
        number<std::size_t>(attributes.at("retention-byte-budget"));
    request.retentionBounds.maxStringBytes =
        number<std::size_t>(attributes.at("string-budget"));
    request.retentionBounds.maxParts =
        number<std::size_t>(attributes.at("part-budget"));
    request.retentionBounds.maxPartDepth =
        number<std::size_t>(attributes.at("part-depth-budget"));
    return request;
  }

  void revokeMailReads() {
    mailRead.cancel();
    for (const auto &[id, lease] : mailReadLeases) {
      static_cast<void>(id);
      mailBroker.revoke(lease);
    }
    mailReadLeases.clear();
  }

  void revokeMailSubmitLeases() {
    for (const auto &[intent, lease] : mailSubmitLeases) {
      static_cast<void>(intent);
      if (lease) mailBroker.revoke(*lease);
    }
    mailSubmitLeases.clear();
  }

  void inspect(RecordId id) {
    const auto record = evidence.inspect(id);
    selected          = id;
    details           = {summary(record), escapedBytes(record.text)};
    if (record.kind == "page") {
      const auto body = evidence.rawText(id);
      details.push_back(
          "Retained response body (" + std::to_string(body.size()) +
          " bytes; non-ASCII/control bytes escaped): " + escapedBytes(body));
    }
    for (const auto &[key, value] : record.attributes) {
      if (key == "mail-draft-bytes") {
        details.push_back(key + ": exact serialized payload retained (" +
                          std::to_string(value.size()) + " bytes)");
      } else {
        details.push_back(key + ": " + escapedBytes(value));
      }
    }
    if (record.parent) {
      details.push_back("parent: #" + std::to_string(*record.parent));
    }
  }

  RecordId sourceNamed(std::string_view name) const {
    for (const auto &record : evidence.records()) {
      if (record.kind == "connection" &&
          evidence.source(record.id).name == name) {
        return record.id;
      }
    }
    throw std::invalid_argument("Unknown connection: " + std::string(name));
  }
};

EvidenceController::EvidenceController(
    std::shared_ptr<UserPermascroll> permascroll,
    FediverseTransportPolicy readPolicy)
    : impl_(std::make_unique<Impl>(std::move(permascroll), readPolicy)) {}
EvidenceController::~EvidenceController() {
  impl_->revokeMailReads();
  impl_->revokeMailSubmitLeases();
  impl_->closeIrcSessions();
}

bool EvidenceController::bindMailReadCredential(
    std::string_view connectionName, std::string username, std::string password,
    const std::chrono::milliseconds lifetime) {
  try {
    if (!impl_->evidence.isOpen())
      throw std::invalid_argument("Open a private case first");
    if (lifetime <= std::chrono::milliseconds::zero() ||
        lifetime > std::chrono::minutes(15))
      throw std::invalid_argument("Read lease lifetime must be 1–900000 ms");
    const auto connection = impl_->mailConnection(connectionName);
    const auto profile    = Impl::mailProfile(connection);
    auto lease            = impl_->mailBroker.issue(
        {profile.serverAuthority, profile.accountScope,
                    MailCredentialOperation::Read},
        std::move(username), std::move(password), lifetime);
    if (auto previous = impl_->mailReadLeases.find(connection.id);
        previous != impl_->mailReadLeases.end())
      impl_->mailBroker.revoke(previous->second);
    impl_->mailReadLeases.insert_or_assign(connection.id, std::move(lease));
    impl_->status = "Session IMAP read credential bound; read explicitly";
    return true;
  } catch (const std::exception &) {
    // Broker diagnostics must not echo credential input into persisted or UI
    // evidence. Connection definitions remain inspectable independently.
    impl_->status = "Cannot bind IMAP credential: check connection, fields and "
                    "lease lifetime (1–900000 ms)";
    return false;
  }
}

bool EvidenceController::bindMailSubmitCredential(
    const RecordId intent, std::string username, std::string password,
    const std::chrono::milliseconds lifetime) {
  try {
    if (!impl_->evidence.isOpen())
      throw std::invalid_argument("Open a private case first");
    if (lifetime <= std::chrono::milliseconds::zero() ||
        lifetime > std::chrono::minutes(15))
      throw std::invalid_argument("Submit lease lifetime must be 1–900000 ms");
    auto &runtime      = impl_->work();
    const auto request = runtime.request(intent);
    if (request.kind != EffectKind::MailSubmit || !request.mailSubmit)
      throw std::invalid_argument("Intent is not an SMTP submission");
    const auto &profile = request.mailSubmit->profile;
    const MailCredentialScope scope{profile.serverAuthority,
                                    profile.accountScope,
                                    MailCredentialOperation::Submit};
    auto lease = std::make_shared<MailCredentialLease>(impl_->mailBroker.issue(
        scope, std::move(username), std::move(password), lifetime));
    runtime.bindMailCredential(intent, lease);
    if (const auto previous = impl_->mailSubmitLeases.find(intent);
        previous != impl_->mailSubmitLeases.end() && previous->second)
      impl_->mailBroker.revoke(*previous->second);
    impl_->mailSubmitLeases.insert_or_assign(intent, std::move(lease));
    impl_->status = "Session SMTP submit credential bound to this exact intent";
    return true;
  } catch (const std::exception &) {
    impl_->status =
        "Cannot bind SMTP credential: check intent, fields and lease lifetime "
        "(1–900000 ms)";
    return false;
  }
}

bool EvidenceController::revokeMailSubmitCredential(const RecordId intent) {
  const auto found = impl_->mailSubmitLeases.find(intent);
  if (found == impl_->mailSubmitLeases.end()) return false;
  impl_->mailBroker.revoke(*found->second);
  impl_->mailSubmitLeases.erase(found);
  impl_->status = "SMTP submit credential lease revoked for this intent";
  return true;
}

bool EvidenceController::bindIrcSaslCredential(
    const std::string_view connectionName, std::string username,
    std::string password, const std::chrono::milliseconds lifetime) {
  try {
    if (!impl_->evidence.isOpen())
      throw std::invalid_argument("Open a private case first");
    if (lifetime <= std::chrono::milliseconds::zero() ||
        lifetime > std::chrono::minutes(15))
      throw std::invalid_argument("SASL lease lifetime must be 1–900000 ms");
    const auto connection = impl_->ircConnection(connectionName);
    auto config           = Impl::ircConfig(connection);
    if (const auto problem = ircConnectionProblem(config))
      throw std::invalid_argument(*problem);
    const auto authority = ircServerAuthority(config);
    if (!authority) throw std::invalid_argument("Invalid IRC server authority");
    auto lease = impl_->ircBroker.issue(*authority, config.accountScope,
                                        std::move(username),
                                        std::move(password), lifetime);
    if (!lease) throw std::invalid_argument("Invalid SASL credential lease");
    if (const auto prior = impl_->ircSaslLeases.find(connection.id);
        prior != impl_->ircSaslLeases.end())
      impl_->ircBroker.revoke(prior->second);
    impl_->ircSaslLeases.insert_or_assign(connection.id, std::move(*lease));
    impl_->status = "Session IRC SASL credential bound; connect explicitly";
    return true;
  } catch (const std::exception &) {
    impl_->status = "Cannot bind IRC credential: check connection, fields and "
                    "lease lifetime (1–900000 ms)";
    return false;
  }
}

bool EvidenceController::revokeIrcSaslCredential(
    const std::string_view connectionName) {
  try {
    if (!impl_->evidence.isOpen())
      throw std::invalid_argument("Open a private case first");
    const auto connection = impl_->ircConnection(connectionName);
    const auto lease      = impl_->ircSaslLeases.find(connection.id);
    if (lease == impl_->ircSaslLeases.end()) return false;
    impl_->ircBroker.revoke(lease->second);
    impl_->ircSaslLeases.erase(lease);
    impl_->status = "Session IRC SASL credential revoked";
    return true;
  } catch (const std::exception &) {
    impl_->status = "Cannot revoke IRC credential for that connection";
    return false;
  }
}

bool EvidenceController::execute(std::string_view command) {
  try {
    auto args = arguments(command);
    if (args.empty() || args.front() == "help") {
      impl_->details = {
          "new \"directory\" \"title\" | open \"directory\" | save | close",
          "sqlite \"name\" \"database\" \"SELECT ...\" [\"key column\"]",
          "rest \"name\" \"https://endpoint\" | refresh \"name\" | poll",
          "fediverse-read NAME HTTPS_ORIGIN ACCOUNT_ID ACTOR_URI (public "
          "statuses)",
          "imap NAME IMAPS_URL ACCOUNT_SCOPE MAILBOX [CA_BUNDLE] | mail-read "
          "NAME | "
          "mail-revoke NAME (credentials use protected UI)",
          "smtp NAME SMTP_URL ACCOUNT_SCOPE TIMEOUT_MS [CA_BUNDLE] | "
          "mail-draft SMTP_NAME FROM SUBJECT TO_LINES CC_LINES BCC_LINES "
          "BODY [HTML_BODY]",
          "irc NAME HOST PORT ACCOUNT_SCOPE NETWORK_SCOPE NICK USER "
          "REAL_NAME CA_BUNDLE TIMEOUT_MS LINE_BYTES TAG_BYTES BATCH_DEPTH "
          "EVENTS READ_TARGETS PENDING_READS PENDING_RECORDS PENDING_BYTES",
          "irc-connect NAME | irc-join NAME TARGET [LEASE_MS] | "
          "irc-live NAME TARGET MESSAGE_LIMIT | irc-history NAME TARGET "
          "AFTER_MSGID LIMIT | irc-close NAME | irc-cancel",
          "mail-submit-revoke INTENT (bind submit credentials through a "
          "protected prompt)",
          "join LEFT_OBS LEFT_KEY RIGHT_OBS RIGHT_KEY inner|left many|one "
          "never|equal [pairs=N work=N rows=N bytes=N scan=N retain=N "
          "metadata=N]",
          "window OBS event|observation TIME_COLUMN START_MS END_MS "
          "WATERMARK_MS include|exclude|reject [arrival=COL key=COL "
          "output=N retain=N work=N scan=N]",
          "list | inspect ID | note ID \"text\"",
          "locate ID \"exact text\" | preview ID BYTE_OFFSET BYTE_LENGTH",
          "span ID BYTE_OFFSET BYTE_LENGTH \"text\" | cancel | cancel-window",
          "draft-fake TARGET DELAY_MS [unknown] | draft-process EXE CWD [ARG "
          "...]",
          "draft-sql DB SQL GUARD_QUERY TYPED_EXPECTED [PARAM ...] --guard "
          "[PARAM ...]",
          "review ID | grant INTENT [LIFETIME_MS] | revoke GRANT | dispatch "
          "INTENT GRANT",
          "retry INTENT GRANT [acknowledge-duplication] | cancel-work | "
          "pending",
          "history-text SOURCE RAW_HISTORY (retains occurrences, never "
          "executes)",
          "compare OLD_OBSERVATION NEW_OBSERVATION | export ID \"directory\"",
          "Connections are immutable. Refresh retains a new observation; notes "
          "remain pinned. HTTP redirects and authenticated endpoints are not "
          "supported by this read-only stage."};
      impl_->status = "Evidence case commands";
      return true;
    }
    const auto &op = args.front();
    if (op == "new" || op == "open") {
      require(args, op == "new" ? 3 : 2,
              "new \"directory\" \"title\" or open \"directory\"");
      if (impl_->evidence.isOpen()) {
        throw std::invalid_argument("Save and close the current case first");
      }
      if (op == "new") {
        impl_->evidence.create(args[1], args[2]);
      } else {
        impl_->evidence.open(args[1]);
      }
      std::ignore = impl_->work();
      ++impl_->generation;
      impl_->selected.reset();
      impl_->span.reset();
      impl_->spanTarget.reset();
      impl_->details.clear();
      impl_->status = "Opened private case: " + impl_->evidence.title() +
                      "; retained observations available offline";
      return true;
    }
    if (op == "poll") {
      require(args, 1, "poll");
      const bool changed = poll();
      if (!changed) {
        impl_->status =
            (impl_->capture.busy() || impl_->communicationRead.busy() ||
             impl_->ircRead.busy() || impl_->pendingIrc || impl_->ircCommit ||
             impl_->join.busy() || impl_->mailRead.busy() || impl_->workBusy())
                ? "Work is pending; poll again"
                : "No pending acquisition";
      }
      return true;
    }
    if (!impl_->evidence.isOpen()) {
      throw std::invalid_argument(
          "Create or open a private evidence case first");
    }
    if (op == "close") {
      require(args, 1, "close");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument(
            "Poll or cancel the pending IRC operation before closing the case");
      if (impl_->work().busy()) impl_->work().cancel();
      impl_->work().revokeAll();
      impl_->evidence.save();
      impl_->capture.cancel();
      impl_->communicationRead.cancel();
      impl_->revokeMailReads();
      impl_->revokeMailSubmitLeases();
      impl_->closeIrcSessions();
      impl_->join.cancel();
      impl_->observationWindow.cancel();
      impl_->pendingWindow.reset();
      ++impl_->generation;
      impl_->evidence.close();
      impl_->selected.reset();
      impl_->span.reset();
      impl_->spanTarget.reset();
      impl_->details.clear();
      impl_->status = "Case saved and closed; no source action dispatched";
    } else if (op == "save") {
      require(args, 1, "save");
      impl_->evidence.save();
      impl_->status = "Private case saved";
    } else if (op == "sqlite" || op == "rest") {
      if ((op == "sqlite" && args.size() != 4 && args.size() != 5) ||
          (op == "rest" && args.size() != 3)) {
        throw std::invalid_argument(
            "sqlite \"name\" \"database\" \"SELECT ...\" [\"key column\"] "
            "or rest \"name\" \"URL\"");
      }
      Source source;
      source.name    = args[1];
      source.kind    = op == "sqlite" ? SourceKind::SQLite : SourceKind::Rest;
      source.locator = args[2];
      if (op == "sqlite") {
        source.query = args[3];
        if (args.size() == 5) source.keyColumn = args[4];
      }
      const auto id = impl_->evidence.addSource(source);
      impl_->evidence.save();
      impl_->inspect(id);
      impl_->status = "Connection retained. Refresh explicitly to acquire.";
    } else if (op == "imap") {
      if (args.size() != 5 && args.size() != 6)
        throw std::invalid_argument(
            "imap NAME IMAPS_URL ACCOUNT_SCOPE MAILBOX [CA_BUNDLE]");
      if (args[1].empty() || args[1].size() > 4096)
        throw std::invalid_argument(
            "Connection name must be bounded and nonempty");
      const auto authority = imapServerAuthority(args[2]);
      if (!authority || !args[2].starts_with("imaps://"))
        throw std::invalid_argument("Use an IMAPS URL without credentials");
      const ImapReadProfile profile{.serverUrl       = args[2],
                                    .serverAuthority = *authority,
                                    .accountScope    = args[3],
                                    .mailbox         = args[4],
                                    .caBundlePath =
                                        args.size() == 6 ? args[5] : ""};
      const ImapReadRequest request;
      if (const auto problem = imapReadProblem(profile, request))
        throw std::invalid_argument(*problem);
      for (const auto &record : impl_->evidence.records())
        if (record.kind == "communication-connection" &&
            record.label == args[1])
          throw std::invalid_argument(
              "Connection revisions are immutable; use a new name");
      const auto id = impl_->evidence.appendRecord(
          "communication-connection", args[1], "Read-only IMAP mailbox", {},
          {{"protocol", "imap"},
           {"server-url", profile.serverUrl},
           {"server-authority", profile.serverAuthority},
           {"account-scope", profile.accountScope},
           {"mailbox", profile.mailbox},
           {"ca-bundle-path", profile.caBundlePath},
           {"message-budget", std::to_string(request.maxMessages)},
           {"message-byte-budget", std::to_string(request.maxMessageBytes)},
           {"total-byte-budget", std::to_string(request.maxTotalBytes)},
           {"timeout-ms", std::to_string(request.timeoutMilliseconds)},
           {"record-budget",
            std::to_string(request.retentionBounds.maxRecords)},
           {"retention-byte-budget",
            std::to_string(request.retentionBounds.maxBytes)},
           {"string-budget",
            std::to_string(request.retentionBounds.maxStringBytes)},
           {"part-budget", std::to_string(request.retentionBounds.maxParts)},
           {"part-depth-budget",
            std::to_string(request.retentionBounds.maxPartDepth)},
           {"authority", "descriptive-only-session-lease-required"}});
      impl_->evidence.durableSave();
      impl_->inspect(id);
      impl_->status =
          "IMAP connection retained; bind a session read credential";
    } else if (op == "smtp") {
      if (args.size() != 5 && args.size() != 6)
        throw std::invalid_argument(
            "smtp NAME SMTP_URL ACCOUNT_SCOPE TIMEOUT_MS [CA_BUNDLE]");
      if (args[1].empty() || args[1].size() > 4096)
        throw std::invalid_argument(
            "Connection name must be bounded and nonempty");
      const auto authority = smtpServerAuthority(args[2]);
      if (!authority)
        throw std::invalid_argument(
            "Use an SMTP submission URL without credentials, query, or "
            "fragment");
      const auto timeout = number<long>(args[4]);
      if (timeout <= 0 || timeout > 5000)
        throw std::invalid_argument(
            "SMTP timeout must be 1–5000 ms to fit effect limits");
      const SmtpSubmissionProfile profile{.serverUrl       = args[2],
                                          .serverAuthority = *authority,
                                          .accountScope    = args[3],
                                          .caBundlePath =
                                              args.size() == 6 ? args[5] : "",
                                          .timeoutMilliseconds = timeout};
      const MailDraftInput validationInput{
          .from       = "validation@example.invalid",
          .recipients = {{"to", "validation@example.invalid"}},
          .subject    = "profile validation",
          .textBody   = "profile validation"};
      const auto validationDraft =
          buildMimeDraft(validationInput, "smtp-profile-validation");
      if (const auto problem = smtpSubmissionProblem(profile, validationDraft))
        throw std::invalid_argument(*problem);
      for (const auto &record : impl_->evidence.records())
        if (record.kind == "communication-connection" &&
            record.label == args[1])
          throw std::invalid_argument(
              "Connection revisions are immutable; use a new name");
      const auto id = impl_->evidence.appendRecord(
          "communication-connection", args[1], "SMTP submission service", {},
          {{"protocol", "smtp-submission"},
           {"server-url", profile.serverUrl},
           {"server-authority", profile.serverAuthority},
           {"account-scope", profile.accountScope},
           {"ca-bundle-path", profile.caBundlePath},
           {"timeout-ms", std::to_string(profile.timeoutMilliseconds)},
           {"authority", "descriptive-only-session-submit-lease-required"}});
      impl_->evidence.durableSave();
      impl_->inspect(id);
      impl_->status =
          "SMTP connection retained; prepare and review an exact local draft";
    } else if (op == "irc") {
      require(args, 19,
              "irc NAME HOST PORT ACCOUNT_SCOPE NETWORK_SCOPE NICK USER "
              "REAL_NAME CA_BUNDLE TIMEOUT_MS LINE_BYTES TAG_BYTES "
              "BATCH_DEPTH EVENTS READ_TARGETS PENDING_READS "
              "PENDING_RECORDS PENDING_BYTES (verified TLS only)");
      if (args[1].empty() || args[1].size() > 4096 || args[4].empty() ||
          args[5].empty())
        throw std::invalid_argument(
            "IRC name, account scope and network scope are required");
      IrcConnectionConfig config;
      config.host               = args[2];
      config.port               = number<std::uint16_t>(args[3]);
      config.accountScope       = args[4];
      config.networkIncarnation = args[5];
      config.nick               = args[6];
      config.username           = args[7];
      config.realName           = args[8];
      config.caBundlePath       = args[9];
      config.useTls             = true;
      config.timeout        = std::chrono::milliseconds(number<long>(args[10]));
      config.maxLineBytes   = number<std::size_t>(args[11]);
      config.maxTagBytes    = number<std::size_t>(args[12]);
      config.maxBatchDepth  = number<std::size_t>(args[13]);
      config.maxEvents      = number<std::size_t>(args[14]);
      config.maxReadTargets = number<std::size_t>(args[15]);
      config.maxPendingReads   = number<std::size_t>(args[16]);
      config.maxPendingRecords = number<std::size_t>(args[17]);
      config.maxPendingBytes   = number<std::size_t>(args[18]);
      const auto authority     = ircServerAuthority(config);
      if (!authority) throw std::invalid_argument("Invalid IRC TLS profile");
      config.serverAuthority = *authority;
      if (const auto problem = ircConnectionProblem(config))
        throw std::invalid_argument(*problem);
      for (const auto &record : impl_->evidence.records())
        if (record.kind == "communication-connection" &&
            record.label == args[1])
          throw std::invalid_argument(
              "Connection revisions are immutable; use a new name");
      const auto id = impl_->evidence.appendRecord(
          "communication-connection", args[1], "IRC TLS service", {},
          {{"protocol", "irc"},
           {"host", config.host},
           {"port", std::to_string(config.port)},
           {"server-authority", config.serverAuthority},
           {"account-scope", config.accountScope},
           {"network-incarnation", config.networkIncarnation},
           {"nick", config.nick},
           {"username", config.username},
           {"real-name", config.realName},
           {"ca-bundle-path", config.caBundlePath},
           {"timeout-ms", std::to_string(config.timeout.count())},
           {"line-budget", std::to_string(config.maxLineBytes)},
           {"tag-budget", std::to_string(config.maxTagBytes)},
           {"batch-depth-budget", std::to_string(config.maxBatchDepth)},
           {"event-budget", std::to_string(config.maxEvents)},
           {"read-target-budget", std::to_string(config.maxReadTargets)},
           {"pending-read-budget", std::to_string(config.maxPendingReads)},
           {"pending-record-budget", std::to_string(config.maxPendingRecords)},
           {"pending-byte-budget", std::to_string(config.maxPendingBytes)},
           {"authority", "configured-TLS-endpoint-and-scope-claim"}});
      impl_->evidence.durableSave();
      impl_->inspect(id);
      impl_->status = "Immutable IRC TLS connection saved; connect explicitly";
    } else if (op == "irc-connect") {
      require(args, 2, "irc-connect NAME");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument("An IRC operation is still pending");
      const auto connection = impl_->ircConnection(args[1]);
      if (impl_->ircSessions.contains(connection.id))
        throw std::invalid_argument(
            "This IRC definition already has a session; close it first");
      auto config = Impl::ircConfig(connection);
      if (const auto credential = impl_->ircSaslLeases.find(connection.id);
          credential != impl_->ircSaslLeases.end())
        config.sasl = credential->second;
      auto session = std::make_shared<IrcSession>(std::move(config));
      if (!impl_->ircRead.start(session, IrcConnectOperation{}))
        throw std::invalid_argument("An IRC operation is still pending");
      impl_->ircSessions.emplace(connection.id, session);
      impl_->ircConnected.insert_or_assign(connection.id, false);
      impl_->pendingIrc =
          Impl::PendingIrc{connection.id, impl_->generation,
                           impl_->evidence.store().documentId().str(), session,
                           Impl::PendingIrc::Kind::Connect};
      impl_->status = "Connecting to the saved IRC TLS endpoint";
    } else if (op == "irc-join") {
      if (args.size() != 3 && args.size() != 4)
        throw std::invalid_argument("irc-join NAME CHANNEL [LEASE_MS]");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument("An IRC operation is still pending");
      const auto connection = impl_->ircConnection(args[1]);
      const auto session    = impl_->ircSessions.find(connection.id);
      if (session == impl_->ircSessions.end() ||
          !impl_->ircConnected[connection.id])
        throw std::invalid_argument("Connect this IRC session first");
      const auto lifetime = args.size() == 4 ? number<std::int64_t>(args[3])
                                             : std::int64_t{300000};
      if (lifetime <= 0 || lifetime > 900000)
        throw std::invalid_argument("IRC JOIN lease must be 1–900000 ms");
      const auto identity = session->second->identity();
      auto revoked        = std::make_shared<std::atomic_bool>(false);
      IrcJoinAuthority authority{identity.serverAuthority,
                                 identity.networkIncarnation,
                                 args[2],
                                 {args[2]},
                                 std::chrono::steady_clock::now() +
                                     std::chrono::milliseconds(lifetime),
                                 std::move(revoked)};
      if (!impl_->ircRead.start(session->second, std::move(authority)))
        throw std::invalid_argument("An IRC operation is still pending");
      impl_->pendingIrc =
          Impl::PendingIrc{connection.id, impl_->generation,
                           impl_->evidence.store().documentId().str(),
                           session->second, Impl::PendingIrc::Kind::Join};
      impl_->status = "Requesting explicit IRC channel membership";
    } else if (op == "irc-live") {
      require(args, 4, "irc-live NAME TARGET MESSAGE_LIMIT");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument("An IRC operation is still pending");
      const auto connection = impl_->ircConnection(args[1]);
      const auto session    = impl_->ircSessions.find(connection.id);
      if (session == impl_->ircSessions.end() ||
          !impl_->ircConnected[connection.id])
        throw std::invalid_argument("Connect this IRC session first");
      const auto limit = number<std::size_t>(args[3]);
      if (!limit || limit > Impl::ircConfig(connection).maxEvents)
        throw std::invalid_argument("IRC live read limit exceeds its budget");
      if (!impl_->ircRead.start(
              session->second,
              IrcLiveReadRequest{.target = args[2], .maxMessages = limit}))
        throw std::invalid_argument("An IRC operation is still pending");
      impl_->pendingIrc =
          Impl::PendingIrc{connection.id, impl_->generation,
                           impl_->evidence.store().documentId().str(),
                           session->second, Impl::PendingIrc::Kind::Live};
      impl_->status = "Reading retained IRC session events; poll to retain";
    } else if (op == "irc-history") {
      require(args, 5, "irc-history NAME TARGET AFTER_MSGID LIMIT");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument("An IRC operation is still pending");
      const auto connection = impl_->ircConnection(args[1]);
      const auto session    = impl_->ircSessions.find(connection.id);
      if (session == impl_->ircSessions.end() ||
          !impl_->ircConnected[connection.id])
        throw std::invalid_argument("Connect this IRC session first");
      const auto limit = number<std::size_t>(args[4]);
      if (!limit || limit > Impl::ircConfig(connection).maxEvents)
        throw std::invalid_argument("IRC history limit exceeds its budget");
      IrcHistoryRequest request{
          .target = args[2], .afterMessageId = args[3], .limit = limit};
      if (!impl_->ircRead.start(session->second, std::move(request)))
        throw std::invalid_argument("An IRC operation is still pending");
      impl_->pendingIrc =
          Impl::PendingIrc{connection.id, impl_->generation,
                           impl_->evidence.store().documentId().str(),
                           session->second, Impl::PendingIrc::Kind::History};
      impl_->status = "Reading explicit IRC history; poll to retain";
    } else if (op == "irc-close") {
      require(args, 2, "irc-close NAME");
      const auto connection = impl_->ircConnection(args[1]);
      const auto session    = impl_->ircSessions.find(connection.id);
      if (session == impl_->ircSessions.end())
        throw std::invalid_argument("No live IRC session exists");
      if (impl_->pendingIrc || impl_->ircCommit || impl_->ircRead.busy())
        throw std::invalid_argument(
            "Poll the pending operation before closing its IRC session");
      session->second->cancel();
      impl_->ircSessions.erase(session);
      impl_->ircConnected.erase(connection.id);
      if (const auto credential = impl_->ircSaslLeases.find(connection.id);
          credential != impl_->ircSaslLeases.end()) {
        impl_->ircBroker.revoke(credential->second);
        impl_->ircSaslLeases.erase(credential);
      }
      impl_->status =
          "IRC session closed and its SASL lease revoked; reconnect explicitly";
    } else if (op == "irc-cancel") {
      require(args, 1, "irc-cancel");
      if (!impl_->pendingIrc)
        throw std::invalid_argument("No IRC operation is pending");
      impl_->ircRead.cancel();
      impl_->status =
          "IRC operation cancellation requested; session remains open";
    } else if (op == "mail-draft") {
      if (args.size() != 8 && args.size() != 9)
        throw std::invalid_argument(
            "mail-draft SMTP_NAME FROM SUBJECT TO_LINES CC_LINES BCC_LINES "
            "BODY [HTML_BODY]");
      const auto connection = impl_->smtpConnection(args[1]);
      const auto profile    = Impl::smtpProfile(connection);
      MailDraftInput draft{
          .from = args[2], .subject = args[3], .textBody = args[7]};
      for (auto [role, values] :
           {std::pair<std::string_view, std::string_view>{"to", args[4]},
            {"cc", args[5]},
            {"bcc", args[6]}}) {
        for (auto &address : recipientLines(values))
          draft.recipients.push_back({std::string(role), std::move(address)});
      }
      if (args.size() == 9) draft.htmlBody = args[8];
      const auto serialized = buildMimeDraft(draft, newDraftCorrelation());
      EffectRequest request;
      request.kind = EffectKind::MailSubmit;
      request.limits.timeoutMs =
          static_cast<std::uint64_t>(profile.timeoutMilliseconds);
      request.limits.maxInputBytes = serialized.bytes.size();
      request.limits.maxFields     = draft.recipients.size();
      request.mailSubmit           = MailSubmitRequest{profile, serialized};
      const auto intent            = impl_->work().draft(request);
      const auto revision          = impl_->evidence.appendRecord(
          "mail-draft-revision", "Reviewed MIME draft revision",
          "Local immutable draft; exact serialized bytes are in the linked "
                   "effect intent.",
          intent,
          {{"connection", connection.label},
                    {"message-id", serialized.messageId},
                    {"correlation-id", serialized.correlationId},
                    {"recipient-occurrences", std::to_string(draft.recipients.size())},
                    {"bcc-in-headers", "no"}});
      static_cast<void>(impl_->evidence.appendRecord("mail-draft-from", "From",
                                                     draft.from, revision));
      static_cast<void>(impl_->evidence.appendRecord(
          "mail-draft-subject", "Subject", draft.subject, revision));
      for (std::size_t index = 0; index < draft.recipients.size(); ++index) {
        const auto &recipient = draft.recipients[index];
        static_cast<void>(impl_->evidence.appendRecord(
            "mail-recipient-occurrence", recipient.role, recipient.address,
            revision,
            {{"role", recipient.role},
             {"position", std::to_string(index)},
             {"envelope-recipient", "yes"},
             {"header-visible", recipient.role == "bcc" ? "no" : "yes"}}));
      }
      static_cast<void>(impl_->evidence.appendRecord(
          "mail-draft-body", "Plain text body", draft.textBody, revision,
          {{"content-type", "text/plain; charset=UTF-8"}}));
      if (draft.htmlBody)
        static_cast<void>(impl_->evidence.appendRecord(
            "mail-draft-body", "HTML body", *draft.htmlBody, revision,
            {{"content-type", "text/html; charset=UTF-8"}}));
      impl_->evidence.durableSave();
      impl_->inspect(intent);
      impl_->details.push_back(summary(impl_->evidence.inspect(revision)));
      for (const auto &record : impl_->evidence.records())
        if (record.parent == revision)
          impl_->details.push_back(summary(record) + ": " +
                                   escapedBytes(record.text));
      impl_->status = "Immutable SMTP draft intent #" + std::to_string(intent) +
                      " retained; review, bind submit credentials, grant, "
                      "then dispatch explicitly";
    } else if (op == "mail-read" || op == "mail-revoke") {
      require(args, 2, "mail-read NAME or mail-revoke NAME");
      const auto connection = impl_->mailConnection(args[1]);
      const auto lease      = impl_->mailReadLeases.find(connection.id);
      if (op == "mail-revoke") {
        if (lease != impl_->mailReadLeases.end()) {
          impl_->mailBroker.revoke(lease->second);
          impl_->mailReadLeases.erase(lease);
        }
        impl_->status = "IMAP session read lease revoked";
      } else {
        if (lease == impl_->mailReadLeases.end() || !lease->second.valid())
          throw std::invalid_argument(
              "Bind a live session read credential first");
        if (impl_->mailPending ||
            !impl_->mailRead.start(Impl::mailProfile(connection),
                                   Impl::mailRequest(connection),
                                   lease->second))
          throw std::invalid_argument("An IMAP read is pending; poll it first");
        impl_->mailPending = Impl::Pending{connection.id, impl_->generation};
        impl_->status = "Reading IMAP mailbox without marking messages read";
      }
    } else if (op == "mail-submit-revoke") {
      require(args, 2, "mail-submit-revoke INTENT_ID");
      if (!revokeMailSubmitCredential(number<RecordId>(args[1])))
        throw std::invalid_argument(
            "No live submit credential lease exists for that intent");
    } else if (op == "fediverse-read") {
      require(args, 5, "fediverse-read NAME HTTPS_ORIGIN ACCOUNT_ID ACTOR_URI");
      if (impl_->communicationPending || impl_->communicationRead.busy())
        throw std::invalid_argument(
            "A communication read is still pending; poll it first");
      if (args[1].empty() || args[1].size() > 4096 || args[2].size() > 4096 ||
          args[3].empty() || args[3].size() > 4096 || args[4].empty() ||
          args[4].size() > 4096)
        throw std::invalid_argument(
            "Connection fields must be nonempty and bounded");
      FediverseReadRequest request{.instanceOrigin  = args[2],
                                   .accountId       = args[3],
                                   .accountActorUri = args[4]};
      if (const auto problem =
              fediverseReadProblem(request, impl_->communicationPolicy))
        throw std::invalid_argument(*problem);
      const auto fingerprint = fediverseRequestFingerprint(request);
      std::optional<RecordId> connection;
      for (const auto &record : impl_->evidence.records()) {
        if (record.kind == "communication-connection" &&
            record.label == args[1]) {
          if (record.attributes.at("request-fingerprint") != fingerprint)
            throw std::invalid_argument(
                "Connection revisions are immutable; choose a new name");
          connection = record.id;
        }
      }
      if (!connection) {
        connection = impl_->evidence.appendRecord(
            "communication-connection", args[1],
            "Public account status collection", {},
            {{"protocol", "mastodon-rest"},
             {"origin", args[2]},
             {"account-id", args[3]},
             {"actor-uri", args[4]},
             {"request-fingerprint", fingerprint},
             {"record-budget",
              std::to_string(request.limits.records.maxRecords)},
             {"byte-budget", std::to_string(request.limits.records.maxBytes)},
             {"string-budget",
              std::to_string(request.limits.records.maxStringBytes)},
             {"page-budget", std::to_string(request.limits.maxPages)},
             {"header-budget", std::to_string(request.limits.maxHeaderBytes)},
             {"timeout-ms", std::to_string(request.limits.timeoutMs)},
             {"authority", "public-read-only"}});
        impl_->evidence.save();
      }
      if (!impl_->communicationRead.start(std::move(request)))
        throw std::invalid_argument("A communication read is still pending");
      impl_->communicationPending =
          Impl::Pending{*connection, impl_->generation};
      impl_->inspect(*connection);
      impl_->status =
          "Reading public account statuses; cancel or check capture status";
    } else if (op == "refresh") {
      require(args, 2, "refresh \"connection name\"");
      const auto id = impl_->sourceNamed(args[1]);
      if (impl_->pending || !impl_->capture.start(impl_->evidence.source(id))) {
        throw std::invalid_argument(
            "A capture is still pending; poll it first");
      }
      impl_->pending = Impl::Pending{id, impl_->generation};
      impl_->status  = "Acquiring " + args[1] + "; poll for completion";
    } else if (op == "join") {
      if (args.size() < 8)
        throw std::invalid_argument(
            "join LEFT_OBS LEFT_KEY RIGHT_OBS RIGHT_KEY inner|left many|one "
            "never|equal [pairs=N work=N rows=N bytes=N scan=N retain=N "
            "metadata=N]");
      if (impl_->pendingJoin || impl_->join.busy())
        throw std::invalid_argument("A join is still pending; poll it first");
      if (impl_->pendingWindow || impl_->observationWindow.busy())
        throw std::invalid_argument(
            "A window evaluation is still pending; poll it first");
      SqliteObservationJoinRequest request{
          .leftObservation  = number<RecordId>(args[1]),
          .leftKeyColumn    = args[2],
          .rightObservation = number<RecordId>(args[3]),
          .rightKeyColumn   = args[4]};
      if (args[5] != "inner" && args[5] != "left")
        throw std::invalid_argument("join kind must be inner or left");
      request.kind = args[5] == "inner" ? vql::RelationJoinKind::Inner
                                        : vql::RelationJoinKind::Left;
      if (args[6] != "many" && args[6] != "one")
        throw std::invalid_argument("join cardinality must be many or one");
      request.cardinality = args[6] == "many"
                                ? vql::RelationCardinality::Many
                                : vql::RelationCardinality::AtMostOne;
      if (args[7] != "never" && args[7] != "equal")
        throw std::invalid_argument("join null policy must be never or equal");
      request.nullMatch = args[7] == "never"
                              ? vql::RelationNullMatchPolicy::Never
                              : vql::RelationNullMatchPolicy::Equal;
      std::set<std::string> options;
      for (std::size_t index = 8; index < args.size(); ++index) {
        const auto equal = args[index].find('=');
        if (equal == std::string::npos)
          throw std::invalid_argument("join budget option requires NAME=N");
        const auto key = args[index].substr(0, equal);
        if (!options.insert(key).second)
          throw std::invalid_argument("join budget option is repeated");
        const auto value = number<std::size_t>(
            std::string_view(args[index]).substr(equal + 1));
        if (key == "pairs")
          request.joinLimits.maxOutputPairs = value;
        else if (key == "work")
          request.joinLimits.maxWork = value;
        else if (key == "rows")
          request.joinLimits.maxInputRows = value;
        else if (key == "bytes")
          request.joinLimits.maxKeyBytes = value;
        else if (key == "scan")
          request.maxScanRecords = value;
        else if (key == "scan-bytes")
          request.maxScanBytes = value;
        else if (key == "retain")
          request.maxRetainedRecords = value;
        else if (key == "metadata")
          request.maxMetadataBytes = value;
        else
          throw std::invalid_argument("unknown join budget option");
      }
      auto prepared = prepareSqliteObservationJoin(impl_->evidence, request);
      if (!prepared) throw std::invalid_argument(prepared.error().message);
      impl_->pendingJoin =
          Impl::PendingJoin{std::move(*prepared), impl_->generation};
      if (!impl_->join.start(impl_->pendingJoin->prepared.evaluation)) {
        impl_->pendingJoin.reset();
        throw std::runtime_error("cannot start join evaluation");
      }
      impl_->status =
          "Computing a bounded local join; poll for retained results";
    } else if (op == "window") {
      if (args.size() < 8)
        throw std::invalid_argument(
            "window OBS event|observation TIME_COLUMN START_MS END_MS "
            "WATERMARK_MS include|exclude|reject [arrival=COL key=COL "
            "input=N gaps=N gap-bytes=N key-bytes=N output=N work=N "
            "scan=N scan-bytes=N retain=N metadata=N]");
      if (impl_->pendingWindow || impl_->observationWindow.busy())
        throw std::invalid_argument(
            "A window evaluation is still pending; poll it first");
      if (impl_->pendingJoin || impl_->join.busy())
        throw std::invalid_argument("A join is still pending; poll it first");
      SqliteObservationWindowRequest request;
      request.observation = number<RecordId>(args[1]);
      if (args[2] == "event") {
        request.timeBasis  = vql::WindowTimeBasis::EventTime;
        request.timeColumn = args[3];
      } else if (args[2] == "observation") {
        if (args[3] != "-")
          throw std::invalid_argument(
              "observation-time uses TIME_COLUMN=- (retained started-at)");
        request.timeBasis = vql::WindowTimeBasis::ObservationTime;
      } else {
        throw std::invalid_argument(
            "window time basis must be event or observation");
      }
      request.startInclusive = number<std::int64_t>(args[4]);
      request.endExclusive   = number<std::int64_t>(args[5]);
      request.watermark      = number<std::int64_t>(args[6]);
      if (args[7] == "include")
        request.latePolicy = vql::WindowLatePolicy::Include;
      else if (args[7] == "exclude")
        request.latePolicy = vql::WindowLatePolicy::Exclude;
      else if (args[7] == "reject")
        request.latePolicy = vql::WindowLatePolicy::Reject;
      else
        throw std::invalid_argument(
            "window late policy must be include, exclude or reject");
      std::set<std::string> options;
      for (std::size_t index = 8; index < args.size(); ++index) {
        const auto equal = args[index].find('=');
        if (equal == std::string::npos || equal == 0 ||
            equal + 1 == args[index].size())
          throw std::invalid_argument("window option requires NAME=VALUE");
        const auto key = args[index].substr(0, equal);
        if (!options.insert(key).second)
          throw std::invalid_argument("window option is repeated");
        const auto value = std::string_view(args[index]).substr(equal + 1);
        if (key == "arrival" || key == "key") {
          if (value.size() > 4096)
            throw std::invalid_argument("window column name is too long");
          if (key == "arrival")
            request.arrivalWatermarkColumn = std::string(value);
          else
            request.keyColumn = std::string(value);
          continue;
        }
        const auto limit = number<std::size_t>(value);
        if (!limit)
          throw std::invalid_argument("window limits must be positive");
        if (key == "input" && limit <= 10000)
          request.maxInputOccurrences = limit;
        else if (key == "gaps" && limit <= 1000)
          request.maxSourceGaps = limit;
        else if (key == "gap-bytes" && limit <= 1024 * 1024)
          request.maxSourceGapBytes = limit;
        else if (key == "key-bytes" && limit <= 1024 * 1024)
          request.maxKeyBytes = limit;
        else if (key == "output" && limit <= 10000)
          request.maxOutputOccurrences = limit;
        else if (key == "work" && limit <= 1000000)
          request.maxWork = limit;
        else if (key == "scan" && limit <= 100000)
          request.maxScanRecords = limit;
        else if (key == "scan-bytes" && limit <= 8 * 1024 * 1024)
          request.maxScanBytes = limit;
        else if (key == "retain" && limit <= 50000)
          request.maxRetainedRecords = limit;
        else if (key == "metadata" && limit <= 1024 * 1024)
          request.maxMetadataBytes = limit;
        else
          throw std::invalid_argument(
              "unknown or excessive window resource option");
      }
      auto prepared = prepareSqliteObservationWindow(impl_->evidence, request);
      if (!prepared) throw std::invalid_argument(prepared.error().message);
      impl_->pendingWindow =
          Impl::PendingWindow{std::move(*prepared), impl_->generation,
                              impl_->evidence.store().documentId().str()};
      if (!impl_->observationWindow.start(
              impl_->pendingWindow->prepared.evaluation)) {
        impl_->pendingWindow.reset();
        throw std::runtime_error("cannot start window evaluation");
      }
      impl_->status =
          "Computing a bounded local window; poll for coverage and lineage";
    } else if (op == "cancel-window") {
      require(args, 1, "cancel-window");
      if (!impl_->pendingWindow)
        throw std::invalid_argument("No window evaluation is pending");
      impl_->observationWindow.cancel();
      impl_->status =
          "Window cancellation requested; poll to discard its result";
    } else if (op == "cancel") {
      require(args, 1, "cancel");
      impl_->capture.cancel();
      impl_->communicationRead.cancel();
      impl_->mailRead.cancel();
      if (impl_->pendingIrc) impl_->ircRead.cancel();
      impl_->join.cancel();
      impl_->observationWindow.cancel();
      impl_->status = "Cancellation requested; poll to retain coverage or "
                      "discard window results";
    } else if (op == "locate" || op == "preview") {
      require(args, op == "locate" ? 3 : 4,
              "locate ID \"exact text\" or preview ID BYTE_OFFSET BYTE_LENGTH");
      const auto id  = number<RecordId>(args[1]);
      const auto raw = impl_->evidence.rawText(id);
      impl_->inspect(id);
      SpanSelection selected{};
      if (op == "locate") {
        if (args[2].empty())
          throw std::invalid_argument("Text to quote is empty");
        const auto first = raw.find(args[2]);
        if (first == std::string::npos) {
          throw std::invalid_argument(
              "Exact text is absent from the retained bytes");
        }
        const auto second = raw.find(args[2], first + 1);
        if (second != std::string::npos) {
          impl_->details = {"Ambiguous text: matches at byte " +
                                std::to_string(first) + " and " +
                                std::to_string(second),
                            "Use a more specific quote, or enter byte "
                            "offset/length and preview."};
          throw std::invalid_argument(
              "Multiple retained occurrences; no range selected");
        }
        selected = {first, args[2].size()};
      } else {
        selected = {number<std::uint64_t>(args[2]),
                    number<std::uint64_t>(args[3])};
        if (selected.length == 0 || selected.offset > raw.size() ||
            selected.length > raw.size() - selected.offset) {
          throw std::invalid_argument(
              "Span lies outside the retained bytes or is empty");
        }
      }
      impl_->span       = selected;
      impl_->spanTarget = id;
      const auto preview =
          raw.substr(static_cast<std::size_t>(selected.offset),
                     static_cast<std::size_t>(selected.length));
      impl_->details = {
          "Retained record #" + std::to_string(id) + " bytes [" +
              std::to_string(selected.offset) + ", " +
              std::to_string(selected.offset + selected.length) + ")",
          "Exact byte preview (non-ASCII/control bytes escaped): " +
              escapedBytes(preview)};
      impl_->status =
          "Range located and previewed; attach a note to pin these exact bytes";
    } else if (op == "list" || op == "sources") {
      require(args, 1, "list or sources");
      impl_->details.clear();
      for (const auto &record : impl_->evidence.records()) {
        if (op == "list" || record.kind == "connection") {
          impl_->details.push_back(summary(record));
        }
      }
      impl_->status = "Retained case records; inspect a record ID for evidence";
    } else if (op == "inspect") {
      require(args, 2, "inspect RECORD_ID");
      impl_->inspect(number<RecordId>(args[1]));
      impl_->status = "Retained evidence (no source request)";
    } else if (op == "note" || op == "span") {
      require(args, op == "note" ? 3 : 5,
              "note ID \"text\" or span ID BYTE_OFFSET BYTE_LENGTH \"text\"");
      const auto target = number<RecordId>(args[1]);
      std::optional<SpanSelection> span;
      if (op == "span") {
        span = SpanSelection{number<std::uint64_t>(args[2]),
                             number<std::uint64_t>(args[3])};
      }
      const auto id = impl_->evidence.annotate(target, args.back(), span);
      impl_->evidence.save();
      impl_->inspect(id);
      impl_->status =
          "Note pinned to retained evidence; refresh will not move it";
    } else if (op == "compare") {
      require(args, 3, "compare OLD_OBSERVATION_ID NEW_OBSERVATION_ID");
      impl_->details = impl_->evidence.compare(number<RecordId>(args[1]),
                                               number<RecordId>(args[2]));
      impl_->status  = "Retained observation comparison; old notes stay pinned";
    } else if (op == "irc-draft") {
      require(args, 4, "irc-draft NAME TARGET MESSAGE");
      const auto connection = impl_->ircConnection(args[1]);
      const auto session    = impl_->ircSessions.find(connection.id);
      if (session == impl_->ircSessions.end() ||
          !impl_->ircConnected[connection.id])
        throw std::invalid_argument(
            "Connect this IRC session before drafting a message");
      const auto config   = Impl::ircConfig(connection);
      const auto identity = session->second->identity();
      EffectRequest request;
      request.kind = EffectKind::IrcSend;
      request.limits.timeoutMs =
          static_cast<std::uint64_t>(config.timeout.count());
      request.limits.maxInputBytes = args[3].size();
      request.limits.maxFields     = 1;
      request.ircSend              = IrcSendRequest{
                       .serverAuthority       = identity.serverAuthority,
                       .networkIncarnation    = identity.networkIncarnation,
                       .connectionIncarnation = identity.connectionIncarnation,
                       .target                = args[2],
                       .payload               = args[3],
                       .audience              = {args[2]}};
      const auto intent = impl_->work().draft(request);
      impl_->work().bindIrcSession(intent, session->second);
      impl_->inspect(intent);
      impl_->status =
          "Exact IRC message intent retained; review and grant it before "
          "dispatch";
    } else if (op == "draft-fake" || op == "draft-process" ||
               op == "draft-sql") {
      EffectRequest request;
      if (op == "draft-fake") {
        if (args.size() != 3 && args.size() != 4)
          throw std::invalid_argument("draft-fake TARGET DELAY_MS [unknown]");
        request.target      = args[1];
        request.fakeDelayMs = number<std::uint64_t>(args[2]);
        if (args.size() == 4 && args[3] != "unknown")
          throw std::invalid_argument("unknown fake outcome flag");
        request.fakeUnknown = args.size() == 4;
      } else if (op == "draft-process") {
        if (args.size() < 3)
          throw std::invalid_argument("draft-process EXE CWD [ARG ...]");
        request.kind   = EffectKind::Process;
        request.target = args[1];
        request.cwd    = args[2];
        request.argv.assign(args.begin() + 3, args.end());
      } else {
        if (args.size() < 6)
          throw std::invalid_argument(
              "draft-sql DB SQL GUARD_QUERY TYPED_EXPECTED [PARAM ...] --guard "
              "[PARAM ...]");
        request.kind   = EffectKind::SqlPatch;
        request.target = args[1];
        request.sql    = args[2];
        request.guard =
            SqlGuard{.query = args[3], .expected = typedArgument(args[4])};
        bool guard = false;
        for (std::size_t i = 5; i < args.size(); ++i) {
          if (args[i] == "--guard") {
            if (guard)
              throw std::invalid_argument(
                  "duplicate guard parameter separator");
            guard = true;
            continue;
          }
          (guard ? request.guard->parameters : request.parameters)
              .push_back(typedArgument(args[i]));
        }
        if (!guard)
          throw std::invalid_argument("SQL patch requires --guard separator");
      }
      const auto id = impl_->work().draft(request);
      impl_->inspect(id);
      impl_->status =
          "Draft #" + std::to_string(id) +
          " retained; review and grant this exact revision before dispatch";
    } else if (op == "review") {
      require(args, 2, "review RECORD_ID");
      impl_->inspect(number<RecordId>(args[1]));
      impl_->status = "Review only; no external dispatch";
    } else if (op == "grant") {
      if (args.size() != 2 && args.size() != 3)
        throw std::invalid_argument("grant INTENT_ID [LIFETIME_MS]");
      EffectGrantPolicy policy;
      if (args.size() == 3)
        policy.lifetime =
            std::chrono::milliseconds(number<std::int64_t>(args[2]));
      const auto id =
          impl_->work().authorize(number<RecordId>(args[1]), policy);
      impl_->inspect(id);
      impl_->status =
          "Host grant #" + std::to_string(id) +
          " issued for this session; dispatch remains a separate action";
    } else if (op == "revoke") {
      require(args, 2, "revoke GRANT_ID");
      impl_->work().revoke(number<RecordId>(args[1]));
      impl_->status = "Host authority revoked; this does not undo an accepted "
                      "external effect";
    } else if (op == "dispatch" || op == "retry") {
      if ((op == "dispatch" && args.size() != 3) ||
          (op == "retry" && args.size() != 3 && args.size() != 4))
        throw std::invalid_argument("dispatch INTENT GRANT | retry INTENT "
                                    "GRANT [acknowledge-duplication]");
      if (args.size() == 4 && args[3] != "acknowledge-duplication")
        throw std::invalid_argument("unknown duplication acknowledgement");
      if (impl_->workBusy() && !impl_->work().busy())
        throw std::runtime_error("Retain the previous case's effect receipt "
                                 "before dispatching more work");
      const auto id = impl_->work().dispatch(number<RecordId>(args[1]),
                                             number<RecordId>(args[2]),
                                             op == "retry", args.size() == 4);
      impl_->inspect(id);
      impl_->status = "Attempt #" + std::to_string(id) +
                      " durably admitted; check status for its receipt";
    } else if (op == "cancel-work") {
      require(args, 1, "cancel-work");
      impl_->work().cancel();
      impl_->status = "Cancellation requested; acceptance or an unknown "
                      "outcome may still be reported";
    } else if (op == "pending") {
      require(args, 1, "pending");
      impl_->work().recover();
      impl_->details.clear();
      const auto records           = impl_->evidence.records();
      const auto appendNewestFirst = [this](const std::string_view prefix,
                                            const auto &ordered) {
        for (auto record = ordered.rbegin(); record != ordered.rend(); ++record)
          if (record->kind.starts_with(prefix))
            impl_->details.push_back(
                summary(*record) +
                (record->attributes.contains("outcome")
                     ? " · " + record->attributes.at("outcome")
                     : ""));
      };
      // Keep recent effect outcomes above imported history so a long history
      // paste cannot push the latest receipt out of the first visible rows.
      appendNewestFirst("effect-", records);
      appendNewestFirst("history-", records);
      impl_->status =
          "Retained work; opening or inspecting a record never dispatches it";
    } else if (op == "history-text") {
      require(args, 3, "history-text SOURCE RAW_HISTORY");
      const auto ids = impl_->work().importHistory(args[2], args[1]);
      impl_->details.clear();
      for (const auto id : ids)
        impl_->details.push_back(summary(impl_->evidence.inspect(id)));
      if (!ids.empty()) impl_->selected = ids.front();
      impl_->status = "Imported " + std::to_string(ids.size()) +
                      " distinct history occurrences; none executed";
    } else if (op == "export") {
      require(args, 3, "export RECORD_ID \"new directory\"");
      const auto id = number<RecordId>(args[1]);
      impl_->evidence.exportSelection(std::span{&id, 1}, args[2]);
      impl_->status = "Selected closure written locally to " + args[2] +
                      "; nothing published";
    } else {
      throw std::invalid_argument("Unknown evidence command: " + op);
    }
    return true;
  } catch (const std::exception &error) {
    impl_->status = error.what();
    return false;
  }
}

bool EvidenceController::poll() {
  bool completed = false;
  for (auto &session : impl_->workSessions) {
    try {
      if (session.runtime->poll()) {
        completed     = true;
        impl_->status = "Effect receipt retained in its original case or "
                        "late-receipt inbox; no downstream work was dispatched";
      }
    } catch (const std::exception &error) {
      impl_->status = "Receipt retention pending: " + std::string(error.what());
    }
  }
  if (auto completion = impl_->join.poll()) {
    completed    = true;
    auto pending = std::exchange(impl_->pendingJoin, std::nullopt);
    if (!pending || pending->generation != impl_->generation ||
        !impl_->evidence.isOpen()) {
      impl_->status = "Stale join completion discarded after case close";
    } else if (completion->cancelled) {
      impl_->status = "Join cancelled; no result retained";
    } else if (!completion->result) {
      impl_->status = "Join failed: " + completion->result.error().message;
    } else {
      try {
        const auto &result  = *completion->result;
        const auto retained = retainSqliteObservationJoin(
            impl_->evidence, pending->prepared, result);
        if (!retained) throw std::invalid_argument(retained.error().message);
        impl_->evidence.durableSave();
        impl_->inspect(retained->queryRun);
        impl_->status =
            "Query run #" + std::to_string(retained->queryRun) + ": " +
            std::to_string(retained->occurrences.size()) +
            " occurrence pairs; input coverage " +
            std::string(vql::relationCoverageName(result.inputCoverage));
        if (result.truncated)
          impl_->status += "; computation truncated: " +
                           result.gap.value_or("budget exhausted");
      } catch (const std::exception &error) {
        impl_->status = "Join retention failed: " + std::string(error.what());
      }
    }
  }
  if (auto completion = impl_->observationWindow.poll()) {
    completed    = true;
    auto pending = std::exchange(impl_->pendingWindow, std::nullopt);
    if (!pending || pending->generation != impl_->generation ||
        !impl_->evidence.isOpen() ||
        pending->document != impl_->evidence.store().documentId().str()) {
      impl_->status = "Stale window completion discarded after case change";
    } else if (completion->cancelled) {
      impl_->status = "Window evaluation cancelled; no result retained";
    } else if (!completion->result) {
      const auto &error = completion->result.error();
      if (error.code == WindowCompositionErrorCode::LateDataRejected) {
        try {
          const auto retained = retainRejectedObservationWindow(
              impl_->evidence, pending->prepared, error);
          if (!retained) {
            impl_->status =
                "Rejected window retention failed: " + retained.error().message;
          } else {
            impl_->evidence.durableSave();
            impl_->inspect(retained->queryRun);
            impl_->status =
                "Window query run #" + std::to_string(retained->queryRun) +
                " rejected late data; no successful members were retained";
          }
        } catch (const std::exception &failure) {
          impl_->status = "Rejected window retention failed: " +
                          std::string(failure.what());
        }
      } else {
        impl_->status = "Window evaluation failed: " + error.message;
      }
    } else {
      try {
        const auto &result = *completion->result;
        const auto retained =
            retainObservationWindow(impl_->evidence, pending->prepared, result);
        if (!retained) {
          impl_->status =
              "Window retention failed: " + retained.error().message;
        } else {
          impl_->evidence.durableSave();
          impl_->inspect(retained->queryRun);
          impl_->status =
              "Window query run #" + std::to_string(retained->queryRun) + ": " +
              std::to_string(retained->members.size()) +
              " members, input coverage " +
              std::string(vql::relationCoverageName(result.inputCoverage)) +
              (result.finalized
                   ? "; declared watermark reached the window end"
                   : "; declared watermark has not reached the window end");
          if (!result.sourceGaps.empty())
            impl_->status +=
                "; source gaps " + std::to_string(result.sourceGaps.size());
          if (result.policyGap)
            impl_->status += "; policy gap: " + *result.policyGap;
          if (result.truncated)
            impl_->status += "; computation truncated: " +
                             result.computationGap.value_or("budget exhausted");
        }
      } catch (const std::exception &error) {
        impl_->status = "Window retention failed: " + std::string(error.what());
      }
    }
  }
  if (auto result = impl_->communicationRead.poll()) {
    completed = true;
    const auto pending =
        std::exchange(impl_->communicationPending, std::nullopt);
    if (!pending || pending->generation != impl_->generation ||
        !impl_->evidence.isOpen()) {
      impl_->status = "Stale communication read discarded after case close";
    } else {
      try {
        const auto retained = retainFediverseRead(impl_->evidence, *result);
        std::ignore         = impl_->evidence.appendRecord(
            "communication-acquisition", "Explicit public read", {},
            pending->source, {}, {{"d.evidence.target", retained.observation}});
        impl_->evidence.save();
        impl_->inspect(retained.observation);
        impl_->status = "Communication observation #" +
                        std::to_string(retained.observation) + " " +
                        stateName(result->observation.state) + ": " +
                        result->observation.coverage;
      } catch (const std::exception &error) {
        impl_->status =
            "Communication retention failed: " + std::string(error.what());
      }
    }
  }
  if (auto completion = impl_->mailRead.poll()) {
    completed          = true;
    const auto pending = std::exchange(impl_->mailPending, std::nullopt);
    if (!pending || pending->generation != impl_->generation ||
        !impl_->evidence.isOpen()) {
      impl_->status = "Stale IMAP read discarded after case close";
    } else {
      try {
        auto &observation = completion->observation;
        if (completion->cancelled &&
            observation.state == CaptureState::Complete) {
          observation.state = CaptureState::Cancelled;
          observation.coverage += "; cancelled before owner retention";
        }
        const auto retained =
            retainMailObservation(impl_->evidence, observation);
        std::ignore = impl_->evidence.appendRecord(
            "communication-acquisition", "Explicit IMAP read", {},
            pending->source, {}, {{"d.evidence.target", retained.observation}});
        impl_->evidence.durableSave();
        impl_->inspect(retained.observation);
        impl_->status =
            "Mail observation #" + std::to_string(retained.observation) + " " +
            stateName(observation.state) + ": " + observation.coverage;
      } catch (const std::exception &error) {
        impl_->status = "Mail retention failed: " + std::string(error.what());
      }
    }
  }
  if (!impl_->ircCommit) {
    if (auto completion = impl_->ircRead.poll()) {
      completed = true;
      if (!impl_->pendingIrc) {
        impl_->status = "Stale IRC completion discarded without ticket ack";
      } else {
        impl_->ircCommit =
            Impl::IrcCommit{*impl_->pendingIrc, std::move(*completion)};
      }
    }
  }
  if (impl_->ircCommit) {
    auto &commit       = *impl_->ircCommit;
    const auto session = impl_->ircSessions.find(commit.pending.connection);
    if (!impl_->evidence.isOpen() ||
        commit.pending.generation != impl_->generation ||
        commit.pending.document != impl_->evidence.store().documentId().str() ||
        session == impl_->ircSessions.end() ||
        session->second != commit.pending.session) {
      impl_->status = "Stale IRC completion discarded without ticket ack";
      impl_->pendingIrc.reset();
      impl_->ircCommit.reset();
      completed = true;
    } else {
      try {
        if (!commit.retained) {
          const auto retained = retainIrcObservation(
              impl_->evidence, commit.completion.observation);
          commit.observation = retained.observation;
          commit.retained    = true;
        }
        const auto operationName = [&] {
          switch (commit.pending.kind) {
          case Impl::PendingIrc::Kind::Connect:
            return std::string{"connect"};
          case Impl::PendingIrc::Kind::Join:
            return std::string{"join"};
          case Impl::PendingIrc::Kind::Live:
            return std::string{"live-read"};
          case Impl::PendingIrc::Kind::History:
            return std::string{"history-read"};
          }
          return std::string{"unknown"};
        }();
        if (!commit.linked) {
          const auto connection =
              impl_->ircConnection(commit.pending.connection);
          std::ignore = impl_->evidence.appendRecord(
              "communication-acquisition", "Explicit IRC " + operationName,
              stateName(commit.completion.observation.state), connection.id,
              {{"protocol", "irc"},
               {"operation", operationName},
               {"pending-read-budget",
                std::to_string(
                    commit.completion.pendingBounds.maxPendingReads)},
               {"pending-record-budget",
                std::to_string(
                    commit.completion.pendingBounds.maxPendingRecords)},
               {"pending-byte-budget",
                std::to_string(
                    commit.completion.pendingBounds.maxPendingBytes)}},
              {{"d.evidence.target", commit.observation}});
          if (commit.completion.join) {
            const auto &join = *commit.completion.join;
            static_cast<void>(impl_->evidence.appendRecord(
                "irc-join-receipt",
                join.joined ? "membership-confirmed"
                            : "membership-not-confirmed",
                join.serverReply +
                    (join.detail.empty() ? std::string{} : "\n" + join.detail),
                commit.observation,
                {{"joined", join.joined ? "true" : "false"}},
                {{"d.evidence.target", commit.observation}}));
          }
          commit.linked = true;
        }
        impl_->evidence.durableSave();
        if (commit.pending.kind == Impl::PendingIrc::Kind::Connect)
          impl_->ircConnected.insert_or_assign(commit.pending.connection,
                                               commit.completion.connected);
        if (commit.completion.readTicket && !commit.completion.cancelled &&
            !commit.acknowledged) {
          if (!commit.pending.session->acknowledgeRead(
                  *commit.completion.readTicket))
            throw std::runtime_error(
                "IRC read ticket acknowledgement was refused; retry poll");
          commit.acknowledged = true;
        }
        const auto observationId = commit.observation;
        const auto cancelled     = commit.completion.cancelled;
        const auto hasTicket     = commit.completion.readTicket.has_value();
        const auto connected =
            commit.pending.kind == Impl::PendingIrc::Kind::Connect
                ? commit.completion.connected
                : impl_->ircConnected[commit.pending.connection];
        const auto joined =
            commit.completion.join && commit.completion.join->joined;
        impl_->inspect(observationId);
        if (cancelled && hasTicket) {
          impl_->status = "Cancelled IRC read retained without advancing its "
                          "cursor; repeat the exact read to replay it";
        } else if (commit.pending.kind == Impl::PendingIrc::Kind::Connect) {
          impl_->status = connected
                              ? "IRC TLS session connected; join or read "
                                "explicitly"
                              : "IRC connection attempt retained without a "
                                "live session";
        } else if (commit.pending.kind == Impl::PendingIrc::Kind::Join) {
          impl_->status = joined
                              ? "IRC channel membership confirmed and retained"
                              : "IRC join result retained; membership was not "
                                "confirmed";
        } else {
          impl_->status =
              cancelled ? "IRC read cancellation retained; unacknowledged "
                          "events remain replayable"
                        : "IRC observation retained and its read ticket "
                          "acknowledged";
        }
        impl_->pendingIrc.reset();
        impl_->ircCommit.reset();
        completed = true;
      } catch (const std::exception &error) {
        impl_->status = "IRC retention pending: " + std::string(error.what());
        completed     = true;
      }
    }
  }
  auto result = impl_->capture.poll();
  if (!result) return completed;
  auto pending = std::exchange(impl_->pending, std::nullopt);
  if (!pending || pending->generation != impl_->generation ||
      !impl_->evidence.isOpen()) {
    impl_->status = "Stale read completion discarded after case close";
    return true;
  }
  try {
    const auto id = impl_->evidence.retain(pending->source, *result);
    impl_->evidence.save();
    impl_->inspect(id);
    impl_->status = "Observation #" + std::to_string(id) + " " +
                    stateName(result->state) + ": " + result->coverage;
  } catch (const std::exception &error) {
    impl_->status = "Capture retention failed: " + std::string(error.what());
  }
  return true;
}

void EvidenceController::select(RecordId id) {
  try {
    impl_->inspect(id);
  } catch (const std::exception &error) {
    impl_->status = error.what();
  }
}

Snapshot EvidenceController::snapshot() const {
  Snapshot result{
      .open = impl_->evidence.isOpen(),
      .busy = impl_->capture.busy() || impl_->communicationRead.busy() ||
              impl_->ircRead.busy() || impl_->pendingIrc || impl_->ircCommit ||
              impl_->mailRead.busy() || impl_->join.busy() ||
              impl_->observationWindow.busy() || impl_->workBusy(),
      .title      = impl_->evidence.title(),
      .path       = impl_->evidence.path().string(),
      .status     = impl_->status,
      .records    = {},
      .selected   = impl_->selected,
      .details    = impl_->details,
      .span       = impl_->span,
      .spanTarget = impl_->spanTarget};
  if (result.open) result.records = impl_->evidence.records();
  return result;
}

const EvidenceCase &EvidenceController::evidenceCase() const {
  return impl_->evidence;
}

} // namespace xanadu::evidence
