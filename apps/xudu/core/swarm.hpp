/**
 * @file swarm.hpp
 * @brief Content fetched from peers rather than from a disk here.
 *
 * This is the implementation that makes a torrent-backed reference behave the
 * way the argument for using one claims it does. Until now a reference could
 * be made to content addressed by its hash, and resolved only if this machine
 * already had the bytes -- which quietly reintroduces the dependency on one
 * particular machine that content addressing was supposed to remove.
 *
 * Nothing above this changes. Resolver still fetches whole pieces and hashes
 * them against the torrent before returning anything, and it does not know or
 * care whether they came off a local disk or from a stranger. That is the
 * point of verifying: a peer is not trusted, so it does not have to be.
 *
 * Only built when libtorrent is installed. Everything else in the engine works
 * without it, and a build without it simply has no swarm to offer.
 */
#ifndef XUDU_SWARM_H
#define XUDU_SWARM_H

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "resolver.hpp"
#include "torrent.hpp"

namespace xudu {

/// Whether this build can join a swarm at all.
[[nodiscard]] bool swarmSupported();

/**
 * @class SwarmContentSource
 * @brief A ContentSource that gets its bytes from BitTorrent peers.
 *
 * Reads block until the pieces they need arrive or the deadline passes, and a
 * read that times out reports what it has -- which the resolver treats as "not
 * available" rather than as a short answer. So a document quoting content
 * nobody is seeding renders with the quotation blank instead of hanging.
 */
class SwarmContentSource : public ContentSource {
public:
  struct Options {
    /**
     * @brief What to listen on, in libtorrent's notation.
     *
     * Port zero asks the operating system to choose, which is what lets
     * several of these run side by side. listenPort() reports what it got.
     */
    std::string listenInterfaces{"0.0.0.0:0"};
    /**
     * @brief Whether to look for peers on the wider network.
     *
     * All three off gives a swarm that contains exactly the peers it was told
     * about through connectPeer(), which is what makes a test of this
     * deterministic and what makes it possible at all without the internet.
     */
    bool enableDht{true};
    bool enableLocalDiscovery{true};
    bool enableTrackers{true};
    /// How long a read waits before giving up on the pieces it needs.
    std::chrono::milliseconds readTimeout{std::chrono::seconds{30}};
    /// How long to wait for a magnet's metadata.
    std::chrono::milliseconds metadataTimeout{std::chrono::seconds{60}};
  };

  /// @throws std::runtime_error when the build has no libtorrent.
  ///
  /// Two declarations rather than one with a default argument: Options has
  /// default member initialisers, and a class is not complete enough inside
  /// its own definition for `= {}` to use them.
  SwarmContentSource();
  explicit SwarmContentSource(Options options);
  ~SwarmContentSource() override;

  /**
   * @brief Offer content this machine already has, and take part in its swarm.
   *
   * @param torrentFile The contents of a .torrent file.
   * @param dataRoot Where its files are, or are to be written.
   * @param seeding True when the files are already complete, which skips
   *        waiting to download what is there.
   */
  InfoHash addTorrent(std::string_view torrentFile, const std::string &dataRoot,
                      bool seeding = false);

  /**
   * @brief Join a swarm named only by a magnet link.
   *
   * The metadata is what a magnet lacks, so it is fetched from a peer (BEP 9)
   * and then checked: libtorrent will not accept an info dictionary that does
   * not hash to the name the link carried, which is the whole reason a magnet
   * can be trusted at all.
   *
   * Returns as soon as the swarm has been joined; use waitForMetadata() to
   * block until the content can actually be described.
   */
  InfoHash addMagnet(const std::string &uri, const std::string &dataRoot);

  /**
   * @brief Tell a swarm about a peer directly.
   *
   * Ordinarily peers are discovered through a tracker or the DHT. Being able
   * to name one is what allows a swarm of exactly two known machines, which is
   * how the network path here is tested without the internet -- and it is also
   * what a reference could carry if it wanted to name a known source.
   */
  void connectPeer(const InfoHash &hash, const std::string &host,
                   std::uint16_t port);

  /// Block until the content behind @p hash can be described, or the timeout
  /// passes. Already true for anything added from a .torrent file.
  [[nodiscard]] bool waitForMetadata(const InfoHash &hash,
                                     std::chrono::milliseconds timeout);

  /// The port actually listened on, once the session is up.
  [[nodiscard]] std::uint16_t listenPort() const;

  /// How many peers this swarm is connected to right now. Note that a peer
  /// disconnects once it has nothing left to give, so this drops back to zero
  /// after a completed transfer.
  [[nodiscard]] int peerCount(const InfoHash &hash) const;

  /**
   * @brief Bytes this swarm has received from peers.
   *
   * Unlike peerCount() this does not go back down, which makes it the honest
   * way to ask whether content arrived over the network or was simply already
   * on the disk.
   */
  [[nodiscard]] std::int64_t bytesFromPeers(const InfoHash &hash) const;

  // -- ContentSource --------------------------------------------------------

  [[nodiscard]] const Metainfo *metainfo(const InfoHash &hash) const override;
  [[nodiscard]] std::string readStream(const InfoHash &hash,
                                       std::uint64_t offset,
                                       std::uint64_t length) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace xudu

#endif // XUDU_SWARM_H
// vi: set sw=2 sts=2 ts=2 et:
