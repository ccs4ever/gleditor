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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "microversion.hpp"
#include "mutable_link.hpp"
#include "ops.hpp"
#include "resolver.hpp"
#include "torrent.hpp"

namespace xudu {

/// What a mutable name points at, and how recently the publisher said so.
struct MutablePointer {
  InfoHash hash;
  /**
   * @brief The sequence number the pointer was published under.
   *
   * The only ordering there is. Two answers to the same name are not two
   * opinions to be reconciled -- the higher sequence number is the later one,
   * and that is the whole of what "current" means here.
   */
  std::int64_t sequence{};
};

/// A publisher's identity: the name, and the right to move it.
struct MutableKeys {
  PublicKey publicKey;
  SecretKey secretKey;
};

/**
 * @brief Mint a new identity.
 *
 * Every call produces a name nobody has used before, without asking anyone,
 * which is the property that makes a public key a usable permanent name in the
 * first place: there is no registry to be turned off.
 *
 * @throws std::runtime_error when the build has no libtorrent.
 */
[[nodiscard]] MutableKeys createMutableKeys();

/// Sign @p buffer, which must be what mutableSigningBuffer() produced.
/// @throws std::runtime_error when the build has no libtorrent.
[[nodiscard]] Signature signMutableItem(std::string_view buffer,
                                        const MutableKeys &keys);

/**
 * @brief Whether @p signature over @p buffer really is @p key's.
 *
 * This is the whole basis on which an answer from a stranger can be believed,
 * so it is worth being clear about what it does not depend on: not on which
 * node answered, not on how many agreed, and not on the DHT being honest.
 *
 * @throws std::runtime_error when the build has no libtorrent.
 */
[[nodiscard]] bool verifyMutableItem(std::string_view buffer,
                                     const Signature &signature,
                                     const PublicKey &key);

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
    /**
     * @brief Whether to keep the DHT routing table to one node per /8.
     *
     * On the open internet this is a defence worth having: it costs an
     * attacker a spread of addresses to fill a routing table. On a private
     * network it is fatal rather than cautious, because every node there is in
     * the same /8 and the rule leaves a DHT of one -- which is the shape a
     * permascroll server on a LAN has, and the shape the tests have.
     */
    bool restrictDhtToDistinctNetworks{true};
    /**
     * @brief Whether to accept a second connection from an address already
     *        connected.
     *
     * Off by default, as libtorrent has it, where refusing is a defence
     * against one host opening many connections. It is wrong for the case
     * where several peers genuinely share an address: a seeder will refuse the
     * newcomer for as long as the previous connection is still being torn
     * down, and the newcomer waits for content that will never be sent.
     *
     * That is the shape of a test harness running several sessions on one
     * address, and of anything behind a single outward address.
     */
    bool allowManyConnectionsPerAddress{false};
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

  /**
   * @brief Tell the DHT about a node directly.
   *
   * The equivalent of connectPeer() for the other network. A public DHT is
   * bootstrapped from well-known routers; a closed one has none, so somebody
   * has to be named. Also what a permascroll server would be pointed at.
   */
  void addDhtNode(const std::string &host, std::uint16_t port);

  /**
   * @brief Ask the DHT what a mutable name currently points at.
   *
   * The get is re-issued until an answer arrives or the timeout passes, rather
   * than asked once: a two-node DHT does not know its neighbours until they
   * have spoken, and the first query is often what introduces them.
   *
   * Only an answer whose signature checks out against the link's own public
   * key is returned. An unsigned, wrongly signed, or non-BEP 46 value is
   * treated as no answer at all -- not as a partial one -- for the same reason
   * a piece that fails its hash is: downstream cannot tell a checked answer
   * from an unchecked one, so the check has to be the gate.
   *
   * A single call is a snapshot, and the specification is explicit that
   * freshness comes from asking again: consumers "periodically poll such ID by
   * issuing get requests to see whether the v property of the response has
   * updated". So this reports what the DHT will say now, not what the
   * publisher has most recently signed.
   *
   * @return The pointer, or nothing when the name has no answer here.
   */
  [[nodiscard]] std::optional<MutablePointer>
  resolveMutable(const MutableLink &link, std::chrono::milliseconds timeout);

  /**
   * @brief Point a name at an info hash.
   *
   * The publisher's half. It exists here mostly because a resolver that has
   * never been tested against a real publisher is a resolver nobody should
   * believe -- and because sealing a permascroll segment ends exactly here.
   *
   * @param sequence Must exceed what was published before, or the DHT keeps
   *        what it has. That is the rule that makes an old answer losable
   *        rather than authoritative.
   */
  void publishMutable(const MutableKeys &keys, const std::string &salt,
                      const InfoHash &hash, std::int64_t sequence);

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

  // -- Real-Time Live Collaborative Swarm Operations -----------------------

  /// A live operation broadcasted between connected collaborative swarm peers.
  struct LiveOpBroadcast {
    InfoHash swarmHash;
    MicroversionId version;
    Op op;
    std::string primediaText;
    std::int64_t timestamp{0};
  };

  using LiveOpHandler = std::function<void(const LiveOpBroadcast &)>;

  /**
   * @brief Broadcast an unsealed live operation to active peers in @p
   * swarmHash.
   */
  void broadcastLiveOp(const InfoHash &swarmHash, const MicroversionId &version,
                       const Op &op, std::string_view primediaText = "");

  /**
   * @brief Register a callback for incoming live operations from peers.
   */
  void onLiveOp(LiveOpHandler handler);

  /**
   * @brief Retrieve and clear any queued live operations received from peers.
   */
  [[nodiscard]] std::vector<LiveOpBroadcast> takePendingLiveOps();

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
