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
    std::cerr << "usage: xudu-swarm-peer <torrent> <data-dir> [listen-address]\n"
                 "\n"
                 "Offers the torrent's content to whoever connects, and prints\n"
                 "the port it is listening on. Runs until interrupted.\n";
    return 2;
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
    options.enableDht             = false;
    options.enableLocalDiscovery  = false;
    options.enableTrackers        = false;
    options.listenInterfaces =
        std::string{argc > 3 ? argv[3] : "0.0.0.0"} + ":0";

    xudu::SwarmContentSource peer(options);
    const auto torrent = readWholeFile(argv[1]);
    // Not seed_mode: libtorrent checks the files against the piece hashes on
    // the way in, so this only claims to be a seed once it has been shown to
    // be one.
    const auto hash = peer.addTorrent(torrent, argv[2], false);

    // Flushed, because the caller is reading this to know where to connect and
    // will otherwise wait for a buffer that never fills.
    std::cout << "port " << peer.listenPort() << "\n"
              << "hash " << hash.hex() << "\n"
              << std::flush;

    while (0 != running) {
      std::this_thread::sleep_for(std::chrono::milliseconds{100});
      // Keeps the session's alert queue drained; an undrained queue eventually
      // stops libtorrent posting the ones that matter.
      static_cast<void>(peer.metainfo(hash));
    }
    return 0;
  } catch (const std::exception &err) {
    std::cerr << "xudu-swarm-peer: " << err.what() << "\n";
    return 1;
  }
}

// vi: set sw=2 sts=2 ts=2 et:
