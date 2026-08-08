#include "swarm.hpp"

#include <stdexcept>

#if XUDU_ENABLE_SWARM

#include <algorithm>
#include <chrono>
#include <iterator>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>

namespace lt = libtorrent;

namespace xudu {

namespace {

/// One of ours for libtorrent's v1 hash.
InfoHash fromLt(const lt::sha1_hash &hash) {
  InfoHash out;
  std::copy(hash.data(), hash.data() + out.bytes.size(), out.bytes.begin());
  return out;
}

} // namespace

bool swarmSupported() { return true; }

/// One swarm this source takes part in.
struct Swarm {
  lt::torrent_handle handle;
  /// Parsed from the torrent libtorrent ended up with, so that verification
  /// runs against this codebase's own reading of the metadata rather than
  /// against libtorrent's. A disagreement between the two is exactly the kind
  /// of thing that must not pass unnoticed.
  std::unique_ptr<Metainfo> meta;
  /// Pieces read back so far, kept because a read of one byte fetches a whole
  /// piece and the next read very likely wants the same one.
  std::map<int, std::string> pieces;
  /**
   * @brief Peers this swarm has been told to connect to.
   *
   * Remembered rather than connected once and forgotten. libtorrent refuses a
   * connection while a torrent is still checking its files -- "if the torrent
   * is uninitialized or in queued or checking mode, this will throw" -- and a
   * torrent added a moment ago is usually exactly that. Retrying while the
   * session is pumped is both simpler and closer to what a real client does
   * than trying to find the one right moment to ask.
   */
  std::vector<lt::tcp::endpoint> wantedPeers;
  /**
   * @brief Pieces that have been asked for but have not arrived.
   *
   * Re-requested on every pump, for the same reason peers are: a torrent added
   * a moment ago is still checking its files, and a deadline set during that
   * window is thrown away. Asking once and waiting produced a read that timed
   * out while the session sat there having forgotten it was wanted.
   *
   * Setting a deadline that is already set is cheap and has no other effect,
   * so re-asking costs nothing once the request has taken.
   */
  std::set<int> pendingPieces;
};

struct SwarmContentSource::Impl {
  SwarmContentSource::Options options;
  lt::session session;
  /// Mutable because reading is logically const -- it answers a question about
  /// content -- while unavoidably driving a session and filling a cache.
  mutable std::map<InfoHash, Swarm> swarms;

  explicit Impl(SwarmContentSource::Options aOptions)
      : options(std::move(aOptions)), session(settings(options)) {}

