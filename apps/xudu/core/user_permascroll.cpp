/**
 * @file user_permascroll.cpp
 * @brief Implementation of sovereign append-only permascroll.
 */
#include "user_permascroll.hpp" // IWYU pragma: associated

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "publication.hpp"
#include "yaml.hpp"

namespace xudu {

namespace {

inline constexpr std::string_view kYamlMasterFingerprint = "master_fingerprint";
inline constexpr std::string_view kYamlDevicePublicKey   = "device_public_key";
inline constexpr std::string_view kYamlDeviceName        = "device_name";
inline constexpr std::string_view kYamlIssuedTimestamp   = "issued_timestamp";
inline constexpr std::string_view kYamlGpgSignature      = "gpg_signature";

std::filesystem::path resolveDefaultStorageDir(std::string_view subDir) {
  const char *xdgData = std::getenv("XDG_DATA_HOME");
  std::filesystem::path base;
  if (xdgData && *xdgData) {
    base = std::filesystem::path(xdgData) / "xudu" / "permascroll";
  } else {
    const char *home = std::getenv("HOME");
    if (home && *home) {
      base = std::filesystem::path(home) / ".local" / "share" / "xudu" /
             "permascroll";
    } else {
      base = std::filesystem::temp_directory_path() / "xudu" / "permascroll";
    }
  }
  return base / subDir;
}

} // namespace

// -- DeviceDelegation --------------------------------------------------------

bool DeviceDelegation::verify() const {
  return masterFingerprint.isValid() && !devicePublicKey.isZero() &&
         !gpgSignatureArmored.empty();
}

std::string DeviceDelegation::toYaml() const {
  std::string out;
  yaml::write(out, kYamlMasterFingerprint, masterFingerprint.toString());
  yaml::write(out, kYamlDevicePublicKey, devicePublicKey.hex());
  yaml::write(out, kYamlDeviceName, deviceName);
  yaml::write(out, kYamlIssuedTimestamp, std::to_string(issuedTimestamp));
  yaml::write(out, kYamlGpgSignature, gpgSignatureArmored);
  return out;
}

std::optional<DeviceDelegation>
DeviceDelegation::fromYaml(const std::string_view yamlText) {
  const auto entries = yaml::read(yamlText);
  if (!entries) {
    return std::nullopt;
  }

  DeviceDelegation cert;
  for (const auto &entry : *entries) {
    if (entry.key == kYamlMasterFingerprint) {
      const auto fp = identity::Fingerprint::fromString(entry.value);
      if (!fp) {
        return std::nullopt;
      }
      cert.masterFingerprint = *fp;
    } else if (entry.key == kYamlDevicePublicKey) {
      cert.devicePublicKey = PublicKey::fromHex(entry.value);
    } else if (entry.key == kYamlDeviceName) {
      cert.deviceName = entry.value;
    } else if (entry.key == kYamlIssuedTimestamp) {
      try {
        cert.issuedTimestamp = std::stoull(entry.value);
      } catch (...) {
        return std::nullopt;
      }
    } else if (entry.key == kYamlGpgSignature) {
      cert.gpgSignatureArmored = entry.value;
    }
  }

  return cert;
}

// -- UserPermascroll ---------------------------------------------------------

UserPermascroll::UserPermascroll() {
  config_.deviceKeys = createMutableKeys();
  config_.deviceId   = "main";
  currentScroll_.publisher = config_.deviceKeys.publicKey;
  currentScroll_.salt      = "permascroll";
}

UserPermascroll::UserPermascroll(Config config) : config_(std::move(config)) {
  if (config_.deviceKeys.publicKey.isZero()) {
    config_.deviceKeys = createMutableKeys();
  }
  if (config_.deviceId.empty()) {
    config_.deviceId = "main";
  }

  currentScroll_.publisher = config_.deviceKeys.publicKey;
  currentScroll_.salt =
      (config_.deviceId == "main") ? "permascroll"
                                   : "permascroll/" + config_.deviceId;

  if (!config_.storageDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(config_.storageDir / "segments", ec);

    const auto activePath = config_.storageDir / "active.primedia";
    if (std::filesystem::exists(activePath, ec)) {
      spool_.openActiveSegment(activePath);
    }
  }
}

UserPermascroll::~UserPermascroll() = default;

PrimediaSpan UserPermascroll::append(const std::string_view text) {
  std::lock_guard lock(appendMutex_);
  return spool_.append(text);
}

std::string UserPermascroll::read(const PrimediaSpan &span) const {
  std::lock_guard lock(appendMutex_);
  return spool_.read(span);
}

std::string_view UserPermascroll::readView(const PrimediaSpan &span) const {
  std::lock_guard lock(appendMutex_);
  return spool_.readView(span);
}

std::uint64_t UserPermascroll::size() const {
  std::lock_guard lock(appendMutex_);
  return spool_.size();
}

std::string_view UserPermascroll::bytes() const {
  std::lock_guard lock(appendMutex_);
  return spool_.bytes();
}

void UserPermascroll::adopt(const std::string_view data) {
  std::lock_guard lock(appendMutex_);
  spool_.adopt(data);
}

void UserPermascroll::clear() {
  std::lock_guard lock(appendMutex_);
  spool_.clear();
  sealedBytes_ = 0;
  currentScroll_.segments.clear();
}

Scroll UserPermascroll::currentScroll() const {
  std::lock_guard lock(appendMutex_);
  return currentScroll_;
}

std::string UserPermascroll::globalScrollKey() const {
  std::lock_guard lock(appendMutex_);
  return scrollKey(currentScroll_);
}

std::optional<ScrollSegment>
UserPermascroll::sealIncremental(const std::filesystem::path &outputDir,
                                 const SignedProvenance &provenance) {
  std::lock_guard lock(appendMutex_);

  const auto allBytes = spool_.bytes();
  if (allBytes.size() <= sealedBytes_) {
    return std::nullopt; // Nothing new to seal
  }

  const auto unsealedSlice = allBytes.substr(sealedBytes_);
  if (unsealedSlice.empty()) {
    return std::nullopt;
  }

  std::vector<TorrentContent> files;
  files.push_back(
      TorrentContent{sealedContentName, std::string{unsealedSlice}});
  if (!provenance.yaml.empty()) {
    files.push_back(TorrentContent{provenanceFileName, provenance.yaml});
  }
  if (!provenance.signature.empty()) {
    files.push_back(TorrentContent{provenanceSigName, provenance.signature});
  }

  auto made = makeTorrent(files, currentScroll_.salt);

  if (!outputDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(outputDir, ec);
    const auto torrentPath = outputDir / (made.hash.hex() + ".torrent");
    std::ofstream out(torrentPath, std::ios::binary);
    if (out.is_open()) {
      out.write(made.file.data(), static_cast<std::streamsize>(made.file.size()));
    }
  }

  ScrollSegment segment;
  segment.at           = sealedBytes_;
  segment.length       = unsealedSlice.size();
  segment.torrent      = made.hash;
  segment.streamOffset = 0;
  segment.fileIndex    = 0;
  segment.path         = sealedContentName;

  currentScroll_.addSegment(segment);
  sealedBytes_ += unsealedSlice.size();

  return segment;
}

bool UserPermascroll::flush() {
  std::lock_guard lock(appendMutex_);
  return spool_.flush();
}

// -- PermascrollRegistry -----------------------------------------------------

PermascrollRegistry &PermascrollRegistry::instance() {
  static PermascrollRegistry reg;
  return reg;
}

std::shared_ptr<UserPermascroll>
PermascrollRegistry::getOrCreate(const identity::Fingerprint &fingerprint,
                                 const std::filesystem::path &customBaseDir) {
  std::lock_guard lock(registryMutex_);
  const auto key = fingerprint.toString();
  auto it        = registry_.find(key);
  if (it != registry_.end()) {
    return it->second;
  }

  UserPermascroll::Config config;
  config.masterIdentity = fingerprint;
  if (!customBaseDir.empty()) {
    config.storageDir = customBaseDir / key;
  } else {
    config.storageDir = resolveDefaultStorageDir(key);
  }

  auto scroll     = std::make_shared<UserPermascroll>(std::move(config));
  registry_[key] = scroll;
  return scroll;
}

std::shared_ptr<UserPermascroll> PermascrollRegistry::defaultUser() {
  std::lock_guard lock(registryMutex_);
  if (!defaultUser_) {
    UserPermascroll::Config config;
    config.storageDir = resolveDefaultStorageDir("default");
    defaultUser_      = std::make_shared<UserPermascroll>(std::move(config));
  }
  return defaultUser_;
}

void PermascrollRegistry::clear() {
  std::lock_guard lock(registryMutex_);
  registry_.clear();
  defaultUser_.reset();
}

} // namespace xudu
