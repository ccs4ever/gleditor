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
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

#include <xudu/core/swarm.hpp>
#include <xudu/core/torrent.hpp>

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

} // namespace

int main(const int argc, char **argv) {
  if (argc < 3) {
    std::cerr << "usage: xudu-swarm-peer <torrent> <data-dir> [listen-address] "
                 "[--discoverable] [--publish]\n"
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
  if (!xudu::swarmSupported()) {
    std::cerr << "built without libtorrent; nothing to seed with\n";
    return 77; // the conventional "skipped" status
  }

  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);

  try {
    xudu::SwarmContentSource::Options options;
    // Nothing that would find a peer without being asked. The swarm is meant
    // to contain exactly the two peers that were introduced to each other, so
    // that a successful transfer says something about this code rather than
    // about whatever else is on the network.
    //
    // Publishing a name is the one thing that needs the DHT, since a mutable
    // item is a DHT item and there is nowhere else to put it. It stays a
    // closed DHT: no bootstrap routers are configured, so it contains whoever
    // turns up on this wire and nobody else.
    options.enableDht            = publish;
    options.enableTrackers       = false;
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

    xudu::SwarmContentSource peer(options);
    const auto torrent = readWholeFile(argv[1]);
    // Not seed_mode: libtorrent checks the files against the piece hashes on
    // the way in, so this only claims to be a seed once it has been shown to
    // be one.
    const auto hash = peer.addTorrent(torrent, argv[2], false);

    xudu::MutableKeys keys;
    if (publish) {
      keys = xudu::createMutableKeys();
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
      static_cast<void>(peer.metainfo(hash));

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
        nextPublish = std::chrono::steady_clock::now() + std::chrono::seconds{1};
      }
    }
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "xudu-swarm-peer: " << err.what() << "\n";
    return 1;
  }
}

// vi: set sw=2 sts=2 ts=2 et:
