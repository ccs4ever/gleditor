/**
 * @file managed_torrent.cpp
 * @brief Implementation of the system-run background torrent manager.
 */
#include "managed_torrent.hpp"

#include <gleditor/paths.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/entry.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/span.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include "merkle_ledger.hpp"
#include "mutable_link.hpp"
#include "swarm.hpp"
#include "torrent.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>

namespace lt = libtorrent;
namespace fs = std::filesystem;

namespace xudu {

namespace {

InfoHash fromLtHash(const lt::sha1_hash &h) {
  InfoHash out;
  std::memcpy(out.bytes.data(), h.data(), 20);
  return out;
}

std::array<char, 32> toLtKey(const PublicKey &key) {
  std::array<char, 32> out{};
  std::memcpy(out.data(), key.bytes.data(), 32);
  return out;
}

std::string encodedValueOf(const lt::entry &value) {
  std::string out;
  lt::bencode(std::back_inserter(out), value);
  return out;
}

ManagedTorrentState mapTorrentState(const lt::torrent_status::state_t st,
                                    const bool isPaused) {
  if (isPaused) {
    return ManagedTorrentState::Paused;
  }
  switch (st) {
  case lt::torrent_status::checking_files:
  case lt::torrent_status::checking_resume_data:
    return ManagedTorrentState::Checking;
  case lt::torrent_status::downloading_metadata:
  case lt::torrent_status::downloading:
    return ManagedTorrentState::Downloading;
  case lt::torrent_status::finished:
  case lt::torrent_status::seeding:
    return ManagedTorrentState::Seeding;
  default:
    return ManagedTorrentState::Queued;
  }
}

bool writeBufferToFile(const std::string &path, std::string_view data) {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

std::string readFileContents(const std::string &path, bool &ok) {
  ok = false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  ok = in.good() || in.eof();
  return ss.str();
}

} // namespace

struct ManagedItem {
  ManagedTorrentDescriptor desc;
  lt::torrent_handle handle;
  ManagedTorrentStatus status;
  std::int64_t sequence{1};
};

struct SystemTorrentManager::Impl {
  Options options;
  std::unique_ptr<lt::session> session;
  std::map<InfoHash, ManagedItem> items;

  void ensureSession() {
    if (session) {
      return;
    }
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::listen_interfaces,
                 options.listenInterfaces);
    pack.set_bool(lt::settings_pack::enable_dht, options.enableDht);
    pack.set_bool(lt::settings_pack::enable_lsd, options.enableLsd);
    pack.set_bool(lt::settings_pack::dht_restrict_routing_ips,
                  options.restrictDhtToDistinctNetworks);
    pack.set_bool(lt::settings_pack::allow_multiple_connections_per_ip,
                  options.allowManyConnectionsPerAddress);
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::status | lt::alert_category::error |
                     lt::alert_category::storage | lt::alert_category::dht);

    session = std::make_unique<lt::session>(pack);
  }
};

SystemTorrentManager::SystemTorrentManager()
    : SystemTorrentManager(Options{defaultCacheRoot()}) {}

SystemTorrentManager::SystemTorrentManager(Options options)
    : impl_(std::make_unique<Impl>()) {
  if (options.cacheRoot.empty()) {
    options.cacheRoot = defaultCacheRoot();
  }
  impl_->options = std::move(options);
  std::error_code ec;
  fs::create_directories(impl_->options.cacheRoot, ec);
  impl_->ensureSession();
}

SystemTorrentManager::~SystemTorrentManager() = default;
SystemTorrentManager::SystemTorrentManager(SystemTorrentManager &&) noexcept =
    default;
SystemTorrentManager &
SystemTorrentManager::operator=(SystemTorrentManager &&) noexcept = default;

std::string SystemTorrentManager::defaultCacheRoot() {
  const std::string root = gleditor::paths::cacheDir("gleditor/torrents");
  if (!root.empty()) {
    return root;
  }
  const char *home = std::getenv("HOME");
  return home ? std::string(home) + "/.cache/gleditor/torrents"
              : "/tmp/gleditor/torrents";
}

std::string SystemTorrentManager::cacheDirFor(const InfoHash &hash) const {
  return impl_->options.cacheRoot + "/" + hash.hex();
}

