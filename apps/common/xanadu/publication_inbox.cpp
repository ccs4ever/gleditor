#include "publication_inbox.hpp"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "store.hpp"
#include "user_permascroll.hpp"

namespace xanadu {
namespace {
using namespace std::chrono_literals;
void checkCancelled(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error("Download cancelled");
}

class DownloadTransport final : public PublicationDownloadTransport {
public:
  DownloadTransport(SwarmContentSource::Options options,
                    std::vector<std::pair<std::string, std::uint16_t>> nodes,
                    std::vector<std::pair<std::string, std::uint16_t>> peers,
                    std::chrono::milliseconds timeout)
      : source_(options), nodes_(std::move(nodes)), peers_(std::move(peers)),
        timeout_(timeout) {}

  MutablePointer resolve(const MutableLink &link,
                         std::stop_token stop) override {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    do {
      checkCancelled(stop);
      for (const auto &[host, port] : nodes_) source_.addDhtNode(host, port);
      if (auto pointer = source_.resolveMutable(link, 250ms)) return *pointer;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error(
        "No signed DHT pointer received; retry when the author is reachable");
  }

  void fetch(const InfoHash &hash, const std::filesystem::path &directory,
             std::uint64_t maximumBytes, std::stop_token stop) override {
    checkCancelled(stop);
    std::filesystem::create_directories(directory);
    (void)source_.addMagnet("magnet:?xt=urn:btih:" + hash.hex(),
                            directory.string(), true);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    do {
      checkCancelled(stop);
      for (const auto &[host, port] : peers_)
        source_.connectPeer(hash, host, port);
      if (source_.waitForMetadata(hash, 250ms)) break;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error("Publication torrent metadata unavailable: " +
                                 hash.hex());
    } while (true);
    const auto meta    = source_.metainfo(hash);
    const auto encoded = source_.torrentMetadata(hash);
    if (!meta || !encoded || meta->totalLength() > maximumBytes)
      throw std::runtime_error(
          "Publication download exceeds configured byte limit");
    // Publication carriers use small Merkle pieces. Refuse a hostile piece
    // layout before any read can allocate the entire admitted byte budget.
    constexpr std::uint64_t maximumPieceBytes = 16 * 1024 * 1024;
    if (meta->pieceLength() > maximumPieceBytes)
      throw std::runtime_error(
          "Publication piece exceeds 16 MiB transfer limit");
    const auto safe = [](const std::filesystem::path &path) {
      if (path.empty() || path.has_root_path() ||
          path.string().find_first_of("\\") != std::string::npos ||
          path.string().find('\0') != std::string::npos)
        return false;
      return std::ranges::none_of(
          path, [](const auto &part) { return part == "." || part == ".."; });
    };
    if (!safe(meta->name()) ||
        std::filesystem::path(meta->name()).has_parent_path() ||
        meta->pieceCount() !=
            meta->totalLength() / meta->pieceLength() +
                (meta->totalLength() % meta->pieceLength() != 0))
      throw std::runtime_error(
          "Unsafe or incomplete publication torrent metadata");
    std::set<std::string> paths;
    std::uint64_t totalBytes{};
    for (const auto &file : meta->files()) {
      if (!safe(file.path) || !paths.insert(file.path).second)
        throw std::runtime_error("Unsafe publication torrent file path");
      if (file.length > maximumBytes - totalBytes)
        throw std::runtime_error(
            "Publication file sizes exceed download byte limit");
      totalBytes += file.length;
    }
    source_.startDownload(hash);
    for (std::size_t piece = 0; piece < meta->pieceCount(); ++piece) {
      const auto pieceDeadline = std::chrono::steady_clock::now() + timeout_;
      do {
        checkCancelled(stop);
        const auto bytes = source_.readStream(hash, piece * meta->pieceLength(),
                                              meta->lengthOfPiece(piece));
        if (meta->verifyPiece(piece, bytes)) {
          source_.discardCachedPieces(hash);
          break;
        }
        if (std::chrono::steady_clock::now() >= pieceDeadline)
          throw std::runtime_error("Publication piece unavailable: " +
                                   hash.hex());
      } while (true);
    }
    const auto flushDeadline = std::chrono::steady_clock::now() + timeout_;
    while (!source_.flushDownload(hash, 250ms)) {
      checkCancelled(stop);
      if (std::chrono::steady_clock::now() >= flushDeadline)
        throw std::runtime_error("Publication files did not finish flushing");
    }
    checkCancelled(stop);
    std::ofstream retained(directory / "metainfo.torrent", std::ios::binary);
    retained << *encoded;
    retained.close();
    if (!retained)
      throw std::runtime_error("Cannot retain publication metainfo");
  }

private:
  SwarmContentSource source_;
  std::vector<std::pair<std::string, std::uint16_t>> nodes_, peers_;
  std::chrono::milliseconds timeout_;
};
} // namespace

std::string_view publicationDownloadPhaseName(PublicationDownloadPhase phase) {
  switch (phase) {
  case PublicationDownloadPhase::Queued:
    return "Queued";
  case PublicationDownloadPhase::Resolving:
    return "Resolving signed DHT name";
  case PublicationDownloadPhase::Downloading:
    return "Downloading dependencies";
  case PublicationDownloadPhase::Verifying:
    return "Verifying complete store";
  case PublicationDownloadPhase::Ready:
    return "Ready to open";
  case PublicationDownloadPhase::Failed:
    return "Download failed";
  case PublicationDownloadPhase::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}

struct PublicationInbox::Impl {
  struct Job {
    MutableLink link;
    std::optional<MutablePointer> minimum;
    PublicationDownloadStatus status;
    std::stop_source cancel;
  };
  Options options;
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::map<std::string, Job> jobs;
  std::jthread worker;

  explicit Impl(Options supplied) : options(std::move(supplied)) {
    if (options.directory.empty() || !options.maximumManifestBytes ||
        !options.maximumDependencyBytes || !options.maximumDependencies)
      throw std::invalid_argument("Invalid publication inbox options");
    std::filesystem::create_directories(options.directory);
    // Only completed, signed snapshots are restored. Interrupted requests are
    // not subscriptions and are never silently resolved again at startup.
    for (const auto &entry :
         std::filesystem::directory_iterator(options.directory)) {
      const auto manifestPath = entry.path() / "publication.xanadoc";
      if (!entry.is_directory() || !std::filesystem::exists(manifestPath))
        continue;
      const auto id = entry.path().filename().string();
      if (!PublicKey::parseHex(id) || std::filesystem::is_symlink(entry.path()))
        throw std::runtime_error(
            "Publication inbox: invalid retained snapshot directory");
      if (std::filesystem::file_size(manifestPath) >
          options.maximumManifestBytes)
        throw std::runtime_error(
            "Publication inbox: retained manifest exceeds byte limit");
      std::ifstream manifest(manifestPath, std::ios::binary);
      const std::string encoded{std::istreambuf_iterator<char>(manifest),
                                std::istreambuf_iterator<char>()};
      const auto pub = decodePublication(encoded);
      if (!pub ||
          !std::filesystem::exists(entry.path() / "store" / "store.tables"))
        throw std::runtime_error(
            "Publication inbox: invalid retained signed snapshot");
      Job job;
      job.link.key         = pub->publisher;
      job.link.salt        = pub->salt;
      job.status.id        = id;
      job.status.uri       = job.link.uri();
      job.status.title     = pub->title;
      job.status.phase     = PublicationDownloadPhase::Ready;
      job.status.storePath = entry.path() / "store";
      job.status.version   = pub->version;
      job.status.sequence  = pub->sequence;
      jobs.emplace(id, std::move(job));
    }
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  ~Impl() {
    {
      const std::scoped_lock lock(guard);
      worker.request_stop();
      for (auto &[id, job] : jobs) job.cancel.request_stop();
    }
    changed.notify_all();
    worker.join();
  }
  template <class Fun> void update(const std::string &id, Fun fun) {
    const std::scoped_lock lock(guard);
    fun(jobs.at(id).status);
    changed.notify_all();
  }
  void download(Job job) {
    const auto id   = job.status.id;
    const auto stop = job.cancel.get_token();
    const auto root = options.directory / id;
    try {
      checkCancelled(stop);
      if (!options.makeTransport)
        throw std::runtime_error("No publication swarm configured. Set up the "
                                 "local test swarm, then retry.");
      // Each attempt owns a fresh session and cache. A failed attempt cannot
      // share active torrent writers with verified installation or a retry.
      std::filesystem::remove_all(root);
      auto transport = options.makeTransport();
      if (!transport)
        throw std::runtime_error("Publication transport unavailable");
      const auto pointer = transport->resolve(job.link, stop);
      if (job.minimum && (pointer.sequence < job.minimum->sequence ||
                          (pointer.sequence == job.minimum->sequence &&
                           pointer.hash != job.minimum->hash)))
        throw std::runtime_error("Publication pointer rolls back or conflicts "
                                 "with the subscription's observed sequence");
      const auto cache = root / "cache";
      transport->fetch(pointer.hash, cache / pointer.hash.hex(),
                       options.maximumManifestBytes, stop);
      const auto seed =
          reviewPublicationSeed(pointer.hash, cache / pointer.hash.hex());
      if (seed.bytes > options.maximumManifestBytes)
        throw std::runtime_error(
            "Publication manifest exceeds configured byte limit");
      DirectoryContentSource source;
      source.add(seed.metainfo, seed.savePath.string());
      const auto encoded = source.readStream(pointer.hash, 0, seed.bytes);
      const auto pub     = decodePublication(encoded);
      if (!pub || pub->publisher != job.link.key ||
          pub->salt != job.link.salt || pub->sequence != pointer.sequence)
        throw std::runtime_error("Publication signature, name or sequence does "
                                 "not match the signed DHT pointer");
      std::set<InfoHash> dependencies;
      for (const auto &[name, scroll] : pub->scrolls) {
        (void)name;
        for (const auto &segment : scroll.segments)
          dependencies.insert(segment.torrent);
      }
      for (const auto &segment : pub->opsSegments)
        dependencies.insert(segment.torrent);
      if (dependencies.size() > options.maximumDependencies)
        throw std::runtime_error("Publication has too many dependencies");
      update(id, [&](auto &status) {
        status.title           = pub->title;
        status.phase           = PublicationDownloadPhase::Downloading;
        status.dependencyCount = dependencies.size();
      });
      auto remaining = options.maximumDependencyBytes;
      for (const auto &hash : dependencies) {
        checkCancelled(stop);
        transport->fetch(hash, cache / hash.hex(), remaining, stop);
        const auto dependency = reviewPublicationSeed(hash, cache / hash.hex());
        if (dependency.bytes > remaining)
          throw std::runtime_error(
              "Publication dependencies exceed configured byte limit");
        remaining -= dependency.bytes;
        update(id, [](auto &status) { ++status.completedDependencies; });
      }
      // Destroy the session before copying retained files: libtorrent flushes
      // disk writes at shutdown, and the installer requires stable bytes.
      transport.reset();
      checkCancelled(stop);
      update(id, [](auto &status) {
        status.phase = PublicationDownloadPhase::Verifying;
      });
      (void)installPublication(
          *pub, {cache}, std::make_shared<UserPermascroll>(), root / "store");
      checkCancelled(stop);
      std::filesystem::remove_all(cache);
      std::ofstream manifest(root / "publication.xanadoc.partial",
                             std::ios::binary);
      manifest << encoded;
      manifest.close();
      if (!manifest)
        throw std::runtime_error("Cannot retain signed publication manifest");
      update(id, [&](auto &status) {
        checkCancelled(stop);
        std::filesystem::rename(root / "publication.xanadoc.partial",
                                root / "publication.xanadoc");
        status.phase        = PublicationDownloadPhase::Ready;
        status.version      = pub->version;
        status.sequence     = pub->sequence;
        status.manifestHash = pointer.hash;
        status.storePath    = root / "store";
      });
    } catch (const std::exception &error) {
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
      update(id, [&](auto &status) {
        status.phase = stop.stop_requested()
                           ? PublicationDownloadPhase::Cancelled
                           : PublicationDownloadPhase::Failed;
        status.error = error.what();
      });
    }
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(guard);
      changed.wait(lock, [&] {
        return stop.stop_requested() ||
               std::ranges::any_of(jobs, [](const auto &item) {
                 return item.second.status.phase ==
                        PublicationDownloadPhase::Queued;
               });
      });
      if (stop.stop_requested()) return;
      const auto next = std::ranges::find_if(jobs, [](const auto &item) {
        return item.second.status.phase == PublicationDownloadPhase::Queued;
      });
      next->second.status.phase = PublicationDownloadPhase::Resolving;
      const auto job            = next->second;
      changed.notify_all();
      lock.unlock();
      download(job);
    }
  }
};

PublicationInbox::PublicationInbox(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PublicationInbox::~PublicationInbox() = default;
std::string PublicationInbox::submit(const MutableLink &link,
                                     std::optional<MutablePointer> minimum) {
  if (link.key.isZero() || link.salt.empty() || link.salt.size() > 64)
    throw std::invalid_argument(
        "A publication needs a nonzero author key and document salt");
  if (minimum && (minimum->sequence < 0 || minimum->hash.isZero()))
    throw std::invalid_argument("Invalid subscription pointer lower bound");
  const std::scoped_lock lock(impl_->guard);
  std::string id;
  do {
    id = createMutableKeys().publicKey.hex();
  } while (impl_->jobs.contains(id) ||
           std::filesystem::exists(impl_->options.directory / id));
  Impl::Job job;
  job.link       = link;
  job.minimum    = minimum;
  job.status.id  = id;
  job.status.uri = link.uri();
  impl_->jobs.emplace(id, std::move(job));
  impl_->changed.notify_all();
  return id;
}
void PublicationInbox::cancel(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase == PublicationDownloadPhase::Ready ||
      job.status.phase == PublicationDownloadPhase::Failed ||
      job.status.phase == PublicationDownloadPhase::Cancelled)
    return;
  job.cancel.request_stop();
  if (job.status.phase == PublicationDownloadPhase::Queued)
    job.status.phase = PublicationDownloadPhase::Cancelled;
  impl_->changed.notify_all();
}
void PublicationInbox::retry(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase != PublicationDownloadPhase::Failed &&
      job.status.phase != PublicationDownloadPhase::Cancelled)
    throw std::logic_error("Only failed or cancelled downloads can be retried");
  job.cancel                       = std::stop_source{};
  job.status.phase                 = PublicationDownloadPhase::Queued;
  job.status.completedDependencies = 0;
  job.status.dependencyCount       = 0;
  job.status.error.clear();
  impl_->changed.notify_all();
}
std::vector<PublicationDownloadStatus> PublicationInbox::statuses() const {
  const std::scoped_lock lock(impl_->guard);
  std::vector<PublicationDownloadStatus> result;
  for (const auto &[id, job] : impl_->jobs) result.push_back(job.status);
  return result;
}
PublicationDownloadStatus PublicationInbox::status(std::string_view id) const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->jobs.at(std::string(id)).status;
}
bool PublicationInbox::waitFor(std::string_view id,
                               PublicationDownloadPhase phase,
                               std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).status.phase == phase;
  });
}
std::function<std::unique_ptr<PublicationDownloadTransport>()>
publicationDownloadSwarmTransport(
    SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes,
    std::vector<std::pair<std::string, std::uint16_t>> peers,
    std::chrono::milliseconds timeout) {
  options.enableLocalDiscovery = false;
  options.enableTrackers       = false;
  options.readTimeout          = 250ms;
  options.metadataTimeout      = 250ms;
  return [options, nodes = std::move(nodes), peers = std::move(peers),
          timeout] {
    return std::make_unique<DownloadTransport>(options, nodes, peers, timeout);
  };
}
} // namespace xanadu
