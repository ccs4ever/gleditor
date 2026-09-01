/**
 * @file merkle_ledger.cpp
 * @brief Implementation of the append-only Merkle ledger for GPG identity.
 */
#include "merkle_ledger.hpp"

#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

#include "provenance.hpp"
#include "torrent.hpp"
#include "yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace xudu {

namespace {

void sha256_lt(const merkle::HashT<32> &l, const merkle::HashT<32> &r,
               merkle::HashT<32> &out) {
  libtorrent::hasher256 h;
  h.update(reinterpret_cast<const char *>(l.bytes), 32);
  h.update(reinterpret_cast<const char *>(r.bytes), 32);
  const libtorrent::sha256_hash digest = h.final();
  std::memcpy(out.bytes, digest.data(), 32);
}

using LedgerTree = merkle::TreeT<32, sha256_lt>;
using LedgerPath = merkle::PathT<32, sha256_lt>;

std::string normalizeFingerprint(std::string_view fp) {
  std::string out;
  out.reserve(fp.size());
  for (const char c : fp) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      out.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

std::string normalizeEmail(std::string_view email) {
  std::string out;
  out.reserve(email.size());
  for (const char c : email) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      out.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return out;
}

} // namespace

std::array<std::uint8_t, 32> sha256Digest(std::string_view data) {
  libtorrent::hasher256 h;
  h.update(data.data(), static_cast<int>(data.size()));
  const libtorrent::sha256_hash digest = h.final();
  std::array<std::uint8_t, 32> out{};
  std::memcpy(out.data(), digest.data(), 32);
  return out;
}

std::string toHex32(const std::array<std::uint8_t, 32> &bytes) {
  static constexpr char hexChars[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const auto b : bytes) {
    out.push_back(hexChars[(b >> 4) & 0x0f]);
    out.push_back(hexChars[b & 0x0f]);
  }
  return out;
}

std::optional<std::array<std::uint8_t, 32>> fromHex32(std::string_view hex) {
  if (hex.size() != 64) {
    return std::nullopt;
  }
  std::array<std::uint8_t, 32> out{};
  for (std::size_t i = 0; i < 32; ++i) {
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (!merkle::decode_hex_digit(hex[2 * i], hi) ||
        !merkle::decode_hex_digit(hex[2 * i + 1], lo)) {
      return std::nullopt;
    }
    out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return out;
}

std::string GpgKeyLink::canonicalForm() const {
  std::string s;
  s.reserve(128 + publicKeyArmored.size() + signature.size());
  s += "seq:" + std::to_string(sequence) + "\n";
  s += "fp:" + normalizeFingerprint(fingerprint) + "\n";
  s += "email:" + normalizeEmail(email) + "\n";
  s += "name:" + identity + "\n";
  if (!gpgKeyId.empty()) {
    s += "key_id:" + gpgKeyId + "\n";
  }
  s += "time:" + std::to_string(timestamp) + "\n";
  s += "revoked:" + std::string(revoked ? "1" : "0") + "\n";
  if (!publicKeyArmored.empty()) {
    s += "pubkey:\n" + publicKeyArmored + "\n";
  }
  if (!signature.empty()) {
    s += "sig:\n" + signature + "\n";
  }
  return s;
}

std::array<std::uint8_t, 32> GpgKeyLink::leafHash() const {
  return sha256Digest(canonicalForm());
}

std::string GpgKeyLink::leafHashHex() const { return toHex32(leafHash()); }

bool MerkleProof::verify(
    const std::array<std::uint8_t, 32> &expectedRoot) const {
  std::array<std::uint8_t, 32> current = leafHash;
  for (const auto &elem : path) {
    merkle::HashT<32> l;
    merkle::HashT<32> r;
    merkle::HashT<32> out;
    if (elem.isLeft) {
      std::memcpy(l.bytes, elem.hash.data(), 32);
      std::memcpy(r.bytes, current.data(), 32);
    } else {
      std::memcpy(l.bytes, current.data(), 32);
      std::memcpy(r.bytes, elem.hash.data(), 32);
    }
    sha256_lt(l, r, out);
    std::memcpy(current.data(), out.bytes, 32);
  }
  return current == expectedRoot;
}

std::string MerkleProof::toYaml() const {
  std::string y;
  yaml::write(y, "leaf_index", std::to_string(leafIndex));
  yaml::write(y, "max_index", std::to_string(maxIndex));
  yaml::write(y, "leaf_hash", toHex32(leafHash));
  yaml::write(y, "root_hash", toHex32(rootHash));
  if (!path.empty()) {
    std::vector<std::string> pathItems;
    pathItems.reserve(path.size());
    for (const auto &el : path) {
      pathItems.push_back(std::string(el.isLeft ? "L:" : "R:") +
                          toHex32(el.hash));
    }
    yaml::writeList(y, "path", pathItems);
  }
  return y;
}

std::optional<MerkleProof> MerkleProof::fromYaml(std::string_view y) {
  const auto entries = yaml::read(y);
  if (!entries) {
    return std::nullopt;
  }
  MerkleProof proof;
  for (const auto &e : *entries) {
    if (!e.listItem) {
      if (e.key == "leaf_index") {
        proof.leafIndex = static_cast<std::size_t>(std::stoull(e.value));
      } else if (e.key == "max_index") {
        proof.maxIndex = static_cast<std::size_t>(std::stoull(e.value));
      } else if (e.key == "leaf_hash") {
        if (const auto h = fromHex32(e.value)) {
          proof.leafHash = *h;
        }
      } else if (e.key == "root_hash") {
        if (const auto h = fromHex32(e.value)) {
          proof.rootHash = *h;
        }
      }
    } else if (e.key == "path" && e.value.size() >= 66) {
      const bool isLeft = (e.value[0] == 'L');
      if (const auto h = fromHex32(std::string_view(e.value).substr(2))) {
        proof.path.push_back(Element{*h, isLeft});
      }
    }
  }
  return proof;
}

struct MerkleLedger::Impl {
  LedgerTree tree;
  std::vector<GpgKeyLink> entries;
  std::map<std::string, std::size_t> byFingerprint;
  std::map<std::string, std::vector<std::size_t>> byEmail;
};

MerkleLedger::MerkleLedger() : impl_(std::make_unique<Impl>()) {}
MerkleLedger::~MerkleLedger()                                   = default;
MerkleLedger::MerkleLedger(MerkleLedger &&) noexcept            = default;
MerkleLedger &MerkleLedger::operator=(MerkleLedger &&) noexcept = default;

MerkleLedger::MerkleLedger(const MerkleLedger &other)
    : impl_(std::make_unique<Impl>()) {
  for (const auto &entry : other.entries()) {
    appendKey(entry);
  }
}

MerkleLedger &MerkleLedger::operator=(const MerkleLedger &other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>();
    for (const auto &entry : other.entries()) {
      appendKey(entry);
    }
  }
  return *this;
}

std::pair<std::uint64_t, std::array<std::uint8_t, 32>>
MerkleLedger::appendKey(GpgKeyLink entry) {
  entry.fingerprint = normalizeFingerprint(entry.fingerprint);
  entry.email       = normalizeEmail(entry.email);
  entry.sequence    = impl_->entries.size();

  const auto leafDigest = entry.leafHash();
  const merkle::HashT<32> leaf(leafDigest.data());
  impl_->tree.insert(leaf);

  const std::size_t idx = impl_->entries.size();
  impl_->entries.push_back(entry);
  impl_->byFingerprint[entry.fingerprint] = idx;
  impl_->byEmail[entry.email].push_back(idx);

  return {entry.sequence, root()};
}

std::array<std::uint8_t, 32> MerkleLedger::root() const {
  std::array<std::uint8_t, 32> out{};
  if (impl_->entries.empty()) {
    return out;
  }
  const auto &r = impl_->tree.root();
  std::memcpy(out.data(), r.bytes, 32);
  return out;
}

std::string MerkleLedger::rootHex() const { return toHex32(root()); }

std::size_t MerkleLedger::size() const { return impl_->entries.size(); }

bool MerkleLedger::empty() const { return impl_->entries.empty(); }

const GpgKeyLink &MerkleLedger::entry(std::size_t index) const {
  if (index >= impl_->entries.size()) {
    throw std::out_of_range("MerkleLedger entry index out of range");
  }
  return impl_->entries[index];
}

const std::vector<GpgKeyLink> &MerkleLedger::entries() const {
  return impl_->entries;
}

MerkleProof MerkleLedger::generateProof(std::size_t index) const {
  if (index >= impl_->entries.size()) {
    throw std::out_of_range("MerkleLedger index out of range for proof");
  }

  const auto pathPtr = impl_->tree.path(index);
  if (!pathPtr) {
    throw std::runtime_error("Could not extract Merkle path from tree");
  }

  MerkleProof proof;
  proof.leafIndex = pathPtr->leaf_index();
  proof.maxIndex  = pathPtr->max_index();
  std::memcpy(proof.leafHash.data(), pathPtr->leaf().bytes, 32);
  proof.rootHash = root();

  for (const auto &elem : *pathPtr) {
    MerkleProof::Element el;
    std::memcpy(el.hash.data(), elem.hash.bytes, 32);
    el.isLeft = (elem.direction == LedgerPath::PATH_LEFT);
    proof.path.push_back(el);
  }

  return proof;
}

bool MerkleLedger::verifyInclusion(
    const GpgKeyLink &entry, const MerkleProof &proof,
    const std::array<std::uint8_t, 32> &expectedRoot) {
  if (entry.leafHash() != proof.leafHash) {
    return false;
  }
  return proof.verify(expectedRoot);
}

const GpgKeyLink *
MerkleLedger::findByFingerprint(std::string_view fingerprint) const {
  const std::string norm = normalizeFingerprint(fingerprint);
  const auto it          = impl_->byFingerprint.find(norm);
  if (it == impl_->byFingerprint.end()) {
    return nullptr;
  }
  return &impl_->entries[it->second];
}

std::vector<const GpgKeyLink *>
MerkleLedger::findByEmail(std::string_view email) const {
  const std::string norm = normalizeEmail(email);
  std::vector<const GpgKeyLink *> result;
  const auto it = impl_->byEmail.find(norm);
  if (it != impl_->byEmail.end()) {
    for (const auto idx : it->second) {
      result.push_back(&impl_->entries[idx]);
    }
  }
  return result;
}

std::string MerkleLedger::toYaml() const {
  std::string y;
  yaml::write(y, "version", "1");
  yaml::write(y, "root", rootHex());
  yaml::write(y, "count", std::to_string(size()));

  for (const auto &e : impl_->entries) {
    y += "\n---\n";
    yaml::write(y, "sequence", std::to_string(e.sequence));
    yaml::write(y, "fingerprint", e.fingerprint);
    yaml::write(y, "email", e.email);
    yaml::write(y, "identity", e.identity);
    if (!e.gpgKeyId.empty()) {
      yaml::write(y, "key_id", e.gpgKeyId);
    }
    yaml::write(y, "timestamp", std::to_string(e.timestamp));
    yaml::write(y, "revoked", e.revoked ? "true" : "false");
    if (!e.publicKeyArmored.empty()) {
      yaml::write(y, "public_key", e.publicKeyArmored);
    }
    if (!e.signature.empty()) {
      yaml::write(y, "signature", e.signature);
    }
  }
  return y;
}

MerkleLedger MerkleLedger::fromYaml(std::string_view yamlText) {
  MerkleLedger ledger;
  // Split on document boundary "\n---\n" or "\n---"
  std::size_t pos = 0;
  while (pos < yamlText.size()) {
    std::size_t next = yamlText.find("\n---", pos);
    if (next == std::string_view::npos) {
      next = yamlText.size();
    }
    std::string_view chunk = yamlText.substr(pos, next - pos);
    pos                    = (next < yamlText.size()) ? next + 4 : next;
    if (pos < yamlText.size() && yamlText[pos] == '\n') {
      pos++;
    }

    const auto parsed = yaml::read(chunk);
    if (!parsed) {
      continue;
    }

    GpgKeyLink link;
    bool hasEntry = false;
    for (const auto &e : *parsed) {
      if (e.listItem) {
        continue;
      }
      if (e.key == "fingerprint") {
        link.fingerprint = e.value;
        hasEntry         = true;
      } else if (e.key == "email") {
        link.email = e.value;
        hasEntry   = true;
      } else if (e.key == "identity") {
        link.identity = e.value;
      } else if (e.key == "key_id") {
        link.gpgKeyId = e.value;
      } else if (e.key == "timestamp") {
        link.timestamp = std::stoull(e.value);
      } else if (e.key == "revoked") {
        link.revoked = (e.value == "true" || e.value == "1");
      } else if (e.key == "public_key") {
        link.publicKeyArmored = e.value;
      } else if (e.key == "signature") {
        link.signature = e.value;
      }
    }
    if (hasEntry) {
      ledger.appendKey(std::move(link));
    }
  }
  return ledger;
}

bool MerkleLedger::saveToFile(const std::string &path) const {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  const std::string y = toYaml();
  out.write(y.data(), static_cast<std::streamsize>(y.size()));
  return out.good();
}

std::optional<MerkleLedger>
MerkleLedger::loadFromFile(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return fromYaml(ss.str());
}

MadeTorrent MerkleLedger::sealToTorrent(std::string_view name,
                                        std::uint64_t pieceLength) const {
  std::vector<TorrentContent> files;
  const std::string ledgerYaml = toYaml();
  const std::string rootHexStr = rootHex() + "\n";

  std::string keysConcat;
  for (const auto &e : impl_->entries) {
    if (!e.publicKeyArmored.empty()) {
      keysConcat += e.publicKeyArmored + "\n";
    }
  }

  files.push_back(TorrentContent{"LEDGER.yaml", ledgerYaml});
  files.push_back(TorrentContent{"ROOT.hex", rootHexStr});
  if (!keysConcat.empty()) {
    files.push_back(TorrentContent{"KEYS.pub", keysConcat});
  }

  return makeTorrent(files, std::string(name), pieceLength);
}

bool MerkleLedger::verifyProvenanceAuthor(
    const Author &author, const SignedProvenance &prov,
    const std::array<std::uint8_t, 32> &expectedRoot,
    std::string *errorMsg) const {
  if (!author.named()) {
    if (errorMsg) {
      *errorMsg = "Author record is missing name or email";
    }
    return false;
  }

  const auto links = findByEmail(author.email);
  if (links.empty()) {
    if (errorMsg) {
      *errorMsg = "No verified ledger entry found for email: " + author.email;
    }
    return false;
  }

  const GpgKeyLink *matched = nullptr;
  const std::string wantedFp =
      !author.gpgKey.empty() ? normalizeFingerprint(author.gpgKey) : "";

  for (const auto *link : links) {
    if (link->revoked) {
      continue;
    }
    if (!wantedFp.empty()) {
      if (link->fingerprint == wantedFp ||
          link->fingerprint.ends_with(wantedFp) ||
          link->gpgKeyId == author.gpgKey) {
        matched = link;
        break;
      }
    } else {
      matched = link;
      break;
    }
  }

  if (!matched) {
    if (errorMsg) {
      *errorMsg = "No active non-revoked key matches author key specification";
    }
    return false;
  }

  // Verify inclusion proof
  const MerkleProof proof = generateProof(matched->sequence);
  if (!verifyInclusion(*matched, proof, expectedRoot)) {
    if (errorMsg) {
      *errorMsg = "Merkle inclusion proof failed against expected ledger root";
    }
    return false;
  }

  // If GnuPG is available and a signature was provided, check the signature
  if (!prov.signature.empty() && gpgAvailable()) {
    const auto check = verifyProvenance(prov);
    if (!check.signatureValid) {
      if (errorMsg) {
        *errorMsg = "OpenPGP signature verification failed: " + check.detail;
      }
      return false;
    }
    if (!check.fingerprint.empty()) {
      const std::string sigFp = normalizeFingerprint(check.fingerprint);
      if (sigFp != matched->fingerprint &&
          !matched->fingerprint.ends_with(sigFp)) {
        if (errorMsg) {
          *errorMsg = "Signer fingerprint mismatch: expected " +
                      matched->fingerprint + " but signature was from " + sigFp;
        }
        return false;
      }
    }
  }

  return true;
}

} // namespace xudu
