/**
 * @file xudu-swarm-peer.cpp
 * @brief A peer that offers a torrent's content and waits to be asked for it.
 *
 * The other half of the swarm test. It is a separate program rather than a
 * thread in the test because the two peers are meant to be separate machines:
 * run under `ip netns exec`, this has its own network stack, its own address
 * and its own loopback, and it can reach the test only over the veth between
 * them. A thread sharing the test's stack could appear to work for reasons
 * that have nothing to do with BitTorrent.
 *
 * It prints the port it ended up listening on, so the caller does not have to
 * guess one and two of these can run side by side.
 */
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <tuple>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/publication_inbox.hpp"
#include "common/xanadu/publication_outbox.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/swarm.hpp"
#include "common/xanadu/torrent.hpp"
#include "common/xanadu/user_permascroll.hpp"

namespace {

volatile std::sig_atomic_t running = 1;

void stop(int /*signal*/) { running = 0; }

std::string readWholeFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot read " + path);
  }
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int restoreRemote(const int argc, char **argv) {
  if (argc != 6)
    throw std::invalid_argument(
        "usage: xudu-swarm-peer --restore-publication HASH HOST PORT CACHE");
  xanadu::SwarmContentSource::Options options;
  options.enableDht                      = false;
  options.enableTrackers                 = false;
  options.enableLocalDiscovery           = false;
  options.allowManyConnectionsPerAddress = true;
  xanadu::SwarmContentSource source(options);
  const auto port = std::stoul(argv[4]);
  if (port == 0 || port > UINT16_MAX)
    throw std::invalid_argument("invalid peer port");
  const auto fetch = [&](const xanadu::InfoHash &hash) {
    const auto cache = std::filesystem::path(argv[5]) / hash.hex();
    (void)source.addMagnet("magnet:?xt=urn:btih:" + hash.hex(), cache.string());
    source.connectPeer(hash, argv[3], static_cast<std::uint16_t>(port));
    if (!source.waitForMetadata(hash, std::chrono::seconds{30}))
      throw std::runtime_error("publication metadata unavailable: " +
                               hash.hex());
  };
  const auto hash = xanadu::InfoHash::fromHex(argv[2]);
  fetch(hash);
  const auto meta  = source.metainfo(hash);
  const auto bytes = source.readStream(hash, 0, meta->totalLength());
  const auto pub   = xanadu::decodePublication(bytes);
  if (!pub) throw std::runtime_error("publication signature failed");
  std::set<xanadu::InfoHash> dependencies;
  for (const auto &[key, scroll] : pub->scrolls) {
    (void)key;
    for (const auto &segment : scroll.segments)
      dependencies.insert(segment.torrent);
  }
  for (const auto &segment : pub->opsSegments)
    dependencies.insert(segment.torrent);
  for (const auto &dependency : dependencies) fetch(dependency);
  // Retain complete carriers, including provenance files absent from visible
  // text, then install a reader whose ContentSource outlives this network peer.
  for (const auto &dependency : dependencies) {
    const auto metadata = source.torrentMetadata(dependency);
    const auto meta     = source.metainfo(dependency);
    if (!metadata || !meta ||
        source.readStream(dependency, 0, meta->totalLength()).size() !=
            meta->totalLength())
      throw std::runtime_error("publication carrier is incomplete");
    std::ofstream torrent(std::filesystem::path(argv[5]) / dependency.hex() /
                              "metainfo.torrent",
                          std::ios::binary);
    torrent << *metadata;
    torrent.close();
    if (!torrent) throw std::runtime_error("cannot retain downloaded metainfo");
  }
  const auto reader   = std::make_shared<xanadu::UserPermascroll>();
  const auto restored = xanadu::installPublication(
      *pub, {argv[5]}, reader, std::filesystem::path(argv[5]) / "reader");
  std::cout << "restored " << pub->publisher.hex() << " " << pub->sequence
            << " " << restored->opCount() << " " << pub->inventory.size() << " "
            << reader->bytes().size() << "\n";
  return 0;
}

int downloadRemote(const int argc, char **argv) {
  if (argc != 6)
    throw std::invalid_argument(
        "usage: xudu-swarm-peer --download-publication URI HOST PORT CACHE");
  const auto port = std::stoul(argv[4]);
  if (!port || port > UINT16_MAX)
    throw std::invalid_argument("invalid peer port");
  xanadu::SwarmContentSource::Options network;
  network.enableLocalDiscovery           = false;
  network.enableTrackers                 = false;
  network.restrictDhtToDistinctNetworks  = false;
  network.allowManyConnectionsPerAddress = true;
  const std::vector<std::pair<std::string, std::uint16_t>> peers{
      {argv[3], static_cast<std::uint16_t>(port)}};
  xanadu::PublicationInbox inbox(
      {.directory = std::filesystem::path(argv[5]) / "inbox",
       .makeTransport =
           xanadu::publicationDownloadSwarmTransport(network, peers)});
  const auto link = xanadu::MutableLink::parse(argv[2]);
  const auto id   = inbox.submit(link);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{120};
  xanadu::PublicationDownloadStatus status;
  do {
    status = inbox.status(id);
    if (status.phase == xanadu::PublicationDownloadPhase::Failed)
      throw std::runtime_error(status.error);
    if (status.phase == xanadu::PublicationDownloadPhase::Ready) break;
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
  } while (std::chrono::steady_clock::now() < deadline);
  if (status.phase != xanadu::PublicationDownloadPhase::Ready)
    throw std::runtime_error("publication download timed out");
  const auto reader = std::make_shared<xanadu::UserPermascroll>();
  xanadu::Store restored(reader);
  restored.load(status.storePath.string());
  const auto manifest = xanadu::decodePublication(readWholeFile(
      (status.storePath.parent_path() / "publication.xanadoc").string()));
  if (!manifest)
    throw std::runtime_error("retained publication signature failed");
  std::cout << "restored " << manifest->publisher.hex() << " "
            << manifest->sequence << " " << restored.opCount() << " "
            << manifest->inventory.size() << " " << reader->bytes().size()
            << "\n";
  return 0;
}

} // namespace

