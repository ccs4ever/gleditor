#include "retained_content.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

#include "bencode.hpp"
#include "resolver.hpp"
#include <gleditor/logging.hpp>

namespace xanadu {
namespace {
std::string readFile(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read retained metainfo: " + path.string());
  std::string bytes{std::istreambuf_iterator<char>(input), {}};
  if (input.bad())
    throw std::runtime_error("cannot read retained metainfo: " + path.string());
  return bytes;
}
void refuseSymlinkAncestors(const std::filesystem::path &destination) {
  std::filesystem::path prefix;
  for (const auto &component : std::filesystem::absolute(destination)) {
    prefix /= component;
    if (std::filesystem::is_symlink(prefix))
      throw std::runtime_error("retained content path contains a symlink");
  }
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

std::filesystem::path carrierDataRoot(const PublicationSeed &seed,
                                      const Metainfo &meta) {
  const auto nested = seed.savePath / meta.name();
  if (!meta.files().empty() &&
      std::filesystem::is_regular_file(nested / meta.files().front().path))
    return nested;
  return seed.savePath;
}
std::filesystem::path retainedDataRoot(const PublicationSeed &seed,
                                       const Metainfo &meta) {
  const auto encoded = bencode::decode(seed.metainfo);
  return encoded.find("info")->find("length") ? seed.savePath
                                              : seed.savePath / meta.name();
}
} // namespace

PublicationSeed reviewPublicationSeed(const PublicationSeed &seed) {
  const auto &hash    = seed.hash;
  const auto &root    = seed.savePath;
  const auto &encoded = seed.metainfo;
  if (std::filesystem::is_symlink(root) ||
      std::filesystem::is_symlink(root / "metainfo.torrent"))
    throw std::runtime_error("retained seed source is a symlink");
  const auto meta = Metainfo::parse(encoded);
  if (meta.hash() != hash || !relativePath(meta.name()) ||
      std::filesystem::path(meta.name()).has_parent_path())
    throw std::runtime_error("publication seed metainfo mismatch: " +
                             hash.hex());
  const auto data          = carrierDataRoot(seed, meta);
  const auto canonicalRoot = std::filesystem::weakly_canonical(data);
  if (canonicalRoot != std::filesystem::weakly_canonical(root) &&
      !relativePath(canonicalRoot.lexically_relative(
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
  source.add(encoded, data.string());
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

PublicationSeed reviewPublicationSeed(const InfoHash &hash,
                                      const std::filesystem::path &directory) {
  return reviewPublicationSeed(
      {.hash     = hash,
       .metainfo = readFile(directory / "metainfo.torrent"),
       .savePath = directory});
}

PublicationSeed
retainPublicationSeed(const PublicationSeed &seed,
                      const std::filesystem::path &destination) {
  namespace fs = std::filesystem;
  refuseSymlinkAncestors(destination);
  fs::create_directories(destination);
  const auto target = destination / seed.hash.hex();
  if (fs::is_symlink(target))
    throw std::runtime_error("retained seed directory is a symlink");
  if (fs::exists(target)) return reviewPublicationSeed(seed.hash, target);
  const auto source = reviewPublicationSeed(seed);
  if (retainedDataRoot(source, Metainfo::parse(source.metainfo)) ==
          source.savePath &&
      Metainfo::parse(source.metainfo).name() == "metainfo.torrent")
    throw std::runtime_error(
        "single-file payload conflicts with retained metainfo sidecar");
  const auto stage = destination / (seed.hash.hex() + ".partial");
  if (!fs::create_directory(stage))
    throw std::runtime_error("retained seed staging directory already exists");
  try {
    std::ofstream metadata(stage / "metainfo.torrent", std::ios::binary);
    metadata << source.metainfo;
    metadata.close();
    if (!metadata) throw std::runtime_error("cannot retain seed metainfo");
    const auto meta       = Metainfo::parse(source.metainfo);
    const auto sourceData = carrierDataRoot(source, meta);
    const auto targetData = retainedDataRoot(
        PublicationSeed{
            .hash = seed.hash, .metainfo = source.metainfo, .savePath = stage},
        meta);
    for (const auto &file : meta.files()) {
      const auto into = targetData / file.path;
      fs::create_directories(into.parent_path());
      fs::copy_file(sourceData / file.path, into);
    }
    auto retained = reviewPublicationSeed(seed.hash, stage);
    if (fs::exists(target) || fs::is_symlink(target))
      throw std::runtime_error("retained seed target appeared during copying");
    fs::rename(stage, target);
    retained.savePath = target;
    return retained;
  } catch (...) {
    fs::remove_all(stage);
    throw;
  }
}

ContentRetentionStats
retainScrollSeeds(const std::span<const Scroll> scrolls,
                  const std::vector<std::filesystem::path> &roots,
                  const std::filesystem::path &destination,
                  const std::vector<PublicationSeed> &mounted) {
  namespace fs = std::filesystem;
  refuseSymlinkAncestors(destination);
  std::map<InfoHash, PublicationSeed> seeds;
  std::set<InfoHash> existing;
  for (const auto &scroll : scrolls) {
    for (const auto &segment : scroll.segments) {
      const auto &hash = segment.torrent;
      if (hash.isZero())
        throw std::runtime_error("quotation has no immutable torrent identity");
      if (!seeds.contains(hash)) {
        const auto target = destination / hash.hex();
        if (fs::is_symlink(destination) || fs::is_symlink(target))
          throw std::runtime_error("retained content directory is a symlink");
        if (fs::exists(target)) {
          seeds.emplace(hash, reviewPublicationSeed(hash, target));
          existing.insert(hash);
        } else {
          for (const auto &root : roots) {
            const auto candidate = root / hash.hex();
            if (!fs::exists(candidate / "metainfo.torrent")) continue;
            seeds.emplace(hash, reviewPublicationSeed(hash, candidate));
            break;
          }
          if (!seeds.contains(hash)) {
            const auto found =
                std::ranges::find(mounted, hash, &PublicationSeed::hash);
            if (found != mounted.end())
              seeds.emplace(hash, reviewPublicationSeed(*found));
          }
        }
        if (!seeds.contains(hash))
          throw std::runtime_error("quotation carrier unavailable locally: " +
                                   hash.hex());
      }
      const auto meta = Metainfo::parse(seeds.at(hash).metainfo);
      if (segment.fileIndex >= meta.files().size())
        throw std::runtime_error("quotation carrier file index is invalid");
      const auto &file = meta.files()[segment.fileIndex];
      if (segment.path != file.path || segment.streamOffset < file.offset ||
          segment.streamOffset - file.offset > file.length ||
          segment.length > file.length - (segment.streamOffset - file.offset) ||
          segment.length > UINT64_MAX - segment.at)
        throw std::runtime_error("quotation segment does not fit its carrier");
    }
  }
  ContentRetentionStats stats;
  for (const auto &[hash, seed] : seeds) {
    if (existing.contains(hash)) {
      ++stats.reusedSeeds;
      continue;
    }
    (void)retainPublicationSeed(seed, destination);
    ++stats.copiedSeeds;
    stats.copiedBytes += seed.bytes;
  }
  GLEDITOR_LOG_DEBUG("xudu.publication",
                     "Retained {} seeds ({} bytes), reused {}",
                     stats.copiedSeeds, stats.copiedBytes, stats.reusedSeeds);
  return stats;
}

struct ContentRetention::Impl {
  struct Request {
    std::uint64_t id;
    std::vector<Scroll> scrolls;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path destination;
    std::vector<PublicationSeed> mounted;
  };
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Request> pending;
  std::map<std::filesystem::path, std::uint64_t> latest;
  std::map<std::filesystem::path, ContentRetentionResult> results;
  std::vector<ContentRetentionResult> notices;
  std::uint64_t next{};
  bool busy{};
  bool stop{};
  std::thread worker;

  Impl() : worker([this] { run(); }) {}
  ~Impl() {
    {
      const std::scoped_lock lock(mutex);
      stop = true;
      changed.notify_all();
    }
    worker.join();
  }
  void run() {
    for (;;) {
      std::unique_lock lock(mutex);
      changed.wait(lock, [this] { return stop || !pending.empty(); });
      if (pending.empty()) return;
      auto request = std::move(pending.front());
      pending.pop_front();
      if (latest.at(request.destination) != request.id) continue;
      busy = true;
      lock.unlock();
      ContentRetentionResult result{.job         = request.id,
                                    .destination = request.destination};
      try {
        result.outcome =
            retainScrollSeeds(request.scrolls, request.roots,
                              request.destination, request.mounted);
      } catch (const std::exception &error) {
        result.outcome = std::unexpected(std::string(error.what()));
        GLEDITOR_LOG_DEBUG("xudu.publication",
                           "Offline quotation retention failed: {}",
                           error.what());
      }
      lock.lock();
      if (latest.at(request.destination) == request.id) {
        results[request.destination] = result;
        notices.push_back(std::move(result));
      }
      busy = false;
      changed.notify_all();
    }
  }
};
ContentRetention::ContentRetention() : impl_(std::make_unique<Impl>()) {}
ContentRetention::~ContentRetention() = default;
std::uint64_t ContentRetention::queue(std::vector<Scroll> scrolls,
                                      std::vector<std::filesystem::path> roots,
                                      std::filesystem::path destination,
                                      std::vector<PublicationSeed> mounted) {
  const std::scoped_lock lock(impl_->mutex);
  const auto id              = ++impl_->next;
  impl_->latest[destination] = id;
  std::erase_if(impl_->pending, [&destination](const auto &request) {
    return request.destination == destination;
  });
  impl_->pending.push_back({id, std::move(scrolls), std::move(roots),
                            std::move(destination), std::move(mounted)});
  impl_->changed.notify_all();
  return id;
}
std::vector<ContentRetentionResult> ContentRetention::takeNotifications() {
  const std::scoped_lock lock(impl_->mutex);
  auto notices = std::exchange(impl_->notices, {});
  std::erase_if(notices, [this](const auto &result) {
    return impl_->latest.at(result.destination) != result.job;
  });
  return notices;
}
std::vector<ContentRetentionResult> ContentRetention::wait() {
  std::unique_lock lock(impl_->mutex);
  impl_->changed.wait(
      lock, [this] { return impl_->pending.empty() && !impl_->busy; });
  std::vector<ContentRetentionResult> results;
  for (const auto &[path, result] : impl_->results) {
    (void)path;
    results.push_back(result);
  }
  return results;
}
} // namespace xanadu
