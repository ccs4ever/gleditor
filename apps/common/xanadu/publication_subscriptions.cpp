#include "publication_subscriptions.hpp"

#include "bencode.hpp"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <lmdb.h>
#include <map>
#include <mutex>
#include <thread>

namespace xanadu {
namespace {
using namespace std::chrono_literals;
using V     = bencode::Value;
using Phase = PublicationSubscriptionPhase;
void dbCheck(int rc) {
  if (rc != MDB_SUCCESS)
    throw std::runtime_error("Publication subscriptions: " +
                             std::string(mdb_strerror(rc)));
}
const V &field(const V &value, std::string_view name) {
  const auto &fields = value.asDict();
  const auto found   = fields.find(std::string(name));
  if (found == fields.end())
    throw PublicationSubscriptionsUnreadable(
        "Subscriptions format 1: missing " + std::string(name));
  return found->second;
}
bool boolean(const V &value) {
  const auto number = value.asInteger();
  if (number != 0 && number != 1)
    throw PublicationSubscriptionsUnreadable(
        "Subscriptions format 1: invalid boolean");
  return number != 0;
}
bool validTitle(std::string_view title) {
  return !title.empty() && title.size() <= 1024;
}
InfoHash hashField(const V &value) {
  const auto hash = InfoHash::parseHex(value.asString());
  if (!hash)
    throw PublicationSubscriptionsUnreadable(
        "Subscriptions format 1: invalid hash");
  return *hash;
}
void validateSnapshot(const PublicationDownloadStatus &snapshot,
                      const MutableLink &link, std::int64_t sequence,
                      const MicroversionId &version) {
  const auto actual = MutableLink::parse(snapshot.uri);
  if (snapshot.phase != PublicationDownloadPhase::Ready ||
      actual.key != link.key || actual.salt != link.salt ||
      snapshot.sequence != sequence || snapshot.version != version)
    throw PublicationSubscriptionsUnreadable(
        "Subscriptions format 1: retained snapshot does not match signed "
        "publication");
}
} // namespace

std::string_view publicationSubscriptionPhaseName(Phase phase) {
  switch (phase) {
  case Phase::Waiting:
    return "Waiting for newer publication";
  case Phase::Checking:
    return "Checking signed DHT name";
  case Phase::Downloading:
    return "Verifying update dependencies";
  case Phase::Failed:
    return "Update unavailable; retry scheduled";
  case Phase::Paused:
    return "Notifications paused";
  }
  return "Unknown";
}

struct PublicationSubscriptions::Impl {
  struct Job {
    PublicationSubscriptionStatus status;
    std::string pending;
    MutablePointer observed;
    std::chrono::steady_clock::time_point next{};
    std::stop_source cancel;
    std::uint64_t generation{};
  };
  Options options;
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env{nullptr,
                                                         mdb_env_close};
  MDB_dbi db{};
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::map<std::string, Job> jobs;
  std::jthread worker;

