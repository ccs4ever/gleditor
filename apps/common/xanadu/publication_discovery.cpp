#include "publication_discovery.hpp"

#include "publication_inbox.hpp"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <gleditor/logging.hpp>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace xanadu {
namespace {
using namespace std::chrono_literals;
void checkStop(std::stop_token stop) {
  if (stop.stop_requested()) throw std::runtime_error("Discovery cancelled");
}
class DiscoveryTransport final : public PublicationDiscoveryTransport {
public:
  DiscoveryTransport(SwarmContentSource::Options options,
                     std::vector<std::pair<std::string, std::uint16_t>> nodes,
                     std::filesystem::path scratch,
                     std::chrono::milliseconds timeout)
      : options_(std::move(options)), nodes_(std::move(nodes)),
        scratch_(std::move(scratch)), timeout_(timeout) {
    // Attempts never share active writers or remove another process's cache.
    scratch_ /= createMutableKeys().publicKey.hex();
  }
  ~DiscoveryTransport() override {
    std::error_code ignored;
    std::filesystem::remove_all(scratch_, ignored);
  }
  std::string author(const PublicKey &key, std::stop_token stop) override {
    auto transport =
        publicationDownloadSwarmTransport(options_, nodes_, {}, timeout_)();
    MutableLink link;
    link.key               = key;
    link.salt              = "catalog";
    const auto pointer     = transport->resolve(link, stop);
    const auto destination = scratch_ / pointer.hash.hex();
    transport->fetch(pointer.hash, destination, maximumAuthorCatalogBytes,
                     stop);
    transport.reset();
    checkStop(stop);
    const auto seed = reviewPublicationSeed(pointer.hash, destination);
    if (seed.bytes > maximumAuthorCatalogBytes)
      throw AuthorCatalogUnreadable(
          "Author catalog download exceeds byte limit");
    DirectoryContentSource source;
    source.add(seed.metainfo, seed.savePath.string());
    const auto bytes   = source.readStream(pointer.hash, 0, seed.bytes);
    const auto catalog = decodeAuthorCatalog(bytes);
    if (catalog.publisher != key || catalog.sequence != pointer.sequence)
      throw AuthorCatalogUnreadable(
          "Author catalog does not match requested key or signed DHT sequence");
    return bytes;
  }
  std::vector<std::string> topic(std::string_view requested,
                                 std::stop_token stop) override {
    return collect({canonicalPublicationTopic(requested)}, {}, stop);
  }
  std::vector<std::string> backlinks(const std::vector<std::string> &scrollKeys,
                                     std::stop_token stop) override {
    return collect({}, scrollKeys, stop);
  }
  std::vector<std::string> collect(const std::vector<std::string> &topics,
                                   const std::vector<std::string> &scrollKeys,
                                   std::stop_token stop) {
    SwarmContentSource source(options_);
    for (const auto &topic : topics)
      source.joinPublicationTopic(topic, scratch_.string());
    for (const auto &key : scrollKeys)
      source.joinLinkPackageScroll(key, scratch_.string());
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    std::chrono::steady_clock::time_point collectedUntil{};
    std::set<std::string> seen;
    std::vector<std::string> result;
    do {
      checkStop(stop);
      for (const auto &[host, port] : nodes_) source.addDhtNode(host, port);
      source.poll();
      for (auto &[hash, bytes] : source.takePublicationCatalogs()) {
        const bool wanted =
            std::ranges::any_of(topics,
                                [&](const auto &topic) {
                                  return hash == publicationTopicTarget(topic);
                                }) ||
            std::ranges::any_of(scrollKeys, [&](const auto &key) {
              return hash.bytes == linkPackageRendezvousTarget(key).bytes;
            });
        if (!wanted || result.size() >= 64) continue;
        try {
          const auto catalog = decodeAuthorCatalog(bytes);
          if (!std::ranges::any_of(catalog.entries, [&](const auto &entry) {
                return std::ranges::any_of(topics,
                                           [&](const auto &topic) {
                                             return std::ranges::find(
                                                        entry.topics, topic) !=
                                                    entry.topics.end();
                                           }) ||
                       (entry.kind == CatalogEntryKind::LinkPackage &&
                        std::ranges::any_of(scrollKeys, [&](const auto &key) {
                          return std::ranges::find(entry.scrollKeys, key) !=
                                 entry.scrollKeys.end();
                        }));
              }))
            continue;
          const auto identity = catalog.publisher.hex() + ":" +
                                std::to_string(catalog.sequence) + ":" + bytes;
          if (seen.insert(identity).second) result.push_back(std::move(bytes));
          if (collectedUntil == std::chrono::steady_clock::time_point{})
            collectedUntil = std::chrono::steady_clock::now() +
                             (scrollKeys.empty() ? 2s : 10s);
        } catch (const AuthorCatalogUnreadable &) {
          GLEDITOR_LOG_DEBUG("xudu.discovery", "Refused invalid topic catalog");
        }
      }
      if (!result.empty() && std::chrono::steady_clock::now() >= collectedUntil)
        return result;
      std::this_thread::sleep_for(50ms);
    } while (std::chrono::steady_clock::now() < deadline);
    if (result.empty())
      throw std::runtime_error("No signed rendezvous metadata received; retry "
                               "when peers are reachable");
    return result;
  }

private:
  SwarmContentSource::Options options_;
  std::vector<std::pair<std::string, std::uint16_t>> nodes_;
  std::filesystem::path scratch_;
  std::chrono::milliseconds timeout_;
};
} // namespace