  static lt::settings_pack settings(const SwarmContentSource::Options &options) {
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::listen_interfaces, options.listenInterfaces);
    pack.set_bool(lt::settings_pack::enable_dht, options.enableDht);
    pack.set_bool(lt::settings_pack::enable_lsd, options.enableLocalDiscovery);
    // Trackers are not disabled by a setting; the flag below is applied per
    // torrent instead. What is disabled here is everything that would find a
    // peer without being asked.
    pack.set_bool(lt::settings_pack::enable_upnp, false);
    pack.set_bool(lt::settings_pack::enable_natpmp, false);
    // Only the alerts that are acted on. The default set is chatty enough that
    // pumping it would be most of the cost of a read.
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::status | lt::alert_category::storage |
                     lt::alert_category::error);
    pack.set_str(lt::settings_pack::user_agent, "xudu/0.1");
    return pack;
  }

  /**
   * @brief Ask again for any peer that has not been reached yet.
   *
   * A no-op once a swarm has a peer. The check for the checking states is what
   * makes this worth doing at all: that is the window in which the request
   * would otherwise be thrown away.
   */
  /// Ask again for any piece that has been requested and not yet arrived.
  void requestPendingPieces() {
    for (auto &[hash, swarm] : swarms) {
      if (swarm.pendingPieces.empty()) {
        continue;
      }
      const auto status = swarm.handle.status();
      if (lt::torrent_status::checking_files == status.state ||
          lt::torrent_status::checking_resume_data == status.state) {
        continue;
      }
      for (auto piece = swarm.pendingPieces.begin();
           piece != swarm.pendingPieces.end();) {
        if (swarm.pieces.contains(*piece)) {
          // Arrived; stop asking for it.
          piece = swarm.pendingPieces.erase(piece);
          continue;
        }
        const lt::piece_index_t index{*piece};
        swarm.handle.piece_priority(index, lt::top_priority);
        swarm.handle.set_piece_deadline(index, 0,
                                        lt::torrent_handle::alert_when_available);
        ++piece;
      }
    }
  }

  void tryConnectPeers() {
    for (auto &[hash, swarm] : swarms) {
      if (swarm.wantedPeers.empty()) {
        continue;
      }
      const auto status = swarm.handle.status();
      if (lt::torrent_status::checking_files == status.state ||
          lt::torrent_status::checking_resume_data == status.state) {
        continue;
      }
      if (status.num_peers > 0) {
        continue;
      }
      for (const auto &peer : swarm.wantedPeers) {
        try {
          swarm.handle.connect_peer(peer);
        } catch (const std::exception &) {
          // Still not ready, or the peer is not there yet. Either way this is
          // tried again on the next pump rather than being fatal.
        }
      }
    }
  }

  /// Drain the session's alerts, updating whatever they are about.
  void pump() {
    tryConnectPeers();
    requestPendingPieces();
    std::vector<lt::alert *> alerts;
    session.pop_alerts(&alerts);
    for (const auto *alert : alerts) {
      if (const auto *got = lt::alert_cast<lt::read_piece_alert>(alert);
          nullptr != got) {
        if (got->error) {
          continue;
        }
        const auto hash = fromLt(got->handle.info_hashes().v1);
        if (const auto found = swarms.find(hash); found != swarms.end()) {
          found->second.pieces.emplace(
              static_cast<int>(got->piece),
              std::string(got->buffer.get(), static_cast<std::size_t>(got->size)));
        }
      } else if (const auto *meta =
                     lt::alert_cast<lt::metadata_received_alert>(alert);
                 nullptr != meta) {
        adoptMetadata(meta->handle);
      }
    }
  }

  /// Take the metadata a handle now has and parse it with this codebase's own
  /// reader, so that the piece hashes verification runs against are the ones
  /// this program understands.
  void adoptMetadata(const lt::torrent_handle &handle) {
    const auto hash  = fromLt(handle.info_hashes().v1);
    const auto found = swarms.find(hash);
    if (found == swarms.end() || nullptr != found->second.meta) {
      return;
    }
    const auto info = handle.torrent_file();
    if (!info) {
      return;
    }
    // Re-encoded from what libtorrent holds. The info dictionary it kept is
    // the one whose hash it checked against the magnet, so this round trip
    // preserves the identity -- and Metainfo::parse recomputes the hash from
    // the bytes anyway, which is what would catch it if it did not.
    lt::create_torrent rebuilt(*info);
    std::string encoded;
    lt::bencode(std::back_inserter(encoded), rebuilt.generate());
    auto parsed = std::make_unique<Metainfo>(Metainfo::parse(encoded));
    if (parsed->hash() != hash) {
      // Refusing beats guessing: content whose name does not match what was
      // asked for is not the content that was asked for.
      return;
    }
    found->second.meta = std::move(parsed);
  }

  /// Wait until @p ready holds, pumping alerts, or the deadline passes.
  template <typename Ready>
  bool waitUntil(Ready ready, const std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      pump();
      if (ready()) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      // Woken by the session when something happens, rather than spun on, but
      // with a bound so the deadline is honoured even in silence.
      session.wait_for_alert(std::chrono::milliseconds{50});
    }
  }
};

SwarmContentSource::SwarmContentSource() : SwarmContentSource(Options{}) {}

SwarmContentSource::SwarmContentSource(Options options)
    : impl(std::make_unique<Impl>(std::move(options))) {}

SwarmContentSource::~SwarmContentSource() = default;

