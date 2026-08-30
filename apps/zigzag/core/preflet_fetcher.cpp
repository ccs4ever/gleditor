#include "preflet_fetcher.hpp"
#include "zzcore.hpp"

#include <xudu/core/torrent.hpp>

#include <gleditor/color.hpp>
#include <gleditor/paths.hpp>

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace lt = libtorrent;
namespace fs = std::filesystem;

namespace zigzag {

namespace {

constexpr std::int64_t maxSliceDownloadBytes = 64LL * 1024 * 1024;
constexpr auto metadataTimeout               = std::chrono::seconds(90);
constexpr std::size_t maxSliceFileBytes      = 16 * 1024 * 1024;

std::string readFileCapped(const std::string &path, bool &ok) {
  ok = false;
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  std::string contents = buffer.str();
  if (contents.size() > maxSliceFileBytes) {
    return {};
  }
  ok = true;
  return contents;
}

std::string toLowerHex(const std::string_view text) {
  std::string out{text};
  for (char &c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string sha1HexOfFile(const std::string &path, bool &ok) {
  const std::string contents = readFileCapped(path, ok);
  if (!ok) {
    return {};
  }
  const auto digest = xudu::sha1(contents);
  return gleditor::color::toHex(std::string_view{
      reinterpret_cast<const char *>(digest.data()), digest.size()});
}

} // namespace

struct PrefletFetcher::Impl {
  std::unique_ptr<lt::session> session;
  lt::torrent_handle handle;
  PrefletFetcher::Progress progress;

  std::string expected_hash;
  std::string preferred_filename;
  std::string save_path;

  bool metadata_received = false;
  std::chrono::steady_clock::time_point started;

  void fail(const std::string &message) {
    progress.status   = PrefletFetcher::Status::Failed;
    progress.message  = message;
    progress.fraction = 0.0F;
    std::cerr << std::format("preflet: fetch failed -- {}\n", message);
    if (session && handle.is_valid()) {
      session->remove_torrent(handle);
      handle = lt::torrent_handle{};
    }
  }

  [[nodiscard]] std::string locateSliceFile() const {
    if (!handle.is_valid()) {
      return {};
    }
    const std::shared_ptr<const lt::torrent_info> info = handle.torrent_file();
    if (!info) {
      return {};
    }

    const lt::file_storage &files = info->files();
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(files.num_files()));
    for (const auto i : files.file_range()) {
      paths.push_back(files.file_path(i));
    }

    const std::string relative =
        zzcore::selectSliceFile(paths, preferred_filename);
    if (relative.empty()) {
      return {};
    }
    return save_path + "/" + relative;
  }
};

PrefletFetcher::PrefletFetcher() : impl_(std::make_unique<Impl>()) {}
PrefletFetcher::~PrefletFetcher()                                     = default;
PrefletFetcher::PrefletFetcher(PrefletFetcher &&) noexcept            = default;
PrefletFetcher &PrefletFetcher::operator=(PrefletFetcher &&) noexcept = default;

std::string PrefletFetcher::cacheRoot() {
  return gleditor::paths::cacheDir("zigzag/slices");
}

bool PrefletFetcher::begin(const Preflet &preflet, std::string &error) {
  if (busy()) {
    error = "fetch already in progress";
    return false;
  }

  lt::error_code ec;
  lt::add_torrent_params params =
      lt::parse_magnet_uri(preflet.resource_identifier, ec);
  if (ec) {
    error = "invalid magnet URI: " + ec.message();
    return false;
  }

  const std::string root = cacheRoot();
  if (root.empty()) {
    error = "could not determine cache directory (XDG_CACHE_HOME/HOME unset)";
    return false;
  }

  // Identify infohash for cache directory
  std::string hashHex;
  try {
    const auto magnet = xudu::MagnetLink::parse(preflet.resource_identifier);
    hashHex           = magnet.hash.hex();
  } catch (const std::exception &) {
    if (params.info_hashes.has_v1()) {
      hashHex = gleditor::color::toHex(std::string_view{
          reinterpret_cast<const char *>(params.info_hashes.v1.data()), 20});
    } else {
      hashHex = "unknown_hash";
    }
  }

  const std::string savePath = root + "/" + hashHex;
  std::error_code fsEc;
  fs::create_directories(savePath, fsEc);
  if (fsEc) {
    error = "could not create cache directory " + savePath;
    return false;
  }

  params.save_path = savePath;

  // Lazily create session
  if (!impl_->session) {
    lt::settings_pack pack;
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_int(lt::settings_pack::alert_mask,
                 lt::alert_category::status | lt::alert_category::error |
                     lt::alert_category::storage);
    impl_->session = std::make_unique<lt::session>(pack);
  }

  impl_->handle = impl_->session->add_torrent(params, ec);
  if (ec) {
    error = "could not start torrent: " + ec.message();
    return false;
  }

  impl_->expected_hash.clear();
  impl_->preferred_filename.clear();
  if (!preflet.hash.empty()) {
    impl_->expected_hash = toLowerHex(preflet.hash);
  }
  for (const auto &[key, val] : preflet.metadata) {
    if (key == "file") {
      impl_->preferred_filename = val;
    }
  }

  impl_->save_path         = savePath;
  impl_->metadata_received = false;
  impl_->started           = std::chrono::steady_clock::now();
  impl_->progress.status   = Status::Fetching;
  impl_->progress.message  = "resolving magnet metadata...";
  impl_->progress.fraction = 0.0F;
  impl_->progress.slice_path.clear();

  return true;
}

void PrefletFetcher::poll() {
  if (!impl_->session || !impl_->handle.is_valid() ||
      impl_->progress.status != Status::Fetching) {
    return;
  }

  std::vector<lt::alert *> alerts;
  impl_->session->pop_alerts(&alerts);

  for (const lt::alert *const a : alerts) {
    if (const auto *const ma = lt::alert_cast<lt::metadata_received_alert>(a)) {
      if (ma->handle == impl_->handle) {
        impl_->metadata_received = true;
        const auto info          = impl_->handle.torrent_file();
        if (info && info->total_size() > maxSliceDownloadBytes) {
          impl_->fail(std::format("slice torrent too large ({} MB > {} MB)",
                                  info->total_size() / (1024 * 1024),
                                  maxSliceDownloadBytes / (1024 * 1024)));
          return;
        }
      }
    } else if (const auto *const fa =
                   lt::alert_cast<lt::torrent_finished_alert>(a)) {
      if (fa->handle == impl_->handle) {
        const std::string slicePath = impl_->locateSliceFile();
        if (slicePath.empty()) {
          impl_->fail("torrent contains no .yaml or .yml Slice file");
          return;
        }

        if (!impl_->expected_hash.empty()) {
          bool ok                  = false;
          const std::string actual = sha1HexOfFile(slicePath, ok);
          if (!ok) {
            impl_->fail("could not read downloaded Slice to verify SHA-1");
            return;
          }
          if (toLowerHex(actual) != impl_->expected_hash) {
            impl_->fail(std::format("SHA-1 mismatch (expected {}, got {})",
                                    impl_->expected_hash, actual));
            return;
          }
        }

        impl_->progress.status     = Status::Ready;
        impl_->progress.message    = "ready";
        impl_->progress.fraction   = 1.0F;
        impl_->progress.slice_path = slicePath;
        return;
      }
    } else if (const auto *const ea =
                   lt::alert_cast<lt::torrent_error_alert>(a)) {
      if (ea->handle == impl_->handle) {
        impl_->fail(ea->error.message());
        return;
      }
    }
  }

  // Check timeout while waiting for metadata
  if (!impl_->metadata_received &&
      (std::chrono::steady_clock::now() - impl_->started) > metadataTimeout) {
    impl_->fail("timed out waiting for magnet metadata (unseeded link?)");
    return;
  }

  const lt::torrent_status st = impl_->handle.status();
  impl_->progress.fraction    = st.progress;
  if (!impl_->metadata_received) {
    impl_->progress.message =
        std::format("resolving magnet (peers: {})", st.num_peers);
  } else {
    impl_->progress.message = std::format("downloading {:.0f}% (peers: {})",
                                          st.progress * 100.0F, st.num_peers);
  }
}

void PrefletFetcher::cancel() {
  if (impl_->session && impl_->handle.is_valid()) {
    impl_->session->remove_torrent(impl_->handle);
    impl_->handle = lt::torrent_handle{};
  }
  impl_->progress = Progress{};
}

void PrefletFetcher::acknowledge() {
  if (impl_->progress.status != Status::Fetching) {
    impl_->progress = Progress{};
  }
}

const PrefletFetcher::Progress &PrefletFetcher::progress() const {
  return impl_->progress;
}

bool PrefletFetcher::busy() const {
  return impl_->progress.status == Status::Fetching;
}

} // namespace zigzag