  static std::string encode(const Job &job) {
    const auto &s = job.status;
    bencode::List notices;
    for (const auto &n : s.notices)
      notices.push_back(
          V::dict({{"sequence", V::integer(n.sequence)},
                   {"previous_sequence", V::integer(n.previousSequence)},
                   {"hash", V::string(n.hash.hex())},
                   {"version", V::string(n.version.str())},
                   {"previous_version", V::string(n.previousVersion.str())},
                   {"title", V::string(n.title)},
                   {"snapshot", V::string(n.snapshotId)},
                   {"previous_snapshot", V::string(n.previousSnapshotId)},
                   {"delivered", V::integer(n.delivered)},
                   {"acknowledged", V::integer(n.acknowledged)}}));
    return "XPS1" +
           V::dict({{"uri", V::string(s.uri)},
                    {"title", V::string(s.title)},
                    {"sequence", V::integer(s.sequence)},
                    {"hash", V::string(s.hash.hex())},
                    {"version", V::string(s.version.str())},
                    {"snapshot", V::string(s.snapshotId)},
                    {"enabled", V::integer(s.enabled)},
                    {"notices", V::list(std::move(notices))},
                    {"pending", V::string(job.pending)},
                    {"observed_hash", V::string(job.observed.hash.hex())},
                    {"observed_sequence", V::integer(job.observed.sequence)}})
               .encode();
  }
  Job decode(std::string_view bytes) const {
    if (!bytes.starts_with("XPS"))
      throw PublicationSubscriptionsUnreadable(
          "Subscriptions format 1: XPS signature missing");
    if (bytes.size() < 4 || bytes[3] != '1')
      throw PublicationSubscriptionsUnreadable(
          "Subscriptions version 1 expected, got byte " +
          (bytes.size() < 4
               ? std::string("missing")
               : std::to_string(static_cast<unsigned char>(bytes[3]))));
    try {
      const auto root = bencode::decode(bytes.substr(4));
      if (root.asDict().size() != 11)
        throw std::runtime_error("unexpected fields");
      Job job;
      auto &s         = job.status;
      s.uri           = field(root, "uri").asString();
      const auto link = MutableLink::parse(s.uri);
      if (link.key.isZero() || link.salt.empty() || link.salt.size() > 64)
        throw std::runtime_error("invalid mutable name");
      s.id         = link.target().hex();
      s.title      = field(root, "title").asString();
      s.sequence   = field(root, "sequence").asInteger();
      s.hash       = hashField(field(root, "hash"));
      s.version    = MicroversionId::parse(field(root, "version").asString());
      s.snapshotId = field(root, "snapshot").asString();
      s.enabled    = boolean(field(root, "enabled"));
      s.phase      = s.enabled ? Phase::Waiting : Phase::Paused;
      job.pending  = field(root, "pending").asString();
      job.observed.hash     = hashField(field(root, "observed_hash"));
      job.observed.sequence = field(root, "observed_sequence").asInteger();
      if (s.sequence < 0 || s.title.empty() || s.title.size() > 1024 ||
          (!job.pending.empty() &&
           (job.observed.hash.isZero() || job.observed.sequence <= s.sequence)))
        throw std::runtime_error("invalid sequence, title or pending pointer");
      validateSnapshot(options.inbox->status(s.snapshotId), link, s.sequence,
                       s.version);
      const auto &notices = field(root, "notices").asList();
      if (notices.size() > options.maximumNotices)
        throw std::runtime_error("notice limit exceeded");
      std::int64_t previous{-1};
      for (const auto &record : notices) {
        if (record.asDict().size() != 10)
          throw std::runtime_error("unexpected notice fields");
        PublicationUpdateNotice n;
        n.sequence         = field(record, "sequence").asInteger();
        n.previousSequence = field(record, "previous_sequence").asInteger();
        n.hash             = hashField(field(record, "hash"));
        n.version = MicroversionId::parse(field(record, "version").asString());
        n.previousVersion =
            MicroversionId::parse(field(record, "previous_version").asString());
        n.title              = field(record, "title").asString();
        n.snapshotId         = field(record, "snapshot").asString();
        n.previousSnapshotId = field(record, "previous_snapshot").asString();
        n.delivered          = boolean(field(record, "delivered"));
        n.acknowledged       = boolean(field(record, "acknowledged"));
        if (n.sequence <= previous || n.sequence > s.sequence ||
            n.previousSequence < 0 || n.previousSequence >= n.sequence ||
            n.hash.isZero() || n.title.empty() || n.title.size() > 1024)
          throw std::runtime_error("invalid notice sequence or identity");
        validateSnapshot(options.inbox->status(n.snapshotId), link, n.sequence,
                         n.version);
        validateSnapshot(options.inbox->status(n.previousSnapshotId), link,
                         n.previousSequence, n.previousVersion);
        previous = n.sequence;
        s.notices.push_back(std::move(n));
      }
      return job;
    } catch (const PublicationSubscriptionsUnreadable &) {
      throw;
    } catch (const std::exception &error) {
      throw PublicationSubscriptionsUnreadable("Subscriptions format 1: " +
                                               std::string(error.what()));
    }
  }
  void save(const Job &job) {
    if (!validTitle(job.status.title) ||
        std::ranges::any_of(job.status.notices, [](const auto &notice) {
          return !validTitle(notice.title);
        }))
      throw std::invalid_argument(
          "Publication subscription title must contain 1 to 1024 bytes");
    auto encoded = encode(job);
    auto name    = job.status.id;
    if (encoded.size() > 1024 * 1024)
      throw PublicationSubscriptionsUnreadable(
          "Subscriptions format 1: record byte limit exceeded");
    MDB_txn *raw{};
    dbCheck(mdb_txn_begin(env.get(), nullptr, 0, &raw));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(raw, mdb_txn_abort);
    MDB_val key{name.size(), name.data()};
    MDB_val value{encoded.size(), encoded.data()};
    dbCheck(mdb_put(raw, db, &key, &value, 0));
    dbCheck(mdb_txn_commit(txn.release()));
  }
  explicit Impl(Options supplied) : options(std::move(supplied)) {
    if (options.directory.empty() || !options.inbox ||
        options.pollInterval <= 0ms || options.retryInterval <= 0ms ||
        !options.maximumSubscriptions || !options.maximumNotices)
      throw std::invalid_argument("Invalid publication subscription options");
    std::filesystem::create_directories(options.directory);
    MDB_env *raw{};
    dbCheck(mdb_env_create(&raw));
    env.reset(raw);
    dbCheck(mdb_env_set_mapsize(raw, 64ULL * 1024 * 1024));
    dbCheck(mdb_env_open(raw, options.directory.string().c_str(), 0, 0600));
    MDB_txn *transaction{};
    dbCheck(mdb_txn_begin(raw, nullptr, 0, &transaction));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(transaction,
                                                           mdb_txn_abort);
    dbCheck(mdb_dbi_open(transaction, nullptr, 0, &db));
    MDB_cursor *cursor{};
    dbCheck(mdb_cursor_open(transaction, db, &cursor));
    {
      const std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> owned(
          cursor, mdb_cursor_close);
      MDB_val key{}, value{};
      int rc;
      while ((rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT)) ==
             MDB_SUCCESS) {
        if (jobs.size() >= options.maximumSubscriptions ||
            value.mv_size > 1024 * 1024)
          throw PublicationSubscriptionsUnreadable(
              "Subscriptions format 1: record/count limit exceeded");
        auto job =
            decode({static_cast<const char *>(value.mv_data), value.mv_size});
        if (std::string_view(static_cast<const char *>(key.mv_data),
                             key.mv_size) != job.status.id)
          throw PublicationSubscriptionsUnreadable(
              "Subscriptions format 1: name/key mismatch");
        jobs.emplace(job.status.id, std::move(job));
      }
      if (rc != MDB_NOTFOUND) dbCheck(rc);
    }
    dbCheck(mdb_txn_commit(txn.release()));
    worker = std::jthread([this](std::stop_token stop) { work(stop); });
  }
  void cancelPending(const std::string &id) {
    try {
      options.inbox->cancel(id);
    } catch (const std::out_of_range &) {
    }
  }
  ~Impl() {
    {
      const std::scoped_lock lock(guard);
      worker.request_stop();
      for (auto &[id, job] : jobs) {
        job.cancel.request_stop();
        if (!job.pending.empty()) cancelPending(job.pending);
      }
    }
    changed.notify_all();
    worker.join();
  }
  void step(const std::string &id, Job copied, std::stop_token stop) {
    const std::stop_callback cancel(stop,
                                    [&] { copied.cancel.request_stop(); });
    try {
      if (copied.pending.empty()) {
        if (!options.makeTransport)
          throw std::runtime_error(
              "No publication swarm configured; resume when connected");
        auto transport = options.makeTransport();
        if (!transport)
          throw std::runtime_error("Subscription transport unavailable");
        const auto link = MutableLink::parse(copied.status.uri);
        const auto pointer =
            transport->resolve(link, copied.cancel.get_token());
        if (pointer.hash.isZero() ||
            pointer.sequence < copied.status.sequence ||
            (pointer.sequence == copied.status.sequence &&
             !copied.status.hash.isZero() &&
             pointer.hash != copied.status.hash))
          throw std::runtime_error("Signed publication pointer rolls back or "
                                   "conflicts with an accepted sequence");
        const std::scoped_lock lock(guard);
        auto &job = jobs.at(id);
        if (job.generation != copied.generation || !job.status.enabled ||
            stop.stop_requested())
          return;
        if (pointer.sequence == job.status.sequence) {
          job.status.phase = Phase::Waiting;
          job.status.error.clear();
          job.next = std::chrono::steady_clock::now() + options.pollInterval;
        } else {
          job.observed     = pointer;
          job.pending      = options.inbox->submit(link, pointer);
          job.status.phase = Phase::Downloading;
          job.next         = std::chrono::steady_clock::now() + 100ms;
          save(job);
        }
      } else {
        PublicationDownloadStatus download;
        try {
          download = options.inbox->status(copied.pending);
        } catch (const std::out_of_range &) {
          // A crash interrupted the inbox attempt. Start from the signed
          // name again; no incomplete snapshot can advance the high water.
          const std::scoped_lock lock(guard);
          auto &job = jobs.at(id);
          if (job.generation != copied.generation) return;
          job.pending.clear();
          save(job);
          job.next = {};
          return;
        }
        const std::scoped_lock lock(guard);
        auto &job = jobs.at(id);
        if (job.generation != copied.generation || !job.status.enabled ||
            stop.stop_requested())
          return;
        if (download.phase == PublicationDownloadPhase::Ready) {
          const auto link = MutableLink::parse(job.status.uri);
          validateSnapshot(download, link, download.sequence, download.version);
          if (download.sequence < job.observed.sequence ||
              download.sequence <= job.status.sequence ||
              (download.sequence == job.observed.sequence &&
               !download.manifestHash.isZero() &&
               download.manifestHash != job.observed.hash))
            throw std::runtime_error(
                "Verified update does not match the observed sequence");
          if (download.manifestHash.isZero() &&
              download.sequence != job.observed.sequence) {
            job.pending.clear();
            save(job);
            job.next = {};
            return;
          }
          if (!validTitle(download.title)) {
            job.pending.clear();
            save(job);
            throw std::invalid_argument(
                "Publication subscription title must contain 1 to 1024 bytes");
          }
          auto accepted = job;
          std::erase_if(accepted.status.notices, [](const auto &n) {
            return n.acknowledged && n.delivered;
          });
          if (accepted.status.notices.size() >= options.maximumNotices)
            throw std::runtime_error(
                "Review pending updates before accepting more publications");
          const auto hash = download.manifestHash.isZero()
                                ? job.observed.hash
                                : download.manifestHash;
          accepted.status.notices.push_back(
              {.sequence           = download.sequence,
               .previousSequence   = accepted.status.sequence,
               .hash               = hash,
               .version            = download.version,
               .previousVersion    = accepted.status.version,
               .title              = download.title,
               .snapshotId         = download.id,
               .previousSnapshotId = accepted.status.snapshotId});
          accepted.status.sequence   = download.sequence;
          accepted.status.hash       = hash;
          accepted.status.version    = download.version;
          accepted.status.title      = download.title;
          accepted.status.snapshotId = download.id;
          accepted.pending.clear();
          accepted.status.phase = Phase::Waiting;
          accepted.status.error.clear();
          accepted.next =
              std::chrono::steady_clock::now() + options.pollInterval;
          save(accepted);
          job = std::move(accepted);
        } else if (download.phase == PublicationDownloadPhase::Failed ||
                   download.phase == PublicationDownloadPhase::Cancelled) {
          if (copied.status.phase == Phase::Failed) {
            options.inbox->retry(job.pending);
            job.status.phase = Phase::Downloading;
            job.status.error.clear();
            job.next = std::chrono::steady_clock::now() + 100ms;
          } else
            throw std::runtime_error(download.error.empty()
                                         ? "Update transfer cancelled"
                                         : download.error);
        } else {
          job.status.phase = Phase::Downloading;
          job.next         = std::chrono::steady_clock::now() + 100ms;
        }
      }
    } catch (const std::exception &error) {
      const std::scoped_lock lock(guard);
      auto &job = jobs.at(id);
      if (job.generation != copied.generation || stop.stop_requested()) return;
      job.status.phase = job.status.enabled ? Phase::Failed : Phase::Paused;
      job.status.error = error.what();
      job.next = std::chrono::steady_clock::now() + options.retryInterval;
    }
    changed.notify_all();
  }
  void work(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(guard);
      changed.wait_for(lock, 100ms, [&] {
        return stop.stop_requested() ||
               std::ranges::any_of(jobs, [](const auto &item) {
                 return item.second.status.enabled &&
                        item.second.next <= std::chrono::steady_clock::now();
               });
      });
      if (stop.stop_requested()) return;
      auto found     = jobs.end();
      const auto now = std::chrono::steady_clock::now();
      for (auto candidate = jobs.begin(); candidate != jobs.end(); ++candidate)
        if (candidate->second.status.enabled && candidate->second.next <= now &&
            (found == jobs.end() ||
             candidate->second.next < found->second.next))
          found = candidate;
      if (found == jobs.end()) continue;
      const auto id  = found->first;
      const auto job = found->second;
      if (job.pending.empty()) found->second.status.phase = Phase::Checking;
      changed.notify_all();
      lock.unlock();
      step(id, job, stop);
    }
  }
};