InfoHash SystemTorrentManager::registerLedger(
    const MerkleLedger &ledger, const std::optional<MutableKeys> &keys,
    const std::string &salt, std::string *error) {
  const std::string ledgerName = salt.empty() ? "identity_ledger" : salt;
  const MadeTorrent made       = ledger.sealToTorrent(ledgerName);

  const std::string saveDir = cacheDirFor(made.hash);
  std::error_code fsEc;
  fs::create_directories(saveDir, fsEc);
  if (fsEc) {
    if (error) {
      *error = "Could not create cache directory: " + saveDir;
    }
    return {};
  }

  // Write ledger payload files inside the cache directory
  writeBufferToFile(saveDir + "/LEDGER.yaml", ledger.toYaml());
  writeBufferToFile(saveDir + "/ROOT.hex", ledger.rootHex() + "\n");
  std::string keysPub;
  for (const auto &entry : ledger.entries()) {
    if (!entry.publicKeyArmored.empty()) {
      keysPub += entry.publicKeyArmored + "\n";
    }
  }
  if (!keysPub.empty()) {
    writeBufferToFile(saveDir + "/KEYS.pub", keysPub);
  }

  ManagedTorrentDescriptor desc;
  desc.infoHash    = made.hash;
  desc.name        = ledgerName;
  desc.role        = TorrentRole::SystemLedger;
  desc.dataRoot    = saveDir;
  desc.torrentFile = made.file;
  desc.isPinned    = true;

  if (keys) {
    MutableLink mlink;
    mlink.key                = keys->publicKey;
    mlink.salt               = salt;
    mlink.displayName        = ledgerName;
    mlink.currentWhenWritten = made.hash;
    desc.mutableLink         = mlink;
  }

  const InfoHash registered = registerTorrent(desc, error);
  if (registered.isZero()) {
    return {};
  }

  // Publish mutable pointer on DHT if keys were provided
  if (keys) {
    const std::string payload        = encodeMutablePointer(made.hash);
    const MutableKeys k              = *keys;
    const std::int64_t seq           = 1;
    impl_->items[made.hash].sequence = seq;

    impl_->session->dht_put_item(
        toLtKey(k.publicKey),
        [k, payload](lt::entry &value, std::array<char, 64> &sig,
                     std::int64_t &outSeq, const std::string &itemSalt) {
          value        = lt::bdecode(lt::span<char const>{
              payload.data(), static_cast<std::ptrdiff_t>(payload.size())});
          outSeq       = 1;
          const auto s = signMutableItem(
              mutableSigningBuffer(itemSalt, outSeq, encodedValueOf(value)), k);
          std::copy(s.bytes.begin(), s.bytes.end(), sig.begin());
        },
        salt);
  }

  return registered;
}

InfoHash SystemTorrentManager::updateLedger(const InfoHash &oldHash,
                                            const MerkleLedger &ledger,
                                            const MutableKeys &keys,
                                            const std::string &salt,
                                            std::string *error) {
  std::int64_t nextSeq = 2;
  if (const auto it = impl_->items.find(oldHash); it != impl_->items.end()) {
    nextSeq = it->second.sequence + 1;
    removeTorrent(oldHash, false);
  }

  const InfoHash newHash = registerLedger(ledger, keys, salt, error);
  if (!newHash.isZero()) {
    impl_->items[newHash].sequence = nextSeq;
    const std::string payload      = encodeMutablePointer(newHash);
    impl_->session->dht_put_item(
        toLtKey(keys.publicKey),
        [keys, payload, nextSeq](lt::entry &value, std::array<char, 64> &sig,
                                 std::int64_t &outSeq,
                                 const std::string &itemSalt) {
          value        = lt::bdecode(lt::span<char const>{
              payload.data(), static_cast<std::ptrdiff_t>(payload.size())});
          outSeq       = nextSeq;
          const auto s = signMutableItem(
              mutableSigningBuffer(itemSalt, nextSeq, encodedValueOf(value)),
              keys);
          std::copy(s.bytes.begin(), s.bytes.end(), sig.begin());
        },
        salt);
  }
  return newHash;
}