struct PublicationDiscovery::Impl {
  Options options;
  mutable std::mutex guard;
  mutable std::condition_variable changed;
  std::map<std::string, PublicationDiscoveryStatus> jobs;
  std::vector<SignedAuthorCatalog> cached;
  std::vector<PublicKey> followed;
  std::jthread worker;
  explicit Impl(Options supplied) : options(std::move(supplied)) {
    if (options.directory.empty() || !options.maximumRequests)
      throw std::invalid_argument("Invalid publication discovery options");
    cached             = retainedAuthorCatalogs(options.directory);
    const auto follows = options.directory / "followed";
    std::filesystem::create_directories(follows);
    for (const auto &entry : std::filesystem::directory_iterator(follows)) {
      if (entry.path().extension() == ".partial") continue;
      const auto key = PublicKey::parseHex(entry.path().filename().string());
      if (!entry.is_regular_file() || !key || key->isZero())
        throw AuthorCatalogUnreadable(
            "Discovery cache: invalid followed author key");
      std::ifstream input(entry.path());
      std::string record;
      std::getline(input, record);
      if (record != "xudu-follow:1:" + key->hex())
        throw AuthorCatalogUnreadable(
            "Discovery cache: followed author format 1 expected");
      followed.push_back(*key);
    }
    worker = std::jthread([this](std::stop_token stop) { work(stop); });
  }
  ~Impl() {
    // Synchronize with the wait predicate so stop cannot race ahead of wait.
    {
      const std::scoped_lock lock(guard);
      worker.request_stop();
    }
    changed.notify_all();
    worker.join();
  }
  void work(std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::unique_lock lock(guard);
      changed.wait(lock, [&] {
        return stop.stop_requested() ||
               std::ranges::any_of(jobs, [](const auto &job) {
                 return job.second.phase == DiscoveryPhase::Queued;
               });
      });
      if (stop.stop_requested()) return;
      const auto found    = std::ranges::find_if(jobs, [](const auto &job) {
        return job.second.phase == DiscoveryPhase::Queued;
      });
      const auto id       = found->first;
      auto status         = found->second;
      found->second.phase = DiscoveryPhase::Searching;
      changed.notify_all();
      lock.unlock();
      try {
        if (!options.makeTransport)
          throw std::runtime_error("No discovery swarm configured. Set up the "
                                   "local test swarm, then retry.");
        auto transport = options.makeTransport();
        if (!transport)
          throw std::runtime_error("Discovery transport unavailable");
        std::vector<std::string> encoded;
        if (!status.scrollKeys.empty())
          encoded = transport->backlinks(status.scrollKeys, stop);
        else if (status.author)
          encoded.push_back(
              transport->author(*PublicKey::parseHex(status.query), stop));
        else
          encoded = transport->topic(status.query, stop);
        checkStop(stop);
        if (encoded.size() > 64)
          throw std::runtime_error("Discovery response count exceeds limit");
        status.catalogs.clear();
        std::map<PublicKey, SignedAuthorCatalog> accepted;
        for (const auto &bytes : encoded) {
          auto catalog = decodeAuthorCatalog(bytes);
          if (status.author && catalog.publisher.hex() != status.query)
            throw AuthorCatalogUnreadable(
                "Author catalog publisher differs from the pinned key");
          if (!status.author &&
              !std::ranges::any_of(catalog.entries, [&](const auto &entry) {
                return status.scrollKeys.empty()
                           ? std::ranges::find(entry.topics, status.query) !=
                                 entry.topics.end()
                           : entry.kind == CatalogEntryKind::LinkPackage &&
                                 std::ranges::any_of(
                                     status.scrollKeys, [&](const auto &key) {
                                       return std::ranges::find(
                                                  entry.scrollKeys, key) !=
                                              entry.scrollKeys.end();
                                     });
              }))
            continue;
          if (retainAuthorCatalog(options.directory, catalog)) {
            auto found = accepted.find(catalog.publisher);
            if (found == accepted.end() ||
                found->second.sequence < catalog.sequence)
              accepted.insert_or_assign(catalog.publisher, std::move(catalog));
          }
        }
        for (auto &[key, catalog] : accepted)
          status.catalogs.push_back(std::move(catalog));
        if (status.catalogs.empty())
          throw std::runtime_error(
              "No new signed catalog accepted; older snapshots were refused");
        auto retained = retainedAuthorCatalogs(options.directory);
        checkStop(stop);
        lock.lock();
        cached       = std::move(retained);
        status.phase = DiscoveryPhase::Ready;
        status.error.clear();
      } catch (const std::exception &error) {
        if (!lock.owns_lock()) lock.lock();
        status.phase = DiscoveryPhase::Failed;
        status.error = error.what();
      }
      jobs[id] = std::move(status);
      changed.notify_all();
    }
  }
};
PublicationDiscovery::PublicationDiscovery(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
PublicationDiscovery::~PublicationDiscovery() = default;
std::string PublicationDiscovery::submit(std::string_view query, bool author) {
  std::string canonical;
  if (author) {
    const auto key = PublicKey::parseHex(query);
    if (!key || key->isZero())
      throw std::invalid_argument("Enter a 64-hex publishing key");
    canonical = key->hex();
  } else
    canonical = canonicalPublicationTopic(query);
  const auto id = (author ? "author:" : "topic:") + canonical;
  const std::scoped_lock lock(impl_->guard);
  auto found = impl_->jobs.find(id);
  if (found != impl_->jobs.end() &&
      (found->second.phase == DiscoveryPhase::Queued ||
       found->second.phase == DiscoveryPhase::Searching))
    return id;
  if (found == impl_->jobs.end() &&
      impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::runtime_error(
        "Discovery request limit reached; restart before adding more topics");
  auto &status = impl_->jobs[id];
  if (author) {
    const auto key = *PublicKey::parseHex(canonical);
    if (std::ranges::find(impl_->followed, key) == impl_->followed.end()) {
      const auto markers = impl_->options.directory / "followed";
      const auto partial =
          markers /
          (canonical + "." + createMutableKeys().publicKey.hex() + ".partial");
      std::ofstream marker(partial);
      marker << "xudu-follow:1:" << canonical << '\n';
      marker.close();
      if (!marker)
        throw std::runtime_error("Cannot retain followed author key");
      std::filesystem::rename(partial, markers / canonical);
      impl_->followed.push_back(key);
    }
  }
  status.query  = canonical;
  status.author = author;
  status.phase  = DiscoveryPhase::Queued;
  status.error.clear();
  impl_->changed.notify_all();
  return id;
}
std::string PublicationDiscovery::submitLinks(std::vector<std::string> keys) {
  std::ranges::sort(keys);
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  if (keys.empty() || keys.size() > 64 ||
      std::ranges::any_of(keys, [](const auto &key) {
        return key.empty() || key.size() > 256 ||
               key.find_first_of("\r\n") != std::string::npos;
      }))
    throw std::invalid_argument(
        "Links and responses needs 1 to 64 global scroll keys");
  std::string joined;
  for (const auto &key : keys) joined += key + '\n';
  const auto id = "links:" + linkPackageRendezvousTarget(joined).hex();
  const std::scoped_lock lock(impl_->guard);
  auto found = impl_->jobs.find(id);
  if (found != impl_->jobs.end() &&
      (found->second.phase == DiscoveryPhase::Queued ||
       found->second.phase == DiscoveryPhase::Searching))
    return id;
  if (found == impl_->jobs.end() &&
      impl_->jobs.size() >= impl_->options.maximumRequests)
    throw std::runtime_error("Discovery request limit reached");
  auto &status      = impl_->jobs[id];
  status.query      = "Links and responses";
  status.author     = false;
  status.scrollKeys = std::move(keys);
  status.phase      = DiscoveryPhase::Queued;
  status.error.clear();
  impl_->changed.notify_all();
  return id;
}
PublicationDiscoveryStatus
PublicationDiscovery::status(std::string_view id) const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->jobs.at(std::string(id));
}
std::vector<SignedAuthorCatalog> PublicationDiscovery::cachedCatalogs() const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->cached;
}
std::vector<PublicKey> PublicationDiscovery::followedAuthors() const {
  const std::scoped_lock lock(impl_->guard);
  return impl_->followed;
}
bool PublicationDiscovery::waitFor(std::string_view id, DiscoveryPhase phase,
                                   std::chrono::milliseconds timeout) const {
  std::unique_lock lock(impl_->guard);
  return impl_->changed.wait_for(lock, timeout, [&] {
    return impl_->jobs.at(std::string(id)).phase == phase;
  });
}
std::function<std::unique_ptr<PublicationDiscoveryTransport>()>
publicationDiscoverySwarmTransport(
    SwarmContentSource::Options options,
    std::vector<std::pair<std::string, std::uint16_t>> nodes,
    std::filesystem::path scratchDirectory, std::chrono::milliseconds timeout) {
  options.enableLocalDiscovery = false;
  options.enableTrackers       = false;
  return [options, nodes = std::move(nodes),
          scratch = std::move(scratchDirectory), timeout] {
    return std::make_unique<DiscoveryTransport>(options, nodes, scratch,
                                                timeout);
  };
}
} // namespace xanadu
