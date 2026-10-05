#include "publication_outbox.hpp"

#include <lmdb.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include <gleditor/logging.hpp>

#include "bencode.hpp"
#include "resolver.hpp"

namespace xanadu {
namespace {
std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot read publication seed: " + path.string());
  std::string bytes{std::istreambuf_iterator<char>(in),
                    std::istreambuf_iterator<char>()};
  if (in.bad())
    throw std::runtime_error("cannot read publication seed: " + path.string());
  return bytes;
}

bool relativePath(const std::filesystem::path &path) {
  if (path.empty() || path.has_root_path() ||
      path.string().find('\0') != std::string::npos ||
      path.string().find('\\') != std::string::npos)
    return false;
  for (const auto &component : path)
    if (component == "." || component == "..") return false;
  return true;
}

PublicationSeed checkedSeed(const InfoHash &hash,
                            const std::filesystem::path &root) {
  const auto encoded = readFile(root / "metainfo.torrent");
  const auto meta    = Metainfo::parse(encoded);
  if (meta.hash() != hash || !relativePath(meta.name()) ||
      std::filesystem::path(meta.name()).has_parent_path())
    throw std::runtime_error("publication seed metainfo mismatch: " +
                             hash.hex());
  const auto data          = root / meta.name();
  const auto canonicalRoot = std::filesystem::weakly_canonical(data);
  if (!relativePath(canonicalRoot.lexically_relative(
          std::filesystem::weakly_canonical(root))))
    throw std::runtime_error("publication seed data escapes its directory");
  if (meta.pieceCount() != meta.totalLength() / meta.pieceLength() +
                               (meta.totalLength() % meta.pieceLength() != 0))
    throw std::runtime_error("publication seed has an incomplete piece table");
  std::set<std::string> paths;
  for (const auto &file : meta.files()) {
    if (!paths.insert(file.path).second)
      throw std::runtime_error("publication seed has duplicate file paths");
    if (!relativePath(file.path) || file.path.find('\\') != std::string::npos)
      throw std::runtime_error("unsafe publication seed path: " + hash.hex());
    const auto full = std::filesystem::weakly_canonical(data / file.path);
    if (!relativePath(full.lexically_relative(canonicalRoot)))
      throw std::runtime_error("publication seed escapes its directory: " +
                               hash.hex());
    if (!std::filesystem::is_regular_file(full) ||
        std::filesystem::file_size(full) != file.length)
      throw std::runtime_error("publication seed file missing or truncated: " +
                               hash.hex());
  }
  DirectoryContentSource source;
  source.add(encoded, root.string());
  for (std::size_t piece = 0; piece < meta.pieceCount(); ++piece) {
    const auto bytes = source.readStream(hash, piece * meta.pieceLength(),
                                         meta.lengthOfPiece(piece));
    if (!meta.verifyPiece(piece, bytes))
      throw std::runtime_error("publication seed piece " +
                               std::to_string(piece) +
                               " failed verification: " + hash.hex());
  }
  return {.hash     = hash,
          .metainfo = encoded,
          .savePath = root,
          .bytes    = meta.totalLength()};
}

std::string jobId(const Publication &pub) {
  return pub.publisher.hex() + ":" + toHex(pub.salt) + ":" +
         std::to_string(pub.sequence);
}

struct Job {
  Publication publication;
  std::vector<std::filesystem::path> roots;
  bool announce{};
  PublicationJobStatus status;
  std::vector<PublicationSeed> seeds;
  std::chrono::steady_clock::time_point nextAttempt{};
};

std::string encodeJob(const Job &job) {
  bencode::List roots;
  for (const auto &root : job.roots)
    roots.push_back(bencode::Value::string(root.string()));
  return "XPO1" +
         bencode::Value::dict(
             {
                 {"announce", bencode::Value::integer(job.announce)},
                 {"attempts", bencode::Value::integer(static_cast<std::int64_t>(
                                  job.status.attempts))},
                 {"error", bencode::Value::string(job.status.error)},
                 {"phase",
                  bencode::Value::integer(static_cast<int>(job.status.phase))},
                 {"publication",
                  bencode::Value::string(encodePublication(job.publication))},
                 {"roots", bencode::Value::list(std::move(roots))},
             })
             .encode();
}

Job decodeJob(std::string_view bytes) {
  if (bytes.size() < 4 || !bytes.starts_with("XPO"))
    throw PublicationOutboxUnreadable(
        "publication outbox format 1: XPO signature missing");
  if (bytes[3] != '1')
    throw PublicationOutboxUnreadable(
        "publication outbox version 1 expected (byte 49), got byte " +
        std::to_string(static_cast<unsigned char>(bytes[3])));
  try {
    const auto root     = bencode::decode(bytes.substr(4));
    const auto pub      = root.find("publication");
    const auto paths    = root.find("roots");
    const auto announce = root.find("announce");
    const auto phase    = root.find("phase");
    const auto attempts = root.find("attempts");
    const auto error    = root.find("error");
    if (!pub || !pub->isString() || !paths || !paths->isList() || !announce ||
        !announce->isInteger() || announce->asInteger() < 0 ||
        announce->asInteger() > 1 || !phase || !phase->isInteger() ||
        phase->asInteger() < 0 ||
        phase->asInteger() > static_cast<int>(PublicationPhase::Superseded) ||
        !attempts || !attempts->isInteger() || attempts->asInteger() < 0 ||
        !error || !error->isString())
      throw PublicationOutboxUnreadable(
          "publication outbox format 1: invalid fields");
    const auto decoded = decodePublication(pub->asString());
    if (!decoded)
      throw PublicationOutboxUnreadable(
          "publication outbox format 1: publication signature failed");
    Job job;
    job.publication     = *decoded;
    job.announce        = announce->asInteger() != 0;
    job.status.id       = jobId(*decoded);
    job.status.title    = decoded->title;
    job.status.sequence = decoded->sequence;
    job.status.phase    = static_cast<PublicationPhase>(phase->asInteger());
    job.status.attempts = static_cast<std::uint64_t>(attempts->asInteger());
    job.status.error    = error->asString();
    for (const auto &path : paths->asList()) {
      if (!path.isString() ||
          !std::filesystem::path(path.asString()).is_absolute())
        throw PublicationOutboxUnreadable(
            "publication outbox format 1: invalid seed root");
      job.roots.emplace_back(path.asString());
    }
    return job;
  } catch (const PublicationOutboxUnreadable &) {
    throw;
  } catch (const std::exception &) {
    throw PublicationOutboxUnreadable(
        "publication outbox format 1: malformed record");
  }
}

void dbCheck(int rc) {
  if (rc != MDB_SUCCESS)
    throw std::runtime_error("publication outbox: " +
                             std::string(mdb_strerror(rc)));
}

class SwarmTransport final : public PublicationTransport {
public:
  SwarmTransport(
      MutableKeys keys, SwarmContentSource::Options options,
      const std::vector<std::pair<std::string, std::uint16_t>> &nodes)
      : keys_(keys), source_(options), nodes_(nodes) {
    for (const auto &[host, port] : nodes) source_.addDhtNode(host, port);
  }
  void seed(const PublicationSeed &seed) override {
    if (seeded_.contains(seed.hash)) return;
    if (source_.addTorrent(seed.metainfo, seed.savePath.string(), true) !=
        seed.hash)
      throw std::runtime_error("publication seeding returned a different hash");
    seeded_.insert(seed.hash);
  }
  void announce(const Publication &pub, const InfoHash &hash) override {
    if (pub.publisher != keys_.publicKey)
      throw std::runtime_error(
          "publication key does not match transport identity");
    // DHT startup is asynchronous. Bootstrap introductions made before it
    // starts may be lost; retry only the explicitly configured nodes.
    for (const auto &[host, port] : nodes_) source_.addDhtNode(host, port);
    source_.publishMutable(keys_, pub.salt, hash, pub.sequence);
  }
  bool acknowledged(const Publication &pub, const InfoHash &hash) override {
    return source_.publicationAcknowledged(pub.publisher, pub.salt, hash,
                                           pub.sequence);
  }
  void poll() override { source_.poll(); }
  std::uint16_t listenPort() const override { return source_.listenPort(); }

private:
  MutableKeys keys_;
  SwarmContentSource source_;
  std::vector<std::pair<std::string, std::uint16_t>> nodes_;
  std::set<InfoHash> seeded_;
};
} // namespace