InfoHash SwarmContentSource::addTorrent(const std::string_view torrentFile,
                                        const std::string &dataRoot,
                                        const bool seeding) {
  // Parsed here as well as by libtorrent. The one this program verifies
  // against must be the one this program read.
  auto meta = std::make_unique<Metainfo>(Metainfo::parse(torrentFile));
  const auto hash = meta->hash();

  lt::add_torrent_params params;
  params.ti = std::make_shared<lt::torrent_info>(
      lt::span<const char>{torrentFile.data(),
                           static_cast<std::ptrdiff_t>(torrentFile.size())},
      lt::from_span);
  params.save_path = dataRoot;
  if (seeding) {
    // The files are already complete, so there is nothing to fetch; this skips
    // re-hashing them on the way in.
    params.flags |= lt::torrent_flags::seed_mode;
  }
  if (!impl->options.enableTrackers) {
    params.trackers.clear();
  }

  auto handle = impl->session.add_torrent(params);
  impl->swarms.insert_or_assign(hash, Swarm{handle, std::move(meta), {}});
  return hash;
}

InfoHash SwarmContentSource::addMagnet(const std::string &uri,
                                       const std::string &dataRoot) {
  // Parsed by this codebase first, so that an unusable link is refused with
  // this program's own complaint rather than libtorrent's.
  const auto link = MagnetLink::parse(uri);

  lt::error_code error;
  auto params = lt::parse_magnet_uri(uri, error);
  if (error) {
    throw std::runtime_error("magnet link: " + error.message());
  }
  params.save_path = dataRoot;
  if (!impl->options.enableTrackers) {
    params.trackers.clear();
  }

  auto handle = impl->session.add_torrent(params);
  // No metadata yet: that is what a magnet lacks, and it arrives from a peer.
  impl->swarms.insert_or_assign(link.hash, Swarm{handle, nullptr, {}});
  return link.hash;
}

void SwarmContentSource::connectPeer(const InfoHash &hash,
                                     const std::string &host,
                                     const std::uint16_t port) {
  const auto found = impl->swarms.find(hash);
  if (found == impl->swarms.end()) {
    return;
  }
  lt::error_code error;
  const auto address = lt::make_address(host, error);
  if (error) {
    throw std::runtime_error("peer address " + host + ": " + error.message());
  }
  found->second.wantedPeers.emplace_back(address, port);
  // Attempted now in case the torrent is already running, and again on every
  // pump until it takes.
  impl->tryConnectPeers();
}

bool SwarmContentSource::waitForMetadata(const InfoHash &hash,
                                         const std::chrono::milliseconds timeout) {
  return impl->waitUntil(
      [this, &hash] {
        const auto found = impl->swarms.find(hash);
        return found != impl->swarms.end() && nullptr != found->second.meta;
      },
      timeout);
}

std::uint16_t SwarmContentSource::listenPort() const {
  return static_cast<std::uint16_t>(impl->session.listen_port());
}

int SwarmContentSource::peerCount(const InfoHash &hash) const {
  const auto found = impl->swarms.find(hash);
  if (found == impl->swarms.end()) {
    return 0;
  }
  return found->second.handle.status().num_peers;
}

std::int64_t SwarmContentSource::bytesFromPeers(const InfoHash &hash) const {
  const auto found = impl->swarms.find(hash);
  if (found == impl->swarms.end()) {
    return 0;
  }
  return found->second.handle.status().all_time_download;
}

const Metainfo *SwarmContentSource::metainfo(const InfoHash &hash) const {
  impl->pump();
  const auto found = impl->swarms.find(hash);
  return found == impl->swarms.end() ? nullptr : found->second.meta.get();
}

