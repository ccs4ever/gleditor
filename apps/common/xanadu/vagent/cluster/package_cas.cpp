/**
 * @file package_cas.cpp
 * @brief Implementation of Content-Addressed Store (CAS) for cluster packages.
 */
#include "common/xanadu/vagent/cluster/package_cas.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <random>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#elif defined(__APPLE__)
#include <sys/clonefile.h>
#endif

#include "common/xanadu/merkle_ledger.hpp"
#include <libtorrent/hasher.hpp>

namespace xanadu::vagent::cluster {

namespace fs = std::filesystem;

namespace {

std::string generatePartFileName() {
  static thread_local std::mt19937_64 rng{
      static_cast<std::uint64_t>(
          std::chrono::steady_clock::now().time_since_epoch().count()) ^
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&rng))};
  std::uniform_int_distribution<std::uint64_t> dist;
  const std::uint64_t v1 = dist(rng);
  const std::uint64_t v2 = dist(rng);
  char buf[33];
  std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                static_cast<unsigned long long>(v1),
                static_cast<unsigned long long>(v2));
  return std::string(buf) + ".part";
}

bool copyStream(int srcFd, int dstFd) {
  constexpr std::size_t kBufSize = 65536;
  char buf[kBufSize];
  while (true) {
    const ssize_t n = ::read(srcFd, buf, kBufSize);
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    ssize_t written = 0;
    while (written < n) {
      const ssize_t w =
          ::write(dstFd, buf + written, static_cast<std::size_t>(n - written));
      if (w < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      written += w;
    }
  }
  return true;
}

} // namespace

std::string_view toString(CasError error) noexcept {
  switch (error) {
  case CasError::Ok:
    return "Ok";
  case CasError::IoError:
    return "IoError";
  case CasError::HashMismatch:
    return "HashMismatch";
  case CasError::SizeMismatch:
    return "SizeMismatch";
  case CasError::QuotaExceeded:
    return "QuotaExceeded";
  case CasError::NotFound:
    return "NotFound";
  case CasError::AlreadyPinned:
    return "AlreadyPinned";
  case CasError::NotPinned:
    return "NotPinned";
  case CasError::InvalidName:
    return "InvalidName";
  case CasError::DestinationExists:
    return "DestinationExists";
  }
  return "UnknownError";
}

std::filesystem::path PackageCas::defaultCasDir() {
  const char *xdg = std::getenv("XDG_DATA_HOME");
  if (xdg != nullptr && xdg[0] != '\0') {
    return fs::path(xdg) / "vagent" / "packages" / "cas";
  }
  const char *home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return fs::path(home) / ".local" / "share" / "vagent" / "packages" / "cas";
  }
  return fs::temp_directory_path() / "vagent" / "packages" / "cas";
}

PackageCas::PackageCas(CasOptions options) : options_(std::move(options)) {
  if (options_.casDir.empty()) {
    options_.casDir = defaultCasDir();
  }
  tmpDir_ = options_.casDir / "tmp";

  std::error_code ec;
  fs::create_directories(options_.casDir, ec);
  fs::create_directories(tmpDir_, ec);

  sweepTmp();
  refresh();
}

