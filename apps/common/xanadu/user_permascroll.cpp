/**
 * @file user_permascroll.cpp
 * @brief Implementation of sovereign append-only permascroll.
 */
#include "user_permascroll.hpp" // IWYU pragma: associated

#include <lmdb.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "bencode.hpp"
#include "common/tsv.hpp"
#include "identity/pgp_verify.hpp"
#include "publication.hpp"

namespace xanadu {

namespace {

inline constexpr std::string_view kMasterFingerprint = "master_fingerprint";
inline constexpr std::string_view kDevicePublicKey   = "device_public_key";
inline constexpr std::string_view kDeviceName        = "device_name";
inline constexpr std::string_view kIssuedTimestamp   = "issued_timestamp";
inline constexpr std::string_view kGpgSignature      = "gpg_signature";

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

// One LMDB handle per path per process; closing overlapping handles would
// release the process locks of the other handle. Transactions also serialize
// distinct processes opening the same author's state.
std::optional<Scroll> permascrollState(UserPermascroll::Config &config,
                                       const Scroll *next = nullptr) {
  if (config.storageDir.empty()) return std::nullopt;
  static std::mutex mutex;
  const std::scoped_lock lock(mutex);
  const auto directory = config.storageDir / "publication-state";
  std::filesystem::create_directories(directory);
  const auto check = [](const int rc) {
    if (rc != MDB_SUCCESS)
      throw std::runtime_error("permascroll state: " +
                               std::string(mdb_strerror(rc)));
  };
  MDB_env *rawEnv = nullptr;
  check(mdb_env_create(&rawEnv));
  const std::unique_ptr<MDB_env, decltype(&mdb_env_close)> env(rawEnv,
                                                               mdb_env_close);
  check(mdb_env_open(env.get(), directory.string().c_str(), 0, 0600));
  MDB_txn *rawTxn = nullptr;
  check(mdb_txn_begin(env.get(), nullptr, 0, &rawTxn));
  std::unique_ptr<MDB_txn, decltype(&mdb_txn_abort)> txn(rawTxn, mdb_txn_abort);
  MDB_dbi db;
  check(mdb_dbi_open(txn.get(), nullptr, 0, &db));
  std::string name = "state";
  MDB_val key{name.size(), name.data()};
  MDB_val value{};
  const auto found = mdb_get(txn.get(), db, &key, &value);
  std::optional<Scroll> restored;
  if (found == MDB_SUCCESS) {
    const std::string_view bytes(static_cast<const char *>(value.mv_data),
                                 value.mv_size);
    if (bytes.size() < 4 || !bytes.starts_with("XUP"))
      throw PermascrollStateUnreadable(
          "permascroll state format 1: XUP signature missing");
    if (bytes[3] != '1')
      throw PermascrollStateUnreadable(
          "permascroll state version 1 expected (byte 49), got byte " +
          std::to_string(static_cast<unsigned char>(bytes[3])));
    try {
      const auto root   = bencode::decode(bytes.substr(4));
      const auto pub    = root.find("public");
      const auto secret = root.find("secret");
      const auto master = root.find("master");
      const auto device = root.find("device");
      const auto seal   = root.find("seal");
      if (!pub || !secret || !master || !device || !seal || !pub->isString() ||
          !secret->isString() || !master->isString() || !device->isString() ||
          !seal->isString())
        throw PermascrollStateUnreadable(
            "permascroll state format 1: missing fields");
      MutableKeys keys{PublicKey::fromHex(pub->asString()),
                       SecretKey::fromHex(secret->asString())};
      if (keys.publicKey.isZero() ||
          !verifyMutableItem("permascroll-state-v1",
                             signMutableItem("permascroll-state-v1", keys),
                             keys.publicKey))
        throw PermascrollStateUnreadable(
            "permascroll state format 1: invalid key pair");
      if (master->asString() != config.masterIdentity.toString() ||
          device->asString() != config.deviceId ||
          (!config.deviceKeys.publicKey.isZero() &&
           (config.deviceKeys.publicKey != keys.publicKey ||
            config.deviceKeys.secretKey.bytes != keys.secretKey.bytes)))
        throw PermascrollStateUnreadable(
            "permascroll state format 1: identity mismatch");
      const auto state = decodeSealState(seal->asString());
      const auto salt  = config.deviceId == "main"
                             ? "permascroll"
                             : "permascroll/" + config.deviceId;
      if (!state || state->opsAlreadySealed != 0 ||
          !state->opsSegments.empty() ||
          state->scroll.publisher != keys.publicKey ||
          state->scroll.salt != salt)
        throw PermascrollStateUnreadable(
            "permascroll state format 1: invalid scroll");
      std::uint64_t end = 0;
      for (const auto &segment : state->scroll.segments) {
        if (segment.at != end || segment.length == 0 || segment.end() < end)
          throw PermascrollStateUnreadable(
              "permascroll state format 1: noncontiguous segments");
        end = segment.end();
      }
      config.deviceKeys = keys;
      restored          = state->scroll;
    } catch (const PermascrollStateUnreadable &) {
      throw;
    } catch (const std::exception &) {
      // A parse error can contain source bytes; never expose private keys.
      throw PermascrollStateUnreadable(
          "permascroll state format 1: malformed record");
    }
  } else if (found != MDB_NOTFOUND) {
    check(found);
  }
  if (next) {
    SealState state;
    state.scroll = *next;
    auto bytes =
        "XUP1" +
        bencode::Value::dict(
            {
                {"public",
                 bencode::Value::string(config.deviceKeys.publicKey.hex())},
                {"secret",
                 bencode::Value::string(config.deviceKeys.secretKey.hex())},
                {"master",
                 bencode::Value::string(config.masterIdentity.toString())},
                {"device", bencode::Value::string(config.deviceId)},
                {"seal", bencode::Value::string(encodeSealState(state))},
            })
            .encode();
    value = MDB_val{bytes.size(), bytes.data()};
    check(mdb_put(txn.get(), db, &key, &value, 0));
    check(mdb_txn_commit(txn.release()));
  }
  return restored;
}

} // namespace

// -- DeviceDelegation --------------------------------------------------------

std::string DeviceDelegation::signingBuffer() const {
  // Length-prefixed rather than delimited: a device named "x\nmaster:..."
  // must not be able to spell out a different delegation.
  const auto field = [](const std::string_view label,
                        const std::string_view value) {
    return std::string(label) + ":" + std::to_string(value.size()) + ":" +
           std::string(value) + "\n";
  };

  std::string out = "xudu-device-delegation-v1\n";
  out += field("master", masterFingerprint.toString());
  out += field("device", devicePublicKey.hex());
  out += field("name", deviceName);
  out += field("issued", std::to_string(issuedTimestamp));
  return out;
}

bool DeviceDelegation::verify(
    const std::string_view masterPublicKeyArmored) const {
  if (!masterFingerprint.isValid() || devicePublicKey.isZero() ||
      gpgSignatureArmored.empty()) {
    return false;
  }
  // The key has to be the one this delegation names before its signature
  // means anything. Otherwise any valid key with any valid signature over
  // these bytes would do, which is the hole this whole layer exists to close.
  if (!identity::pgp::keyMatchesFingerprint(masterPublicKeyArmored,
                                            masterFingerprint)) {
    return false;
  }
  return identity::pgp::verifyDetached(masterPublicKeyArmored, signingBuffer(),
                                       gpgSignatureArmored);
}

std::string DeviceDelegation::toTsv() const {
  std::string out;
  common::tsv::write(out, kMasterFingerprint, masterFingerprint.toString());
  common::tsv::write(out, kDevicePublicKey, devicePublicKey.hex());
  common::tsv::write(out, kDeviceName, deviceName);
  common::tsv::write(out, kIssuedTimestamp, std::to_string(issuedTimestamp));
  common::tsv::write(out, kGpgSignature, gpgSignatureArmored);
  return out;
}

std::optional<DeviceDelegation>
DeviceDelegation::fromTsv(const std::string_view tsv) {
  const auto entries = common::tsv::read(tsv);
  if (!entries) {
    return std::nullopt;
  }

  DeviceDelegation cert;
  for (const auto &entry : *entries) {
    if (entry.key == kMasterFingerprint) {
      const auto fp = identity::Fingerprint::fromString(entry.value);
      if (!fp) {
        return std::nullopt;
      }
      cert.masterFingerprint = *fp;
    } else if (entry.key == kDevicePublicKey) {
      // fromTsv() answers nullopt for a malformed certificate; a key that is
      // not hex is one, not an exception.
      const auto key = PublicKey::parseHex(entry.value);
      if (!key) {
        return std::nullopt;
      }
      cert.devicePublicKey = *key;
    } else if (entry.key == kDeviceName) {
      cert.deviceName = entry.value;
    } else if (entry.key == kIssuedTimestamp) {
      try {
        cert.issuedTimestamp = std::stoull(entry.value);
      } catch (...) {
        return std::nullopt;
      }
    } else if (entry.key == kGpgSignature) {
      cert.gpgSignatureArmored = entry.value;
    }
  }

  return cert;
}

// -- UserPermascroll ---------------------------------------------------------

UserPermascroll::UserPermascroll() {
  config_.deviceKeys       = createMutableKeys();
  config_.deviceId         = "main";
  currentScroll_.publisher = config_.deviceKeys.publicKey;
  currentScroll_.salt      = "permascroll";
}

UserPermascroll::UserPermascroll(Config config) : config_(std::move(config)) {
  if (config_.deviceId.empty()) config_.deviceId = "main";
  const auto restored = permascrollState(config_);
  if (config_.deviceKeys.publicKey.isZero())
    config_.deviceKeys = createMutableKeys();

  currentScroll_.publisher = config_.deviceKeys.publicKey;
  currentScroll_.salt      = (config_.deviceId == "main")
                                 ? "permascroll"
                                 : "permascroll/" + config_.deviceId;

  if (!config_.storageDir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(config_.storageDir / "segments", ec);

    // Unconditionally, not only when the file is already there. This is what
    // binds the spool to its backing file in *both* directions -- it restores
    // what the file holds and it is what a later flush() writes through -- so
    // skipping it for a permascroll that does not exist yet left the first
    // session with nowhere to write, and the author's first document reopened
    // empty. openActiveSegment() creates the file.
    spool_.openActiveSegment(config_.storageDir / "active.primedia");
    if (restored) {
      if (restored->length() > spool_.size())
        throw PermascrollStateUnreadable(
            "permascroll state format 1: sealed bytes exceed active primedia");
      currentScroll_ = *restored;
      sealedBytes_   = restored->length();
    } else {
      (void)permascrollState(config_, &currentScroll_);
    }
  }
}

UserPermascroll::~UserPermascroll() {
  // A permascroll is the only copy of what its author typed, and nothing else
  // holds a reference by the time this runs. Losing the unflushed tail here
  // would lose the end of the session -- the part most likely to matter.
  if (!config_.storageDir.empty()) {
    spool_.flush();
  }
}

PrimediaSpan UserPermascroll::append(const std::string_view text) {
  std::scoped_lock lock(appendMutex_);
  return spool_.append(text);
}

// -- the lock-free read path --------------------------------------------------
//
// These four take no lock, which is what they always claimed to do and for two
// commits did not. What makes it sound is not optimism about how short the
// critical section was: it is that a permascroll is append-only over an arena
// whose base address never moves, so the only thing a reader has to agree with
// an appender about is *how much* has been published. That agreement is the one
// atomic in SegmentedPrimediaSpool -- release on the appending side, acquire
// here -- and a reader clamps to what it loaded, so it never looks at a byte an
// append has not finished writing.
//
// The lock was not protecting a race, it was serialising the render thread
// against typing: every glyph of every visible span went through the same mutex
// the keystroke path holds, on a permascroll shared by every open document.
//
// What still needs the mutex is anything that *re-addresses* the arena rather
// than extending it -- clear(), adopt(), opening or sealing a segment. No
// ordering on a size can make those safe against a concurrent reader, because
// the bytes themselves move. They are construction-time and test-time
// operations and they keep the lock below; a reader racing one is a bug in the
// caller, not something this class can absorb.

std::string UserPermascroll::read(const PrimediaSpan &span) const {
  return spool_.read(span);
}

std::string_view UserPermascroll::readView(const PrimediaSpan &span) const {
  return spool_.readView(span);
}

std::uint64_t UserPermascroll::size() const { return spool_.size(); }

std::string_view UserPermascroll::bytes() const { return spool_.bytes(); }

void UserPermascroll::adopt(const std::string_view data) {
  std::scoped_lock lock(appendMutex_);
  spool_.adopt(data);
}

void UserPermascroll::clear() {
  std::scoped_lock lock(appendMutex_);
  spool_.clear();
  sealedBytes_ = 0;
  currentScroll_.segments.clear();
  if (!config_.storageDir.empty()) {
    std::filesystem::resize_file(config_.storageDir / "active.primedia", 0);
    spool_.openActiveSegment(config_.storageDir / "active.primedia");
    (void)permascrollState(config_, &currentScroll_);
  }
}

Scroll UserPermascroll::currentScroll() const {
  std::scoped_lock lock(appendMutex_);
  return currentScroll_;
}

std::string UserPermascroll::globalScrollKey() const {
  std::scoped_lock lock(appendMutex_);
  return scrollKey(currentScroll_);
}

std::optional<ScrollSegment> UserPermascroll::sealIncremental(
    const std::filesystem::path &outputDir, const SignedProvenance &provenance,
    const std::vector<PublishedHoleRecord> &holes) {
  std::scoped_lock lock(appendMutex_);

  const auto allBytes = spool_.bytes();
  if (allBytes.size() <= sealedBytes_) {
    return std::nullopt; // Nothing new to seal
  }

  const auto unsealedSlice = allBytes.substr(sealedBytes_);
  if (unsealedSlice.empty()) {
    return std::nullopt;
  }

  auto wirePayload = publicationPrimedia(unsealedSlice, sealedBytes_, holes);

  std::vector<TorrentContent> files;
  files.push_back(
      TorrentContent{.path = sealedContentName, .data = wirePayload});
  if (!provenance.tsv.empty()) {
    files.push_back(
        TorrentContent{.path = provenanceFileName, .data = provenance.tsv});
  }
  if (!provenance.signature.empty()) {
    files.push_back(TorrentContent{.path = provenanceSigName,
                                   .data = provenance.signature});
  }

  auto made = makeTorrent(files, "permascroll");

  if (!outputDir.empty()) {
    (void)writeTorrentSeed(outputDir, made, files);
  }

  ScrollSegment segment;
  segment.at           = sealedBytes_;
  segment.length       = unsealedSlice.size();
  segment.torrent      = made.hash;
  segment.streamOffset = 0;
  segment.fileIndex    = 0;
  segment.path         = sealedContentName;

  auto next = currentScroll_;
  next.addSegment(segment);
  // Persist the bytes before committing a descriptor that promises they exist.
  if (!spool_.flush())
    throw std::runtime_error("cannot flush permascroll before sealing");
  (void)permascrollState(config_, &next);
  currentScroll_ = std::move(next);
  sealedBytes_ += unsealedSlice.size();

  return segment;
}

bool UserPermascroll::flush() {
  std::scoped_lock lock(appendMutex_);
  return spool_.flush();
}

bool UserPermascroll::refresh() {
  std::scoped_lock lock(appendMutex_);
  return spool_.refreshActiveSegment();
}

// -- PermascrollRegistry -----------------------------------------------------

PermascrollRegistry &PermascrollRegistry::instance() {
  static PermascrollRegistry reg;
  return reg;
}

std::shared_ptr<UserPermascroll>
PermascrollRegistry::getOrCreate(const identity::Fingerprint &fingerprint,
                                 const std::filesystem::path &customBaseDir) {
  std::scoped_lock lock(registryMutex_);
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

  auto scroll    = std::make_shared<UserPermascroll>(std::move(config));
  registry_[key] = scroll;
  return scroll;
}

std::shared_ptr<UserPermascroll> PermascrollRegistry::defaultUser() {
  std::scoped_lock lock(registryMutex_);
  if (!defaultUser_) {
    UserPermascroll::Config config;
    config.storageDir = resolveDefaultStorageDir("default");
    defaultUser_      = std::make_shared<UserPermascroll>(std::move(config));
  }
  return defaultUser_;
}

void PermascrollRegistry::clear() {
  std::scoped_lock lock(registryMutex_);
  registry_.clear();
  defaultUser_.reset();
}

} // namespace xanadu