std::vector<PublicationSeed>
reviewPublicationDependencies(const Publication &pub,
                              const std::vector<std::filesystem::path> &roots) {
  if (!verifyPublication(pub))
    throw std::runtime_error("publication signature failed");
  std::set<InfoHash> required;
  for (const auto &[name, scroll] : pub.scrolls) {
    (void)name;
    for (const auto &segment : scroll.segments)
      required.insert(segment.torrent);
  }
  for (const auto &segment : pub.opsSegments) required.insert(segment.torrent);
  std::vector<PublicationSeed> seeds;
  for (const auto &hash : required) {
    bool found = false;
    for (const auto &root : roots) {
      const auto dir = root / hash.hex();
      if (!std::filesystem::exists(dir / "metainfo.torrent")) continue;
      seeds.push_back(checkedSeed(hash, dir));
      found = true;
      break;
    }
    if (!found)
      throw std::runtime_error(
          "publication dependency is not available locally: " + hash.hex());
  }
  const auto checkSegment = [&](const ScrollSegment &segment) {
    const auto seed =
        std::ranges::find(seeds, segment.torrent, &PublicationSeed::hash);
    const auto meta = Metainfo::parse(seed->metainfo);
    if (segment.fileIndex >= meta.files().size())
      throw std::runtime_error("publication segment has an invalid file index");
    const auto &file = meta.files()[segment.fileIndex];
    if (segment.path != file.path || segment.streamOffset < file.offset ||
        segment.streamOffset - file.offset > file.length ||
        segment.length > file.length - (segment.streamOffset - file.offset) ||
        segment.length > UINT64_MAX - segment.at)
      throw std::runtime_error(
          "publication segment does not fit its seed file");
  };
  for (const auto &[name, scroll] : pub.scrolls) {
    if (name != scrollKey(scroll))
      throw std::runtime_error(
          "publication scroll key does not match its descriptor");
    for (const auto &segment : scroll.segments) checkSegment(segment);
  }
  for (const auto &segment : pub.opsSegments) checkSegment(segment);
  const auto checkSpan = [&](const GlobalSpan &span) {
    if (span.scroll == breakMarkerKey) return;
    const auto scroll = pub.scrolls.find(span.scroll);
    if (scroll == pub.scrolls.end() || span.length > UINT64_MAX - span.start)
      throw std::runtime_error("publication span has no valid scroll");
    auto at = span.start;
    while (at < span.end()) {
      const auto segment =
          std::ranges::find_if(scroll->second.segments, [at](const auto &item) {
            return item.covers(at);
          });
      if (segment == scroll->second.segments.end())
        throw std::runtime_error("publication span has an unsealed range");
      at = std::min(segment->end(), span.end());
    }
  };
  for (const auto &span : pub.pieces) checkSpan(span);
  for (const auto &link : pub.links) {
    for (const auto &span : link.left) checkSpan(span);
    for (const auto &span : link.right) checkSpan(span);
  }
  return seeds;
}

