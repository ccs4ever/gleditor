#include "link_package_exchange.hpp"

#include "bencode.hpp"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <gleditor/logging.hpp>
#include <map>
#include <mutex>
#include <thread>

namespace xanadu {
namespace {
using V     = bencode::Value;
using Phase = LinkPackagePhase;
using namespace std::chrono_literals;
std::string readRecord(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || std::filesystem::is_symlink(path) || size > 1024 * 1024)
    throw LinkPackageExchangeUnreadable(
        "Package exchange format 1: unsafe or unreadable record or byte limit");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw LinkPackageExchangeUnreadable(
        "Package exchange format 1: unreadable record");
  return {std::istreambuf_iterator<char>(file),
          std::istreambuf_iterator<char>()};
}
void writeRecord(const std::filesystem::path &path, const std::string &bytes) {
  const auto partial = path.string() + ".partial";
  std::ofstream file(partial, std::ios::binary | std::ios::trunc);
  file << bytes;
  file.close();
  if (!file) throw std::runtime_error("Cannot retain package exchange record");
  std::filesystem::rename(partial, path);
}
std::string packageId(const LinkPackage &pkg) {
  return "send-" + pkg.name().hex() + "-" + std::to_string(pkg.sequence);
}
InfoHash packageHash(const LinkPackage &pkg) {
  const std::vector<TorrentContent> files{
      {.path = "links.xanalinks", .data = encodeLinkPackage(pkg)}};
  return makeTorrent(files, "link-package").hash;
}
void retainRequest(const std::filesystem::path &root,
                   const std::string &bytes) {
  const auto stage = root.string() + ".partial";
  if (std::filesystem::is_symlink(stage))
    throw LinkPackageExchangeUnreadable(
        "Package exchange format 1: unsafe request stage");
  std::filesystem::create_directories(stage);
  writeRecord(std::filesystem::path(stage) / "request", bytes);
  std::filesystem::rename(stage, root);
}
void stopCheck(std::stop_token stop) {
  if (stop.stop_requested())
    throw std::runtime_error("Package transfer cancelled");
}
} // namespace
std::string_view linkPackagePhaseName(Phase phase) {
  switch (phase) {
  case Phase::Queued:
    return "Queued";
  case Phase::NeedsVerification:
    return "Curator enrollment required";
  case Phase::Seeding:
    return "Seeding signed package";
  case Phase::Downloading:
    return "Fetching signed package endpoints";
  case Phase::Superseded:
    return "Retained earlier package; newer publication queued";
  case Phase::AwaitingDht:
    return "Waiting for package and catalog acknowledgement";
  case Phase::Published:
    return "Published to rendezvous";
  case Phase::Ready:
    return "Signed endpoints ready to review";
  case Phase::Failed:
    return "Package unavailable";
  case Phase::Cancelled:
    return "Cancelled";
  }
  return "Unknown";
}
struct LinkPackageExchange::Impl {
  struct Job {
    LinkPackageStatus status;
    std::optional<SignedAuthorCatalog> catalog;
    AuthorCatalogEntry entry;
    std::vector<std::string> queried;
    std::stop_source cancel;
    std::uint64_t generation{};
    std::chrono::steady_clock::time_point next{};
  };
  Options options;
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::map<std::string, Job> jobs;
  std::unique_ptr<PublicationTransport> publisher;
  std::jthread worker;
  static std::string encoded(const Job &job) {
    bencode::List keys;
    for (const auto &key : job.queried) keys.push_back(V::string(key));
    return V::dict(
               {{"cancelled", V::integer(job.status.phase == Phase::Cancelled)},
                {"format", V::integer(1)},
                {"mode", V::integer(job.status.received   ? 2
                                    : job.status.announce ? 1
                                                          : 0)},
                {"payload",
                 V::string(job.catalog
                               ? encodeAuthorCatalog(*job.catalog)
                               : encodeLinkPackage(job.status.package))},
                {"salt", V::string(job.catalog ? job.entry.salt
                                               : job.status.package.salt)},
                {"scrolls", V::list(std::move(keys))}})
        .encode();
  }
  Job decoded(std::string_view bytes) const {
    try {
      const auto root   = bencode::decode(bytes);
      const auto &dict  = root.asDict();
      const auto format = dict.at("format").asInteger();
      if (format != 1)
        throw LinkPackageExchangeUnreadable(
            "Package exchange version 1 expected, got " +
            std::to_string(format));
      if (dict.size() != 6) throw std::runtime_error("unexpected fields");
      const auto mode = dict.at("mode").asInteger();
      if (mode < 0 || mode > 2)
        throw std::runtime_error("unknown transfer mode");
      Job job;
      const auto cancelled = dict.at("cancelled").asInteger();
      if (cancelled < 0 || cancelled > 1)
        throw std::runtime_error("invalid cancellation state");
      if (cancelled) job.status.phase = Phase::Cancelled;
      job.status.received = mode == 2;
      job.status.announce = mode == 1;
      const auto &salt    = dict.at("salt").asString();
      for (const auto &key : dict.at("scrolls").asList())
        job.queried.push_back(key.asString());
      if (job.queried.size() > 64 ||
          std::ranges::any_of(job.queried, [](const auto &key) {
            return key.empty() || key.size() > 256;
          }))
        throw std::runtime_error("invalid queried scrolls");
      if (job.status.received) {
        job.catalog      = decodeAuthorCatalog(dict.at("payload").asString());
        const auto found = std::ranges::find(job.catalog->entries, salt,
                                             &AuthorCatalogEntry::salt);
        if (found == job.catalog->entries.end() ||
            found->kind != CatalogEntryKind::LinkPackage)
          throw std::runtime_error("catalog has no package entry");
        job.entry = *found;
        if (!job.queried.empty() &&
            !std::ranges::any_of(job.queried, [&](const auto &key) {
              return std::ranges::find(job.entry.scrollKeys, key) !=
                     job.entry.scrollKeys.end();
            }))
          throw std::runtime_error(
              "package does not advertise requested scrolls");
        job.status.hash = found->hash;
        job.status.id   = "receive-" + found->hash.hex();
      } else {
        const auto package = decodeLinkPackage(dict.at("payload").asString());
        if (!package || package->salt != salt)
          throw std::runtime_error("invalid signed package request");
        reviewLinkPackage(*package);
        job.status.package = *package;
        job.status.hash    = packageHash(*package);
        job.status.id      = packageId(*package);
      }
      return job;
    } catch (const LinkPackageExchangeUnreadable &) {
      throw;
    } catch (const std::exception &error) {
      throw LinkPackageExchangeUnreadable("Package exchange format 1: " +
                                          std::string(error.what()));
    }
  }
  void checkHighWater(const LinkPackage &pkg, const InfoHash &hash) const {
    for (const auto &[id, prior] : jobs) {
      (void)id;
      if (prior.status.received && prior.status.phase != Phase::Ready) continue;
      const auto &accepted = prior.status.package;
      if (accepted.curator == pkg.curator && accepted.salt == pkg.salt &&
          (accepted.sequence > pkg.sequence ||
           (accepted.sequence == pkg.sequence && prior.status.hash != hash)))
        throw std::invalid_argument(
            "Link package rolls back or conflicts with an accepted sequence");
    }
  }
  explicit Impl(Options supplied) : options(std::move(supplied)) {
    if (options.directory.empty() || !options.maximumRequests ||
        options.retryInterval <= 0ms)
      throw std::invalid_argument("Invalid package exchange options");
    if (std::filesystem::is_symlink(options.directory))
      throw LinkPackageExchangeUnreadable(
          "Package exchange format 1: unsafe cache root");
    std::filesystem::create_directories(options.directory);
    for (const auto &entry :
         std::filesystem::directory_iterator(options.directory)) {
      if (entry.path().extension() == ".partial") continue;
      if (!entry.is_directory() || std::filesystem::is_symlink(entry.path()))
        throw LinkPackageExchangeUnreadable(
            "Package exchange format 1: invalid job directory");
      if (jobs.size() >= options.maximumRequests)
        throw LinkPackageExchangeUnreadable(
            "Package exchange format 1: request limit exceeded");
      auto job = decoded(readRecord(entry.path() / "request"));
      if (job.status.id != entry.path().filename())
        throw LinkPackageExchangeUnreadable(
            "Package exchange format 1: job identity mismatch");
      if (job.status.received &&
          std::filesystem::exists(entry.path() / "package")) {
        try {
          restorePackage(job);
        } catch (const std::exception &error) {
          throw LinkPackageExchangeUnreadable("Package exchange format 1: " +
                                              std::string(error.what()));
        }
        job.status.phase = Phase::Ready;
      }
      jobs.emplace(job.status.id, std::move(job));
    }
    // Filesystem enumeration order cannot allow a lower sequence to become
    // the accepted cache. Equal-sequence equivocation is always refused.
    for (const auto &[id, job] : jobs) {
      (void)id;
      if (job.status.phase == Phase::Ready)
        for (const auto &[otherId, other] : jobs) {
          (void)otherId;
          if (other.status.phase == Phase::Ready &&
              job.status.package.name() == other.status.package.name() &&
              job.status.package.sequence == other.status.package.sequence &&
              job.status.hash != other.status.hash)
            throw LinkPackageExchangeUnreadable(
                "Package exchange format 1: conflicting retained packages");
        }
    }
    for (auto &[id, job] : jobs) {
      (void)id;
      if (job.status.received) continue;
      for (const auto &[otherId, other] : jobs) {
        (void)otherId;
        if (!other.status.received &&
            job.status.package.name() == other.status.package.name() &&
            job.status.package.sequence < other.status.package.sequence)
          job.status.phase = Phase::Superseded;
      }
    }
    worker = std::jthread([this](std::stop_token stop) { run(stop); });
  }
  ~Impl() {
    {
      const std::scoped_lock lock(guard);
      worker.request_stop();
      for (auto &[id, job] : jobs) {
        (void)id;
        job.cancel.request_stop();
      }
    }
    changed.notify_all();
    worker.join();
  }
  void validateReceived(Job &job, const LinkPackage &pkg) const {
    reviewLinkPackage(pkg);
    if (pkg.curator != job.catalog->publisher || pkg.salt != job.entry.salt ||
        pkg.sequence != job.entry.sequence || pkg.title != job.entry.title ||
        linkPackageScrollKeys(pkg) != job.entry.scrollKeys)
      throw std::runtime_error("Signed package differs from its advertised "
                               "identity, sequence or endpoints");
    job.status.package = pkg;
  }
  void restorePackage(Job &job) const {
    const auto root = options.directory / job.status.id;
    const auto seed = reviewPublicationSeed(job.status.hash, root / "seed");
    if (seed.bytes > maximumLinkPackageBytes)
      throw std::runtime_error("Package byte limit exceeded");
    DirectoryContentSource source;
    source.add(seed.metainfo, seed.savePath.string());
    const auto pkg =
        decodeLinkPackage(source.readStream(job.status.hash, 0, seed.bytes));
    if (!pkg) throw std::runtime_error("Invalid retained package signature");
    validateReceived(job, *pkg);
    if (readRecord(root / "package") != encodeLinkPackage(*pkg))
      throw LinkPackageExchangeUnreadable(
          "Package exchange format 1: retained package differs from verified "
          "bytes");
  }
  void update(const Job &copied, const std::function<void(Job &)> &change) {
    const std::scoped_lock lock(guard);
    auto &current = jobs.at(copied.status.id);
    if (current.generation != copied.generation ||
        current.cancel.stop_requested())
      return;
    change(current);
    changed.notify_all();
  }
  void step(Job job, std::stop_token stop) {
    const std::stop_callback cancellation(stop,
                                          [&] { job.cancel.request_stop(); });
    const auto root = options.directory / job.status.id;
    try {
      stopCheck(job.cancel.get_token());
      if (job.status.received) {
        update(job, [](auto &current) {
          current.status.phase = Phase::Downloading;
        });
        if (!options.makeDownloader)
          throw std::runtime_error("No package download swarm configured");
        std::filesystem::remove_all(root / "seed");
        auto transport = options.makeDownloader();
        if (!transport)
          throw std::runtime_error("Package download transport unavailable");
        transport->fetch(job.status.hash, root / "seed",
                         maximumLinkPackageBytes, job.cancel.get_token());
        transport.reset();
        stopCheck(job.cancel.get_token());
        const auto seed = reviewPublicationSeed(job.status.hash, root / "seed");
        if (seed.bytes > maximumLinkPackageBytes)
          throw std::runtime_error("Package byte limit exceeded");
        DirectoryContentSource source;
        source.add(seed.metainfo, seed.savePath.string());
        const auto pkg = decodeLinkPackage(
            source.readStream(job.status.hash, 0, seed.bytes));
        if (!pkg)
          throw std::runtime_error(
              "Package signature or canonical encoding failed");
        validateReceived(job, *pkg);
        update(job, [&](auto &current) {
          checkHighWater(*pkg, job.status.hash);
          writeRecord(root / "package", encodeLinkPackage(*pkg));
          current.status.package = *pkg;
          current.status.phase   = Phase::Ready;
          current.status.error.clear();
        });
      } else {
        reviewLinkPackage(job.status.package);
        const std::vector<TorrentContent> files{
            {.path = "links.xanalinks",
             .data = encodeLinkPackage(job.status.package)}};
        const auto torrent = makeTorrent(files, "link-package");
        const auto seeds   = root / "seeds";
        if (!std::filesystem::exists(seeds / torrent.hash.hex() /
                                     "metainfo.torrent"))
          (void)writeTorrentSeed(seeds, torrent, files);
        const auto seed =
            reviewPublicationSeed(torrent.hash, seeds / torrent.hash.hex());
        update(job, [&](auto &current) { current.status.hash = torrent.hash; });
        if (!job.status.announce) {
          update(job,
                 [](auto &current) { current.status.phase = Phase::Ready; });
          return;
        }
        const auto identity = options.verifyIdentity
                                  ? options.verifyIdentity(job.status.package)
                                  : PublicationIdentity::Unknown;
        update(job, [&](auto &current) { current.status.identity = identity; });
        if (identity == PublicationIdentity::Unknown) {
          update(job, [](auto &current) {
            current.status.phase = Phase::NeedsVerification;
          });
          return;
        }
        if (!publisher) {
          if (!options.makePublisher)
            throw std::runtime_error("No package publishing swarm configured");
          publisher = options.makePublisher();
          if (!publisher)
            throw std::runtime_error(
                "Package publishing transport unavailable");
        }
        publisher->seed(seed);
        if (job.status.phase == Phase::Queued ||
            job.status.phase == Phase::Seeding)
          publisher->announcePackage(job.status.package, torrent.hash);
        if (publisher->packageAcknowledged(job.status.package, torrent.hash) &&
            publisher->advertisePackage(job.status.package, torrent.hash)) {
          update(job, [](auto &current) {
            current.status.phase = Phase::Published;
            current.status.error.clear();
          });
        } else {
          update(job, [&](auto &current) {
            current.status.phase = Phase::AwaitingDht;
            current.next =
                std::chrono::steady_clock::now() + options.retryInterval;
          });
          // Reannounce without reserving another sequence on the next attempt.
          publisher->announcePackage(job.status.package, torrent.hash);
        }
      }
    } catch (const std::exception &error) {
      update(job, [&](auto &current) {
        current.status.phase = Phase::Failed;
        current.status.error = error.what();
      });
    }
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(guard);
      changed.wait_for(lock, 50ms, [&] {
        return stop.stop_requested() ||
               std::ranges::any_of(jobs, [](const auto &item) {
                 const auto &job = item.second;
                 return (job.status.phase == Phase::Queued ||
                         job.status.phase == Phase::AwaitingDht) &&
                        job.next <= std::chrono::steady_clock::now();
               });
      });
      if (stop.stop_requested()) break;
      const auto found = std::ranges::find_if(jobs, [](const auto &item) {
        return (item.second.status.phase == Phase::Queued ||
                item.second.status.phase == Phase::AwaitingDht) &&
               item.second.next <= std::chrono::steady_clock::now();
      });
      std::optional<Job> job;
      if (found != jobs.end()) {
        job = found->second;
        if (found->second.status.phase == Phase::Queued)
          found->second.status.phase = Phase::Seeding;
      }
      changed.notify_all();
      lock.unlock();
      if (publisher) {
        try {
          publisher->poll();
        } catch (const std::exception &error) {
          GLEDITOR_LOG_DEBUG("xudu.packages",
                             "Package transport polling failed: {}",
                             error.what());
          publisher.reset();
          if (job && !job->status.received) job.reset();
          lock.lock();
          for (auto &[id, current] : jobs) {
            (void)id;
            if (!current.status.received &&
                (current.status.phase == Phase::Published ||
                 current.status.phase == Phase::Seeding ||
                 current.status.phase == Phase::AwaitingDht)) {
              current.status.phase = Phase::Failed;
              current.status.error =
                  "Package transport stopped; retry publication";
            }
          }
          changed.notify_all();
          lock.unlock();
        }
      }
      if (job) step(std::move(*job), stop);
    }
    publisher.reset();
  }
};
LinkPackageExchange::LinkPackageExchange(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
LinkPackageExchange::~LinkPackageExchange() = default;
std::string LinkPackageExchange::submit(const LinkPackage &pkg, bool announce) {
  reviewLinkPackage(pkg);
  Impl::Job job;
  job.status.package  = pkg;
  job.status.hash     = packageHash(pkg);
  job.status.announce = announce;
  job.status.id       = packageId(pkg);
  const auto record   = Impl::encoded(job);
  const std::scoped_lock lock(impl_->guard);
  impl_->checkHighWater(pkg, job.status.hash);
  if (const auto found = impl_->jobs.find(job.status.id);
      found != impl_->jobs.end()) {
    if (!found->second.status.announce && announce &&
        encodeLinkPackage(found->second.status.package) ==
            encodeLinkPackage(pkg)) {
      ++job.generation;
      job.generation += found->second.generation;
      writeRecord(impl_->options.directory / job.status.id / "request", record);
      found->second.cancel.request_stop();
      found->second = job;
      impl_->changed.notify_all();
      return job.status.id;
    }
    if (encodeLinkPackage(found->second.status.package) !=
            encodeLinkPackage(pkg) ||
        found->second.status.announce != announce)
      throw std::invalid_argument(
          "Conflicting package request at the same sequence");
    return job.status.id;
  }
  if (impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::runtime_error("Package request limit reached");
  const auto root = impl_->options.directory / job.status.id;
  retainRequest(root, record);
  for (auto &[id, prior] : impl_->jobs) {
    (void)id;
    if (!prior.status.received && prior.status.package.name() == pkg.name() &&
        prior.status.package.sequence < pkg.sequence) {
      prior.cancel.request_stop();
      ++prior.generation;
      prior.status.phase = Phase::Superseded;
    }
  }
  impl_->jobs.emplace(job.status.id, job);
  impl_->changed.notify_all();
  return job.status.id;
}
std::string LinkPackageExchange::fetch(const SignedAuthorCatalog &catalog,
                                       const AuthorCatalogEntry &entry,
                                       std::vector<std::string> keys) {
  Impl::Job job;
  job.catalog         = decodeAuthorCatalog(encodeAuthorCatalog(catalog));
  job.entry           = entry;
  job.queried         = std::move(keys);
  job.status.received = true;
  job.status.id       = "receive-" + entry.hash.hex();
  job.status.hash     = entry.hash;
  const auto record   = Impl::encoded(job);
  job                 = impl_->decoded(record);
  if (job.entry != entry)
    throw std::invalid_argument("Package entry differs from signed catalog");
  const std::scoped_lock lock(impl_->guard);
  if (impl_->jobs.contains(job.status.id)) return job.status.id;
  if (impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::runtime_error("Package request limit reached");
  const auto root = impl_->options.directory / job.status.id;
  retainRequest(root, record);
  impl_->jobs.emplace(job.status.id, job);
  impl_->changed.notify_all();
  return job.status.id;
}
void LinkPackageExchange::retry(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase != Phase::Failed &&
      job.status.phase != Phase::Cancelled &&
      job.status.phase != Phase::NeedsVerification)
    throw std::logic_error("Only unavailable packages can be retried");
  auto candidate = job;
  ++candidate.generation;
  candidate.cancel       = std::stop_source{};
  candidate.status.phase = Phase::Queued;
  candidate.status.error.clear();
  candidate.next = {};
  writeRecord(impl_->options.directory / job.status.id / "request",
              Impl::encoded(candidate));
  job = std::move(candidate);
  impl_->changed.notify_all();
}
void LinkPackageExchange::cancel(std::string_view id) {
  const std::scoped_lock lock(impl_->guard);
  auto &job = impl_->jobs.at(std::string(id));
  if (job.status.phase == Phase::Ready || job.status.phase == Phase::Published)
    return;
  if (job.status.phase == Phase::Superseded) return;
  auto candidate         = job;
  candidate.status.phase = Phase::Cancelled;
  ++candidate.generation;
  writeRecord(impl_->options.directory / job.status.id / "request",
              Impl::encoded(candidate));
  job.cancel.request_stop();
  job = std::move(candidate);
  impl_->changed.notify_all();
}
LinkPackageStatus LinkPackageExchange::status(std::string_view id) const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->jobs.at(std::string(id)).status;
}
std::vector<LinkPackageStatus> LinkPackageExchange::statuses() const {
  const std::scoped_lock lock(impl_->guard);
  std::vector<LinkPackageStatus> result;
  for (const auto &[id, job] : impl_->jobs) {
    (void)id;
    result.push_back(job.status);
  }
  return result;
}
bool LinkPackageExchange::waitFor(std::string_view id, Phase phase,
                                  std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).status.phase == phase;
  });
}
} // namespace xanadu