bool PackageCas::isValidSha256Name(std::string_view name) const noexcept {
  if (name.size() != 64) return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::size_t PackageCas::sweepTmp() {
  std::size_t removedCount = 0;
  std::error_code ec;
  if (!fs::exists(tmpDir_, ec)) return 0;

  for (const auto &entry : fs::directory_iterator(tmpDir_, ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".part") {
      if (fs::remove(entry.path(), ec)) {
        ++removedCount;
      }
    }
  }
  return removedCount;
}

bool PackageCas::hasBlob(std::string_view sha256) const {
  if (!isValidSha256Name(sha256)) return false;
  return blobs_.find(std::string(sha256)) != blobs_.end();
}

std::optional<std::filesystem::path>
PackageCas::blobPath(std::string_view sha256) const {
  if (!hasBlob(sha256)) return std::nullopt;
  return options_.casDir / sha256;
}

void PackageCas::refresh() {
  blobs_.clear();
  totalBytes_ = 0;

  std::error_code ec;
  if (!fs::exists(options_.casDir, ec)) return;

  for (const auto &entry : fs::directory_iterator(options_.casDir, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    const std::string name = entry.path().filename().string();
    if (!isValidSha256Name(name)) continue;

    const auto sz = entry.file_size(ec);
    if (ec) continue;

    const auto mtime = entry.last_write_time(ec);
    if (ec) continue;

    const std::uint32_t pin = pins_.contains(name) ? pins_[name] : 0;
    blobs_.emplace(name, CasBlobInfo{name, sz, mtime, pin});
    totalBytes_ += sz;
  }
}

std::uint64_t PackageCas::unpinnedBytes() const noexcept {
  std::uint64_t bytes = 0;
  for (const auto &[sha, info] : blobs_) {
    if (info.pinCount == 0) {
      bytes += info.size;
    }
  }
  return bytes;
}

std::expected<std::filesystem::path, CasError>
PackageCas::storeBlob(std::string_view expectedSha256,
                      std::span<const std::uint8_t> data) {
  if (!isValidSha256Name(expectedSha256)) {
    return std::unexpected(CasError::InvalidName);
  }

  // Hash validation
  const std::string computedSha = toHex32(sha256Digest(std::string_view(
      reinterpret_cast<const char *>(data.data()), data.size())));
  if (computedSha != expectedSha256) {
    return std::unexpected(CasError::HashMismatch);
  }

  const std::string shaKey(expectedSha256);
  const fs::path finalPath = options_.casDir / shaKey;

  // Check if already in CAS
  auto it = blobs_.find(shaKey);
  if (it != blobs_.end()) {
    std::error_code ec;
    fs::last_write_time(finalPath, fs::file_time_type::clock::now(), ec);
    if (!ec) {
      it->second.lastAccess = fs::file_time_type::clock::now();
    }
    return finalPath;
  }

  std::error_code ec;
  fs::create_directories(tmpDir_, ec);

  // 1. Write to tmp file with mode 0600 and O_EXCL
  const fs::path tmpPath = tmpDir_ / generatePartFileName();
  const int fd =
      ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) {
    return std::unexpected(CasError::IoError);
  }

  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t w = ::write(fd, data.data() + written, data.size() - written);
    if (w < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      fs::remove(tmpPath, ec);
      return std::unexpected(CasError::IoError);
    }
    written += static_cast<std::size_t>(w);
  }

  // 2. fsync() on temporary file
  if (::fsync(fd) != 0) {
    ::close(fd);
    fs::remove(tmpPath, ec);
    return std::unexpected(CasError::IoError);
  }
  ::close(fd);

  // 3. Atomically commit via rename
  if (::rename(tmpPath.c_str(), finalPath.c_str()) != 0) {
    fs::remove(tmpPath, ec);
    return std::unexpected(CasError::IoError);
  }

  // 4. Set file mode to 0400 (read-only)
  ::chmod(finalPath.c_str(), 0400);

  // 5. fsync() on parent directory
  const int dirFd = ::open(options_.casDir.c_str(), O_RDONLY | O_DIRECTORY);
  if (dirFd >= 0) {
    ::fsync(dirFd);
    ::close(dirFd);
  }

  // 6. Record metadata
  const std::uint64_t dataSize = data.size();
  const std::uint32_t pin      = pins_.contains(shaKey) ? pins_[shaKey] : 0;
  blobs_[shaKey] =
      CasBlobInfo{shaKey, dataSize, fs::file_time_type::clock::now(), pin};
  totalBytes_ += dataSize;

  // 7. Check quota and sweep LRU if unpinned bytes exceed maxBytes
  if (options_.maxBytes > 0 && totalBytes_ > options_.maxBytes) {
    const std::uint64_t target = static_cast<std::uint64_t>(
        options_.maxBytes * options_.lowWatermarkFraction);
    evictLru(target);
    if (totalBytes_ > options_.maxBytes) {
      fs::remove(finalPath, ec);
      blobs_.erase(shaKey);
      totalBytes_ -= dataSize;
      return std::unexpected(CasError::QuotaExceeded);
    }
  }

  return finalPath;
}

std::expected<std::filesystem::path, CasError>
PackageCas::storeBlob(std::string_view expectedSha256, std::string_view data) {
  return storeBlob(
      expectedSha256,
      std::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t *>(data.data()), data.size()));
}

std::expected<std::filesystem::path, CasError>
PackageCas::storeBlobFromFile(std::string_view expectedSha256,
                              const fs::path &sourcePath) {
  std::ifstream is(sourcePath, std::ios::binary);
  if (!is) return std::unexpected(CasError::IoError);

  std::string buffer((std::istreambuf_iterator<char>(is)),
                     std::istreambuf_iterator<char>());
  return storeBlob(expectedSha256, buffer);
}

std::expected<std::string, CasError>
PackageCas::readBlob(std::string_view sha256) const {
  if (!hasBlob(sha256)) {
    return std::unexpected(CasError::NotFound);
  }

  const fs::path p = options_.casDir / sha256;
  std::ifstream is(p, std::ios::binary);
  if (!is) return std::unexpected(CasError::IoError);

  std::string content((std::istreambuf_iterator<char>(is)),
                      std::istreambuf_iterator<char>());
  return content;
}

std::expected<void, CasError> PackageCas::pinBlob(std::string_view sha256) {
  const std::string shaKey(sha256);
  auto it = blobs_.find(shaKey);
  if (it == blobs_.end()) {
    return std::unexpected(CasError::NotFound);
  }

  ++pins_[shaKey];
  it->second.pinCount = pins_[shaKey];
  return {};
}

std::expected<void, CasError> PackageCas::unpinBlob(std::string_view sha256) {
  const std::string shaKey(sha256);
  auto itPin = pins_.find(shaKey);
  if (itPin == pins_.end() || itPin->second == 0) {
    return std::unexpected(CasError::NotPinned);
  }

  --itPin->second;
  auto itBlob = blobs_.find(shaKey);
  if (itBlob != blobs_.end()) {
    itBlob->second.pinCount = itPin->second;
  }
  if (itPin->second == 0) {
    pins_.erase(itPin);
  }
  return {};
}