PublicationSubscriptions::PublicationSubscriptions(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PublicationSubscriptions::~PublicationSubscriptions() = default;
std::string PublicationSubscriptions::subscribe(std::string_view downloadedId) {
  const auto baseline = impl_->options.inbox->status(downloadedId);
  if (baseline.phase != PublicationDownloadPhase::Ready ||
      baseline.sequence < 0)
    throw std::logic_error(
        "Open a verified publication before enabling notifications");
  if (!validTitle(baseline.title))
    throw std::invalid_argument(
        "Publication subscription title must contain 1 to 1024 bytes");
  const auto link = MutableLink::parse(baseline.uri);
  const auto id   = link.target().hex();
  const std::scoped_lock lock(impl_->guard);
  auto found = impl_->jobs.find(id);
  if (found == impl_->jobs.end()) {
    if (impl_->jobs.size() >= impl_->options.maximumSubscriptions)
      throw std::runtime_error("Publication subscription limit reached");
    Impl::Job job;
    job.status.id         = id;
    job.status.uri        = link.uri();
    job.status.title      = baseline.title;
    job.status.sequence   = baseline.sequence;
    job.status.version    = baseline.version;
    job.status.hash       = baseline.manifestHash;
    job.status.snapshotId = baseline.id;
    impl_->save(job);
    impl_->jobs.emplace(id, std::move(job));
  } else {
    auto next           = found->second;
    next.status.enabled = true;
    next.status.phase   = Phase::Waiting;
    next.cancel         = std::stop_source{};
    ++next.generation;
    next.next = {};
    impl_->save(next);
    found->second.cancel.request_stop();
    found->second = std::move(next);
  }
  impl_->changed.notify_all();
  return id;
}
void PublicationSubscriptions::setEnabled(std::string_view id, bool enabled) {
  const std::scoped_lock lock(impl_->guard);
  auto &current       = impl_->jobs.at(std::string(id));
  auto next           = current;
  next.status.enabled = enabled;
  next.status.phase   = enabled ? Phase::Waiting : Phase::Paused;
  next.cancel         = std::stop_source{};
  ++next.generation;
  next.next = {};
  impl_->save(next);
  current.cancel.request_stop();
  if (!enabled && !current.pending.empty())
    impl_->cancelPending(current.pending);
  current = std::move(next);
  impl_->changed.notify_all();
}
void PublicationSubscriptions::checkNow(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (!job.status.enabled)
    throw std::logic_error("Resume notifications before checking for updates");
  job.next = {};
  if (job.pending.empty()) {
    job.status.phase = Phase::Waiting;
    job.status.error.clear();
  }
  impl_->changed.notify_all();
}
void PublicationSubscriptions::acknowledge(std::string_view id,
                                           std::int64_t sequence) {
  const std::scoped_lock lock(impl_->guard);
  auto &job        = impl_->jobs.at(std::string(id));
  auto next        = job;
  const auto found = std::ranges::find(next.status.notices, sequence,
                                       &PublicationUpdateNotice::sequence);
  if (found == next.status.notices.end())
    throw std::out_of_range("No verified update at this sequence");
  found->acknowledged = true;
  found->delivered    = true;
  impl_->save(next);
  job = std::move(next);
}
std::vector<PublicationSubscriptionStatus>
PublicationSubscriptions::statuses() const {
  const std::scoped_lock lock(impl_->guard);
  std::vector<PublicationSubscriptionStatus> result;
  for (const auto &[id, job] : impl_->jobs) result.push_back(job.status);
  return result;
}
PublicationSubscriptionStatus
PublicationSubscriptions::status(std::string_view id) const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->jobs.at(std::string(id)).status;
}
std::vector<std::pair<std::string, PublicationUpdateNotice>>
PublicationSubscriptions::takeNotifications(std::size_t maximum) {
  const std::scoped_lock lock(impl_->guard);
  std::vector<std::pair<std::string, PublicationUpdateNotice>> result;
  for (auto &[id, job] : impl_->jobs) {
    if (result.size() >= maximum) break;
    if (!job.status.enabled) continue;
    if (!std::ranges::any_of(job.status.notices, [](const auto &n) {
          return !n.delivered && !n.acknowledged;
        }))
      continue;
    auto next           = job;
    std::size_t claimed = 0;
    for (auto &notice : next.status.notices)
      if (!notice.delivered && !notice.acknowledged &&
          result.size() + claimed < maximum) {
        notice.delivered = true;
        ++claimed;
      }
    if (!claimed) continue;
    impl_->save(next);
    for (std::size_t i = 0; i < next.status.notices.size(); ++i)
      if (!job.status.notices[i].delivered && next.status.notices[i].delivered)
        result.emplace_back(id, next.status.notices[i]);
    job = std::move(next);
  }
  return result;
}
bool PublicationSubscriptions::waitForSequence(
    std::string_view id, std::int64_t sequence,
    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).status.sequence >= sequence;
  });
}
bool PublicationSubscriptions::waitForPhase(
    std::string_view id, PublicationSubscriptionPhase phase,
    std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).status.phase == phase;
  });
}
} // namespace xanadu