std::string_view publicationPhaseName(PublicationPhase phase) {
  switch (phase) {
  case PublicationPhase::Queued:
    return "Queued";
  case PublicationPhase::LocalReady:
    return "Local ready";
  case PublicationPhase::NeedsVerification:
    return "Needs identity verification";
  case PublicationPhase::Seeding:
    return "Seeding dependencies";
  case PublicationPhase::AwaitingDht:
    return "Awaiting DHT acknowledgement";
  case PublicationPhase::Published:
    return "Published";
  case PublicationPhase::Failed:
    return "Failed; retry available";
  case PublicationPhase::Superseded:
    return "Superseded";
  }
  return "Unknown";
}

struct PublicationOutbox::Impl {
  Options options;
  std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env{nullptr,
                                                         mdb_env_close};
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::deque<Job> submitted;
  std::deque<std::string> retries;
  std::map<std::string, Job> jobs;
  std::vector<PublicationJobStatus> visible;
  std::atomic<std::uint16_t> port{0};
  std::string workerError;
  std::jthread worker;

  explicit Impl(Options value) : options(std::move(value)) {
    if (options.directory.empty() || options.retryInterval.count() <= 0)
      throw std::invalid_argument("invalid publication outbox options");
    std::filesystem::create_directories(options.directory);
    MDB_env *raw = nullptr;
    dbCheck(mdb_env_create(&raw));
    env.reset(raw);
    dbCheck(
        mdb_env_open(env.get(), options.directory.string().c_str(), 0, 0600));
    MDB_txn *rawTxn = nullptr;
    dbCheck(mdb_txn_begin(env.get(), nullptr, MDB_RDONLY, &rawTxn));
    const std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(rawTxn,
                                                                 mdb_txn_abort);
    MDB_dbi db;
    dbCheck(mdb_dbi_open(txn.get(), nullptr, 0, &db));
    MDB_cursor *rawCursor = nullptr;
    dbCheck(mdb_cursor_open(txn.get(), db, &rawCursor));
    const std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> cursor(
        rawCursor, mdb_cursor_close);
    MDB_val key{}, data{};
    int rc;
    while ((rc = mdb_cursor_get(cursor.get(), &key, &data, MDB_NEXT)) ==
           MDB_SUCCESS) {
      auto job =
          decodeJob({static_cast<const char *>(data.mv_data), data.mv_size});
      if (std::string_view(static_cast<const char *>(key.mv_data),
                           key.mv_size) != job.status.id)
        throw PublicationOutboxUnreadable(
            "publication outbox format 1: job identity mismatch");
      job.status.phase = PublicationPhase::Queued;
      jobs.emplace(job.status.id, std::move(job));
    }
    if (rc != MDB_NOTFOUND) dbCheck(rc);
    refresh();
    worker = std::jthread([this](std::stop_token stop) {
      try {
        work(stop);
      } catch (const std::exception &error) {
        const std::scoped_lock lock(guard);
        workerError = error.what();
        for (auto &[id, job] : jobs) {
          (void)id;
          job.status.phase = PublicationPhase::Failed;
          job.status.error = error.what();
        }
        refresh();
      }
    });
  }
  ~Impl() {
    worker.request_stop();
    changed.notify_all();
  }