bool PackageCas::isPinned(std::string_view sha256) const {
  auto it = pins_.find(std::string(sha256));
  return it != pins_.end() && it->second > 0;
}

std::uint32_t PackageCas::pinCount(std::string_view sha256) const {
  auto it = pins_.find(std::string(sha256));
  return it != pins_.end() ? it->second : 0;
}

std::expected<void, CasError>
PackageCas::stageBlob(std::string_view sha256, const fs::path &dstPath) const {
  if (!hasBlob(sha256)) {
    return std::unexpected(CasError::NotFound);
  }

  const fs::path srcPath = options_.casDir / sha256;

  std::error_code ec;
  fs::create_directories(dstPath.parent_path(), ec);
  if (fs::exists(dstPath, ec)) {
    fs::remove(dstPath, ec);
  }

  const int srcFd = ::open(srcPath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (srcFd < 0) {
    return std::unexpected(CasError::IoError);
  }

  int dstFd =
      ::open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0500);
  if (dstFd < 0) {
    ::close(srcFd);
    return std::unexpected(CasError::IoError);
  }

  bool cloned = false;
#ifdef __linux__
  if (::ioctl(dstFd, FICLONE, srcFd) == 0) {
    cloned = true;
  }
#elif defined(__APPLE__)
  // clonefile on macOS operates on path names
  ::close(dstFd);
  fs::remove(dstPath, ec);
  if (::clonefile(srcPath.c_str(), dstPath.c_str(), 0) == 0) {
    cloned = true;
  }
  dstFd =
      ::open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0500);
#endif

  if (!cloned) {
    if (dstFd < 0 || !copyStream(srcFd, dstFd)) {
      if (dstFd >= 0) ::close(dstFd);
      ::close(srcFd);
      fs::remove(dstPath, ec);
      return std::unexpected(CasError::IoError);
    }
  }

  if (dstFd >= 0) ::close(dstFd);
  ::close(srcFd);

  // Set permissions strictly to 0500 (read & execute only, no write
  // permissions)
  ::chmod(dstPath.c_str(), 0500);

  return {};
}

std::size_t PackageCas::evictLru(std::optional<std::uint64_t> targetBytes) {
  const std::uint64_t target = targetBytes.value_or(static_cast<std::uint64_t>(
      options_.maxBytes * options_.lowWatermarkFraction));

  if (totalBytes_ <= target) return 0;

  // Collect unpinned candidates sorted by last access ascending (oldest first)
  std::vector<CasBlobInfo> candidates;
  for (const auto &[sha, info] : blobs_) {
    if (info.pinCount == 0) {
      candidates.push_back(info);
    }
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const CasBlobInfo &a, const CasBlobInfo &b) {
              return a.lastAccess < b.lastAccess;
            });

  std::size_t evictedCount = 0;
  std::error_code ec;

  for (const auto &cand : candidates) {
    if (totalBytes_ <= target) break;

    const fs::path p = options_.casDir / cand.sha256;
    if (fs::remove(p, ec)) {
      totalBytes_ -= cand.size;
      blobs_.erase(cand.sha256);
      if (const auto it = sha256ToInfoHash_.find(cand.sha256);
          it != sha256ToInfoHash_.end()) {
        infoHashToSha256_.erase(it->second);
        sha256ToInfoHash_.erase(it);
      }
      ++evictedCount;
    }
  }

  return evictedCount;
}

void PackageCas::registerInfoHash(const xanadu::InfoHash &hash,
                                  std::string_view sha256) {
  const std::string s(sha256);
  infoHashToSha256_[hash] = s;
  sha256ToInfoHash_[s]    = hash;
}

std::optional<std::string>
PackageCas::sha256ForInfoHash(const xanadu::InfoHash &hash) const {
  const auto it = infoHashToSha256_.find(hash);
  if (it != infoHashToSha256_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::optional<xanadu::InfoHash>
PackageCas::infoHashForSha256(std::string_view sha256) const {
  const auto it = sha256ToInfoHash_.find(std::string(sha256));
  if (it != sha256ToInfoHash_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::expected<xanadu::MadeTorrent, CasError>
PackageCas::createTorrentForBlob(std::string_view sha256,
                                 std::string_view /* name */,
                                 std::uint64_t pieceLength) {
  if (!hasBlob(sha256)) {
    return std::unexpected(CasError::NotFound);
  }
  const auto data = readBlob(sha256);
  if (!data) {
    return std::unexpected(data.error());
  }
  // Single-file torrents stored in CAS must always use sha256 as the canonical
  // file name in info["name"] so that libtorrent seeds from <casDir>/<sha256>.
  const std::string torrentName = std::string(sha256);
  xanadu::MadeTorrent made =
      xanadu::makeTorrent(*data, torrentName, pieceLength);
  registerInfoHash(made.hash, sha256);
  return made;
}

} // namespace xanadu::vagent::cluster