InfoHash SystemTorrentManager::registerSpool(const std::string &spoolPath,
                                             const std::string &name,
                                             std::string *error) {
  bool ok                  = false;
  const std::string buffer = readFileContents(spoolPath, ok);
  if (!ok) {
    if (error) {
      *error = "Could not read spool file at " + spoolPath;
    }
    return {};
  }

  const std::string torrentName =
      name.empty() ? fs::path(spoolPath).filename().string() : name;
  const MadeTorrent made = makeTorrent(buffer, torrentName);

  const std::string saveDir = cacheDirFor(made.hash);
  std::error_code fsEc;
  fs::create_directories(saveDir, fsEc);
  if (fsEc) {
    if (error) {
      *error = "Could not create spool cache directory: " + saveDir;
    }
    return {};
  }

  writeBufferToFile(saveDir + "/" + torrentName, buffer);

  ManagedTorrentDescriptor desc;
  desc.infoHash    = made.hash;
  desc.name        = torrentName;
  desc.role        = TorrentRole::DocumentSpool;
  desc.dataRoot    = saveDir;
  desc.torrentFile = made.file;
  desc.isPinned    = true;

  return registerTorrent(desc, error);
}

InfoHash SystemTorrentManager::registerSlice(const std::string &slicePath,
                                             const std::string &name,
                                             std::string *error) {
  bool ok                  = false;
  const std::string buffer = readFileContents(slicePath, ok);
  if (!ok) {
    if (error) {
      *error = "Could not read slice file at " + slicePath;
    }
    return {};
  }

  const std::string sliceName =
      name.empty() ? fs::path(slicePath).filename().string() : name;
  const MadeTorrent made = makeTorrent(buffer, sliceName);

  const std::string saveDir = cacheDirFor(made.hash);
  std::error_code fsEc;
  fs::create_directories(saveDir, fsEc);
  if (fsEc) {
    if (error) {
      *error = "Could not create slice cache directory: " + saveDir;
    }
    return {};
  }

  writeBufferToFile(saveDir + "/" + sliceName, buffer);

  ManagedTorrentDescriptor desc;
  desc.infoHash    = made.hash;
  desc.name        = sliceName;
  desc.role        = TorrentRole::SliceCache;
  desc.dataRoot    = saveDir;
  desc.torrentFile = made.file;
  desc.isPinned    = false;

  return registerTorrent(desc, error);
}

InfoHash
SystemTorrentManager::registerTorrent(const ManagedTorrentDescriptor &desc,
                                      std::string *error) {
  impl_->ensureSession();

  lt::add_torrent_params params;
  lt::error_code ec;

  if (!desc.torrentFile.empty()) {
    params.ti = std::make_shared<lt::torrent_info>(
        desc.torrentFile.data(), static_cast<int>(desc.torrentFile.size()), ec);
    if (ec) {
      if (error) {
        *error = "Failed to parse torrent metainfo: " + ec.message();
      }
      return {};
    }
  } else if (!desc.magnetUri.empty()) {
    params = lt::parse_magnet_uri(desc.magnetUri, ec);
    if (ec) {
      if (error) {
        *error = "Failed to parse magnet URI: " + ec.message();
      }
      return {};
    }
  } else {
    if (error) {
      *error = "Descriptor must contain either torrentFile or magnetUri";
    }
    return {};
  }

  InfoHash targetHash = desc.infoHash;
  if (targetHash.isZero()) {
    if (params.ti) {
      targetHash = fromLtHash(params.ti->info_hashes().v1);
    } else if (params.info_hashes.has_v1()) {
      targetHash = fromLtHash(params.info_hashes.v1);
    }
  }

  const std::string saveDir =
      desc.dataRoot.empty() ? cacheDirFor(targetHash) : desc.dataRoot;
  std::error_code fsEc;
  fs::create_directories(saveDir, fsEc);
  params.save_path = saveDir;

  // If registering local content, set seed mode to avoid re-checking from
  // scratch
  if (!desc.torrentFile.empty()) {
    params.flags |= lt::torrent_flags::seed_mode;
  }

  lt::torrent_handle handle = impl_->session->add_torrent(params, ec);
  if (ec) {
    if (error) {
      *error = "libtorrent add_torrent failed: " + ec.message();
    }
    return {};
  }

  ManagedItem item;
  item.desc             = desc;
  item.desc.infoHash    = targetHash;
  item.desc.dataRoot    = saveDir;
  item.handle           = handle;
  item.status.infoHash  = targetHash;
  item.status.name      = desc.name;
  item.status.role      = desc.role;
  item.status.savePath  = saveDir;
  item.status.state     = ManagedTorrentState::Seeding;
  item.status.progress  = 1.0F;
  item.status.magnetUri = desc.magnetUri;

  impl_->items[targetHash] = std::move(item);
  return targetHash;
}