  void write(const Job &job) {
    MDB_txn *raw = nullptr;
    dbCheck(mdb_txn_begin(env.get(), nullptr, 0, &raw));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(raw, mdb_txn_abort);
    MDB_dbi db;
    dbCheck(mdb_dbi_open(txn.get(), nullptr, 0, &db));
    auto id     = job.status.id;
    auto record = encodeJob(job);
    MDB_val key{id.size(), id.data()}, value{record.size(), record.data()};
    dbCheck(mdb_put(txn.get(), db, &key, &value, 0));
    dbCheck(mdb_txn_commit(txn.release()));
  }
  bool insert(const Job &job) {
    MDB_txn *raw = nullptr;
    dbCheck(mdb_txn_begin(env.get(), nullptr, 0, &raw));
    std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(raw, mdb_txn_abort);
    MDB_dbi db;
    dbCheck(mdb_dbi_open(txn.get(), nullptr, 0, &db));
    auto id = job.status.id;
    MDB_val key{id.size(), id.data()}, value{};
    const auto found = mdb_get(txn.get(), db, &key, &value);
    if (found == MDB_SUCCESS) {
      const auto prior =
          decodeJob({static_cast<const char *>(value.mv_data), value.mv_size});
      if (encodePublication(prior.publication) !=
              encodePublication(job.publication) ||
          prior.announce != job.announce)
        throw std::invalid_argument(
            "a different publication already owns this sequence");
      return false;
    }
    if (found != MDB_NOTFOUND) dbCheck(found);
    auto record = encodeJob(job);
    value       = MDB_val{record.size(), record.data()};
    dbCheck(mdb_put(txn.get(), db, &key, &value, 0));
    dbCheck(mdb_txn_commit(txn.release()));
    return true;
  }
  void refresh() {
    visible.clear();
    for (const auto &[id, job] : jobs) {
      (void)id;
      visible.push_back(job.status);
    }
    changed.notify_all();
  }
  void update(Job &job, PublicationPhase phase) {
    job.status.phase = phase;
    write(job);
    const std::scoped_lock lock(guard);
    refresh();
  }
  void prepare(Job &job, const std::vector<std::filesystem::path> &roots) {
    job.seeds = reviewPublicationDependencies(job.publication, roots);
    job.status.dependencyCount = job.seeds.size();
    job.status.dependencyBytes = 0;
    for (const auto &seed : job.seeds) job.status.dependencyBytes += seed.bytes;
    const std::vector<TorrentContent> files{
        {.path = "publication.xanadoc",
         .data = encodePublication(job.publication)}};
    const auto torrent = makeTorrent(files, "publication");
    const auto output  = options.directory / "seeds";
    const auto root    = output / torrent.hash.hex();
    if (!std::filesystem::exists(root / "metainfo.torrent"))
      (void)writeTorrentSeed(output, torrent, files);
    job.seeds.push_back(checkedSeed(torrent.hash, root));
    job.status.manifestHash = torrent.hash;
  }
  void work(std::stop_token stop) {
    std::unique_ptr<PublicationTransport> transport;
    std::string transportError;
    try {
      if (options.makeTransport) transport = options.makeTransport();
      if (transport) port.store(transport->listenPort());
    } catch (const std::exception &error) {
      transportError = error.what();
    }
    while (!stop.stop_requested()) {
      {
        const std::scoped_lock lock(guard);
        while (!submitted.empty()) {
          auto job = std::move(submitted.front());
          submitted.pop_front();
          jobs.insert_or_assign(job.status.id, std::move(job));
        }
        while (!retries.empty()) {
          const auto found = jobs.find(retries.front());
          retries.pop_front();
          if (found != jobs.end()) {
            found->second.seeds.clear();
            found->second.status.phase = PublicationPhase::Queued;
            found->second.nextAttempt  = {};
          }
        }
        refresh();
      }
      std::vector<std::filesystem::path> roots;
      std::map<DhtTarget, std::int64_t> latest;
      for (const auto &[id, job] : jobs) {
        (void)id;
        for (const auto &root : job.roots)
          if (std::ranges::find(roots, root) == roots.end())
            roots.push_back(root);
        if (job.announce)
          latest[job.publication.name()] = std::max(
              latest[job.publication.name()], job.publication.sequence);
      }
      for (auto &[id, job] : jobs) {
        (void)id;
        if (stop.stop_requested()) break;
        if (job.status.phase == PublicationPhase::Published ||
            job.status.phase == PublicationPhase::LocalReady ||
            job.status.phase == PublicationPhase::Failed ||
            job.status.phase == PublicationPhase::Superseded ||
            job.status.phase == PublicationPhase::NeedsVerification)
          continue;
        const auto now = std::chrono::steady_clock::now();
        try {
          if (job.seeds.empty()) prepare(job, roots);
          if (!job.announce) {
            update(job, PublicationPhase::LocalReady);
            continue;
          }
          job.status.identity = options.verifyIdentity
                                    ? options.verifyIdentity(job.publication)
                                    : PublicationIdentity::Unknown;
          if (job.status.identity == PublicationIdentity::Unknown) {
            update(job, PublicationPhase::NeedsVerification);
            continue;
          }
          if (!transport && options.makeTransport) {
            transport = options.makeTransport();
            if (transport) port.store(transport->listenPort());
          }
          if (!transport)
            throw std::runtime_error(transportError.empty()
                                         ? "no publication transport configured"
                                         : transportError);
          if (job.status.phase == PublicationPhase::Queued) {
            update(job, PublicationPhase::Seeding);
            for (const auto &seed : job.seeds) transport->seed(seed);
          }
          if (job.publication.sequence < latest[job.publication.name()]) {
            update(job, PublicationPhase::Superseded);
            continue;
          }
          if (job.status.phase == PublicationPhase::AwaitingDht &&
              transport->acknowledged(job.publication,
                                      job.status.manifestHash)) {
            update(job, PublicationPhase::Published);
            continue;
          }
          if (now < job.nextAttempt) continue;
          ++job.status.attempts;
          transport->announce(job.publication, job.status.manifestHash);
          job.nextAttempt = now + options.retryInterval;
          job.status.error.clear();
          update(job, PublicationPhase::AwaitingDht);
        } catch (const std::exception &error) {
          job.status.error = error.what();
          update(job, PublicationPhase::Failed);
        }
      }
      try {
        if (transport) transport->poll();
      } catch (const std::exception &error) {
        GLEDITOR_LOG_DEBUG("xudu.publication", "Transport poll failed: {}",
                           error.what());
      }
      std::unique_lock lock(guard);
      changed.wait_for(lock, std::chrono::milliseconds{50}, [&] {
        return stop.stop_requested() || !submitted.empty() || !retries.empty();
      });
    }
  }
};