int main(const int argc, char **argv) {
  if (argc > 1 && std::string_view(argv[1]) == "--download-publication") {
    try {
      return downloadRemote(argc, argv);
    } catch (const std::exception &error) {
      std::cerr << error.what() << "\n";
      return 1;
    }
  }
  if (argc > 1 && std::string_view(argv[1]) == "--restore-publication") {
    try {
      return restoreRemote(argc, argv);
    } catch (const std::exception &error) {
      std::cerr << "xudu-swarm-peer: " << error.what() << "\n";
      return 1;
    }
  }
  if (argc < 3) {
    std::cerr
        << "usage: xudu-swarm-peer <torrent> <data-dir> [listen-address] "
           "[--discoverable] [--publish]\n"
           "       xudu-swarm-peer --restore-publication HASH HOST PORT CACHE\n"
           "\n"
           "Offers the torrent's content to whoever connects, and prints\n"
           "the port it is listening on. Runs until interrupted.\n"
           "\n"
           "--discoverable also announces on the local network, for\n"
           "clients that cannot be told about a peer directly.\n"
           "\n"
           "--publish mints an ed25519 key pair, points it at the\n"
           "torrent as a BEP 46 mutable item, and prints the public key.\n"
           "That key is a name that outlives the torrent it points at.\n";
    return 2;
  }
  bool discoverable = false;
  bool publish      = false;
  for (int i = 1; i < argc; i++) {
    if (std::string{"--discoverable"} == argv[i]) {
      discoverable = true;
    } else if (std::string{"--publish"} == argv[i]) {
      publish = true;
    }
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);

  try {
    xanadu::SwarmContentSource::Options options;
    // Nothing that would find a peer without being asked. The swarm is meant
    // to contain exactly the two peers that were introduced to each other, so
    // that a successful transfer says something about this code rather than
    // about whatever else is on the network.
    //
    // Publishing a name is the one thing that needs the DHT, since a mutable
    // item is a DHT item and there is nowhere else to put it. It stays a
    // closed DHT: no bootstrap routers are configured, so it contains whoever
    // turns up on this wire and nobody else.
    options.enableDht      = publish;
    options.enableTrackers = false;
    // Both nodes here are on the same private network and therefore in the
    // same /8, which the public-DHT rule would read as one node pretending to
    // be two.
    options.restrictDhtToDistinctNetworks = false;
    // A test runs several sessions one after another from the one address the
    // other namespace has. Each is a separate peer in every sense except that
    // address, and refusing the newcomer while the previous connection is
    // still being torn down leaves it waiting for content nobody will send.
    options.allowManyConnectionsPerAddress = true;
    // Local discovery is off for the same reason, unless asked for: a peer
    // that announces itself on the network is a peer the test did not
    // introduce. It is available because some clients -- btfs, for one -- have
    // no way to be told about a peer and can only find one by discovery.
    options.enableLocalDiscovery = discoverable;
    const std::string address =
        (argc > 3 && std::string{"--discoverable"} != argv[3]) ? argv[3]
                                                               : "0.0.0.0";
    options.listenInterfaces = address + ":0";

    xanadu::SwarmContentSource peer(options);
    const auto torrent = readWholeFile(argv[1]);
    // Not seed_mode: libtorrent checks the files against the piece hashes on
    // the way in, so this only claims to be a seed once it has been shown to
    // be one.
    const auto hash = peer.addTorrent(torrent, argv[2], false);

    xanadu::MutableKeys keys;
    if (publish) {
      keys = xanadu::createMutableKeys();
    }

    // Flushed, because the caller is reading this to know where to connect and
    // will otherwise wait for a buffer that never fills.
    std::cout << "port " << peer.listenPort() << "\n"
              << "hash " << hash.hex() << "\n";
    if (publish) {
      std::cout << "pubkey " << keys.publicKey.hex() << "\n";
    }
    std::cout << std::flush;

    auto nextPublish = std::chrono::steady_clock::now();
    while (0 != running) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      // Keeps the session's alert queue drained; an undrained queue eventually
      // stops libtorrent posting the ones that matter.
      std::ignore = peer.metainfo(hash);

      if (publish && std::chrono::steady_clock::now() >= nextPublish) {
        // Republished rather than put once. A DHT item is held by the nodes
        // nearest the target that the publisher knows about, and at startup it
        // knows about nobody: the first node it hears from is the one asking
        // for the name. Republishing is also what keeps an item alive in a
        // real DHT, where entries expire.
        //
        // The sequence number stays at one because this points at one torrent
        // for its whole life. Moving a name is what a higher one is for.
        peer.publishMutable(keys, "", hash, 1);
        nextPublish =
            std::chrono::steady_clock::now() + std::chrono::seconds{1};
      }
    }
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "xudu-swarm-peer: " << err.what() << "\n";
    return 1;
  }
}