bool SystemTorrentManager::removeTorrent(const InfoHash &hash,
                                         const bool deleteFiles) {
  const auto it = impl_->items.find(hash);
  if (it == impl_->items.end()) {
    return false;
  }

  if (impl_->session && it->second.handle.is_valid()) {
    impl_->session->remove_torrent(it->second.handle,
                                   deleteFiles ? lt::session::delete_files
                                               : lt::remove_flags_t{});
  }

  if (deleteFiles && !it->second.desc.dataRoot.empty()) {
    std::error_code fsEc;
    fs::remove_all(it->second.desc.dataRoot, fsEc);
  }

  impl_->items.erase(it);
  return true;
}

bool SystemTorrentManager::pauseTorrent(const InfoHash &hash) {
  const auto it = impl_->items.find(hash);
  if (it == impl_->items.end() || !it->second.handle.is_valid()) {
    return false;
  }
  it->second.handle.pause();
  it->second.status.state = ManagedTorrentState::Paused;
  return true;
}

bool SystemTorrentManager::resumeTorrent(const InfoHash &hash) {
  const auto it = impl_->items.find(hash);
  if (it == impl_->items.end() || !it->second.handle.is_valid()) {
    return false;
  }
  it->second.handle.resume();
  it->second.status.state = ManagedTorrentState::Seeding;
  return true;
}

void SystemTorrentManager::poll() {
  if (!impl_->session) {
    return;
  }

  std::vector<lt::alert *> alerts;
  impl_->session->pop_alerts(&alerts);

  for (auto &[hash, item] : impl_->items) {
    if (!item.handle.is_valid()) {
      continue;
    }
    const lt::torrent_status st = item.handle.status();
    item.status.progress        = st.progress;
    item.status.downloadedBytes = static_cast<std::uint64_t>(st.total_done);
    item.status.uploadedBytes   = static_cast<std::uint64_t>(st.total_upload);
    item.status.totalBytes      = static_cast<std::uint64_t>(st.total_wanted);
    item.status.numPeers        = st.num_peers;
    item.status.numSeeds        = st.num_seeds;
    item.status.state =
        mapTorrentState(st.state, bool(st.flags & lt::torrent_flags::paused));
  }
}

std::vector<ManagedTorrentStatus> SystemTorrentManager::listTorrents() const {
  std::vector<ManagedTorrentStatus> list;
  list.reserve(impl_->items.size());
  for (const auto &[hash, item] : impl_->items) {
    list.push_back(item.status);
  }
  return list;
}

std::optional<ManagedTorrentStatus>
SystemTorrentManager::getStatus(const InfoHash &hash) const {
  const auto it = impl_->items.find(hash);
  if (it == impl_->items.end()) {
    return std::nullopt;
  }
  return it->second.status;
}

void SystemTorrentManager::connectPeer(const InfoHash &hash,
                                       const std::string &host,
                                       const std::uint16_t port) {
  const auto it = impl_->items.find(hash);
  if (it != impl_->items.end() && it->second.handle.is_valid()) {
    it->second.handle.connect_peer(
        lt::tcp::endpoint(boost::asio::ip::make_address(host), port));
  }
}

void SystemTorrentManager::addDhtNode(const std::string &host,
                                      const std::uint16_t port) {
  if (impl_->session) {
    impl_->session->add_dht_node({host, port});
  }
}

std::uint16_t SystemTorrentManager::listenPort() const {
  if (!impl_->session) {
    return 0;
  }
  return static_cast<std::uint16_t>(impl_->session->listen_port());
}

} // namespace xudu