PublicationOutbox::PublicationOutbox(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PublicationOutbox::~PublicationOutbox() = default;
std::string
PublicationOutbox::submit(const Publication &pub,
                          const std::vector<std::filesystem::path> &roots,
                          bool announce) {
  if (pub.sequence < 0 || pub.salt.empty() || pub.salt.size() > 64 ||
      !verifyPublication(pub))
    throw std::invalid_argument("invalid publication outbox request");
  Job job;
  job.publication     = pub;
  job.announce        = announce;
  job.status.id       = jobId(pub);
  job.status.title    = pub.title;
  job.status.sequence = pub.sequence;
  for (const auto &root : roots)
    job.roots.push_back(std::filesystem::absolute(root));
  const auto id = job.status.id;
  {
    const std::scoped_lock lock(impl_->guard);
    if (!impl_->workerError.empty())
      throw std::runtime_error(
          "publication outbox stopped; restart after repairing: " +
          impl_->workerError);
  }
  if (!impl_->insert(job)) return id;
  {
    const std::scoped_lock lock(impl_->guard);
    impl_->submitted.push_back(std::move(job));
  }
  impl_->changed.notify_all();
  return id;
}
void PublicationOutbox::retry(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  if (!impl_->workerError.empty())
    throw std::runtime_error(
        "publication outbox stopped; restart after repairing: " +
        impl_->workerError);
  impl_->retries.emplace_back(id);
  impl_->changed.notify_all();
}
std::vector<PublicationJobStatus> PublicationOutbox::statuses() const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->visible;
}
bool PublicationOutbox::waitFor(std::string_view id, PublicationPhase phase,
                                std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return std::ranges::any_of(impl_->visible, [&](const auto &status) {
      return status.id == id && status.phase == phase;
    });
  });
}
std::uint16_t PublicationOutbox::listenPort() const {
  return impl_->port.load();
}

std::function<std::unique_ptr<PublicationTransport>()>
publicationSwarmTransport(
    MutableKeys keys, SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes) {
  options.enableLocalDiscovery = false;
  options.enableTrackers       = false;
  return [keys, options, nodes = std::move(nodes)] {
    return std::make_unique<SwarmTransport>(keys, options, nodes);
  };
}
} // namespace xanadu
