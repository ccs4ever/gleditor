/**
 * @file merkle_ledger.cpp
 * @brief Implementation of the append-only Merkle ledger for GPG identity.
 */
#include "merkle_ledger.hpp"

#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

#include "common/tsv.hpp"
#include "provenance.hpp"
#include "torrent.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

#include "merkle_domain.hpp"

namespace xanadu {

namespace {

constexpr char kLeafDomain     = kMerkleLeafDomain;
constexpr char kInteriorDomain = kMerkleInteriorDomain;

void sha256_lt(const merkle::HashT<32> &l, const merkle::HashT<32> &r,
               merkle::HashT<32> &out) {
  libtorrent::hasher256 h;
  h.update(&kInteriorDomain, 1);
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
  // Tagged, to keep this out of the domain sha256_lt hashes interior nodes
  // into. sha256Digest stays an untagged SHA-256 because its other callers
  // want a plain digest rather than a tree leaf.
  return sha256Digest(kLeafDomain + canonicalForm());
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

std::string MerkleProof::toTsv() const {
  std::string out;
  common::tsv::write(out, "leaf_index", std::to_string(leafIndex));
  common::tsv::write(out, "max_index", std::to_string(maxIndex));
  common::tsv::write(out, "leaf_hash", toHex32(leafHash));
  common::tsv::write(out, "root_hash", toHex32(rootHash));
  for (const auto &element : path) {
    common::tsv::write(out, "path",
                       std::string(element.isLeft ? "L:" : "R:") +
                           toHex32(element.hash));
  }
  return out;
}

std::optional<MerkleProof> MerkleProof::fromTsv(const std::string_view tsv) {
  const auto entries = common::tsv::read(tsv);
  if (!entries) {
    return std::nullopt;
  }
  MerkleProof proof;
  for (const auto &e : *entries) {
    if (e.key == "leaf_index") {
      proof.leafIndex = static_cast<std::size_t>(std::stoull(e.value));
    } else if (e.key == "max_index") {
      proof.maxIndex = static_cast<std::size_t>(std::stoull(e.value));
    } else if (e.key == "leaf_hash") {
      if (const auto hash = fromHex32(e.value)) {
        proof.leafHash = *hash;
      }
    } else if (e.key == "root_hash") {
      if (const auto hash = fromHex32(e.value)) {
        proof.rootHash = *hash;
      }
    } else if (e.key == "path" && e.value.size() >= 66) {
      const bool isLeft = (e.value[0] == 'L');
      if (const auto hash = fromHex32(std::string_view(e.value).substr(2))) {
        proof.path.push_back(Element{.hash = *hash, .isLeft = isLeft});
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

std::string MerkleLedger::toTsv() const {
  std::string out;
  common::tsv::write(out, "version", "1");
  common::tsv::write(out, "root", rootHex());
  common::tsv::write(out, "count", std::to_string(size()));

  for (const auto &e : impl_->entries) {
    common::tsv::write(out, "entry", std::to_string(e.sequence));
    common::tsv::write(out, "fingerprint", e.fingerprint);
    common::tsv::write(out, "email", e.email);
    common::tsv::write(out, "identity", e.identity);
    common::tsv::write(out, "key_id", e.gpgKeyId);
    common::tsv::write(out, "timestamp", std::to_string(e.timestamp));
    common::tsv::write(out, "revoked", e.revoked ? "true" : "false");
    common::tsv::write(out, "public_key", e.publicKeyArmored);
    common::tsv::write(out, "signature", e.signature);
  }
  return out;
}

MerkleLedger MerkleLedger::fromTsv(const std::string_view tsv) {
  MerkleLedger ledger;
  const auto entries = common::tsv::read(tsv);
  if (!entries) {
    return ledger;
  }
  GpgKeyLink link;
  bool hasEntry     = false;
  const auto append = [&] {
    if (hasEntry) {
      ledger.appendKey(std::move(link));
      link     = GpgKeyLink{};
      hasEntry = false;
    }
  };
  for (const auto &entry : *entries) {
    if ("entry" == entry.key) {
      append();
      hasEntry = true;
    } else if (!hasEntry) {
      continue;
    } else if ("fingerprint" == entry.key) {
      link.fingerprint = entry.value;
    } else if ("email" == entry.key) {
      link.email = entry.value;
    } else if ("identity" == entry.key) {
      link.identity = entry.value;
    } else if ("key_id" == entry.key) {
      link.gpgKeyId = entry.value;
    } else if ("timestamp" == entry.key) {
      link.timestamp = std::stoull(entry.value);
    } else if ("revoked" == entry.key) {
      link.revoked = "true" == entry.value;
    } else if ("public_key" == entry.key) {
      link.publicKeyArmored = entry.value;
    } else if ("signature" == entry.key) {
      link.signature = entry.value;
    }
  }
  append();
  return ledger;
}

bool MerkleLedger::saveToFile(const std::string &path) const {
  std::ofstream out(path, std::ios::binary);
  if (!out.is_open()) {
    return false;
  }
  const std::string tsv = toTsv();
  out.write(tsv.data(), static_cast<std::streamsize>(tsv.size()));
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
  return fromTsv(ss.str());
}

MadeTorrent MerkleLedger::sealToTorrent(std::string_view name,
                                        std::uint64_t pieceLength) const {
  std::vector<TorrentContent> files;
  const std::string ledgerTsv  = toTsv();
  const std::string rootHexStr = rootHex() + "\n";

  std::string keysConcat;
  for (const auto &e : impl_->entries) {
    if (!e.publicKeyArmored.empty()) {
      keysConcat += e.publicKeyArmored + "\n";
    }
  }

  files.push_back(TorrentContent{.path = "LEDGER.tsv", .data = ledgerTsv});
  files.push_back(TorrentContent{.path = "ROOT.hex", .data = rootHexStr});
  if (!keysConcat.empty()) {
    files.push_back(TorrentContent{.path = "KEYS.pub", .data = keysConcat});
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

} // namespace xanadu