std::string SwarmContentSource::readStream(const InfoHash &hash,
                                           const std::uint64_t offset,
                                           const std::uint64_t length) const {
  impl->pump();
  const auto found = impl->swarms.find(hash);
  if (found == impl->swarms.end() || nullptr == found->second.meta) {
    return {};
  }
  auto &swarm      = found->second;
  const auto &meta = *swarm.meta;
  if (0 == length) {
    return {};
  }

  const auto [firstPiece, endPiece] = meta.piecesForRange(offset, length);
  if (endPiece <= firstPiece) {
    return {};
  }

  // Ask for exactly the pieces this range needs, rather than the whole file.
  // A quotation is usually a sentence out of something long, and fetching the
  // rest would make a reference cost what a copy costs.
  std::vector<int> wanted;
  for (auto piece = firstPiece; piece < endPiece; piece++) {
    const auto index = static_cast<int>(piece);
    if (!swarm.pieces.contains(index)) {
      wanted.push_back(index);
    }
  }

  if (!wanted.empty()) {
    // Recorded rather than requested outright. A deadline set while the
    // torrent is still checking its files is discarded, and a torrent added a
    // moment ago is usually exactly that; pump() re-asks until the piece
    // arrives. A deadline with alert_when_available is how libtorrent is told
    // to hand the bytes back once it has them, whether it just downloaded them
    // or already had them on disk.
    swarm.pendingPieces.insert(wanted.begin(), wanted.end());
    const auto haveAll = [&swarm, &wanted] {
      return std::ranges::all_of(wanted, [&swarm](const int index) {
        return swarm.pieces.contains(index);
      });
    };
    if (!impl->waitUntil(haveAll, impl->options.readTimeout)) {
      // Timed out. Reporting nothing is what the contract asks for: the
      // resolver reads that as "not available" and the quotation renders
      // blank, which beats blocking a frame for as long as nobody is seeding.
      return {};
    }
  }

  // Stitch the pieces together and cut out the range asked for. The bytes are
  // not trusted here and are not checked here either: Resolver hashes each
  // piece against the torrent, which is the one place that decision belongs.
  const auto readFrom = static_cast<std::uint64_t>(firstPiece) * meta.pieceLength();
  std::string joined;
  for (auto piece = firstPiece; piece < endPiece; piece++) {
    const auto held = swarm.pieces.find(static_cast<int>(piece));
    if (held == swarm.pieces.end()) {
      return {};
    }
    joined += held->second;
  }

  const auto into = static_cast<std::size_t>(offset - readFrom);
  if (into >= joined.size()) {
    return {};
  }
  return joined.substr(into, static_cast<std::size_t>(length));
}

} // namespace xudu

#else // XUDU_ENABLE_SWARM

namespace xudu {

bool swarmSupported() { return false; }

struct SwarmContentSource::Impl {};

SwarmContentSource::SwarmContentSource() : SwarmContentSource(Options{}) {}

SwarmContentSource::SwarmContentSource(Options /*options*/) {
  throw std::runtime_error(
      "this build has no swarm support: it was compiled without libtorrent");
}
SwarmContentSource::~SwarmContentSource() = default;

InfoHash SwarmContentSource::addTorrent(std::string_view, const std::string &,
                                        bool) {
  return {};
}
InfoHash SwarmContentSource::addMagnet(const std::string &, const std::string &) {
  return {};
}
void SwarmContentSource::connectPeer(const InfoHash &, const std::string &,
                                     std::uint16_t) {}
bool SwarmContentSource::waitForMetadata(const InfoHash &,
                                         std::chrono::milliseconds) {
  return false;
}
std::uint16_t SwarmContentSource::listenPort() const { return 0; }
int SwarmContentSource::peerCount(const InfoHash &) const { return 0; }
std::int64_t SwarmContentSource::bytesFromPeers(const InfoHash &) const {
  return 0;
}
const Metainfo *SwarmContentSource::metainfo(const InfoHash &) const {
  return nullptr;
}
std::string SwarmContentSource::readStream(const InfoHash &, std::uint64_t,
                                           std::uint64_t) const {
  return {};
}

} // namespace xudu

#endif // XUDU_ENABLE_SWARM

// vi: set sw=2 sts=2 ts=2 et:
