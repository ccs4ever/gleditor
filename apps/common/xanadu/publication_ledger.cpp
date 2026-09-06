/**
 * @file publication_ledger.cpp
 * @brief Implementation of the append-only publication Merkle ledger.
 */
#include "publication_ledger.hpp"

#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

#include "yaml.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace xanadu {

namespace {

constexpr char kLeafDomain     = '\x00';
constexpr char kInteriorDomain = '\x01';

void sha256_pub(const merkle::HashT<32> &l, const merkle::HashT<32> &r,
                merkle::HashT<32> &out) {
  libtorrent::hasher256 h;
  h.update(&kInteriorDomain, 1);
  h.update(reinterpret_cast<const char *>(l.bytes), 32);
  h.update(reinterpret_cast<const char *>(r.bytes), 32);
  const libtorrent::sha256_hash digest = h.final();
  std::memcpy(out.bytes, digest.data(), 32);
}

using PubTree = merkle::TreeT<32, sha256_pub>;
using PubPath = merkle::PathT<32, sha256_pub>;

std::string normalizeTopicTag(std::string_view tag) {
  std::string out;
  out.reserve(tag.size());
  for (const char c : tag) {
    if (c == '#' || std::isspace(static_cast<unsigned char>(c))) {
      continue;
    }
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

} // namespace

std::string PublicationEntry::canonicalForm() const {
  std::ostringstream ss;
  ss << "pub:1\n"
     << "infoHash:" << infoHash << "\n"
     << "bep46Uri:" << bep46Uri << "\n"
     << "title:" << title << "\n"
     << "authorName:" << authorName << "\n"
     << "authorFingerprint:" << authorFingerprint << "\n"
     << "abstract:" << abstractText << "\n"
     << "timestamp:" << timestamp << "\n"
     << "sequence:" << sequence << "\n"
     << "totalBytes:" << totalBytes << "\n"
     << "microversions:" << microversions << "\n"
     << "hasTranscopyright:" << (hasTranscopyright ? "1" : "0") << "\n"
     << "transcopyrightTerms:" << transcopyrightTerms << "\n"
     << "merkleRoot:" << toHex32(merkleRoot) << "\n"
     << "signature:" << signature << "\n"
     << "topics:";
  for (const auto &t : topics) {
    ss << normalizeTopicTag(t) << ",";
  }
  ss << "\n";
  return ss.str();
}

std::array<std::uint8_t, 32> PublicationEntry::leafHash() const {
  const auto canon = canonicalForm();
  libtorrent::hasher256 h;
  h.update(&kLeafDomain, 1);
  h.update(canon.data(), static_cast<int>(canon.size()));
  const libtorrent::sha256_hash digest = h.final();
  std::array<std::uint8_t, 32> out{};
  std::memcpy(out.data(), digest.data(), 32);
  return out;
}

std::string PublicationEntry::leafHashHex() const {
  return toHex32(leafHash());
}

struct PublicationLedger::Impl {
  PubTree tree;
  std::vector<PublicationEntry> entries;
  std::map<std::string, std::size_t> infoHashMap;
  std::map<std::string, std::vector<std::size_t>> topicMap;
  std::map<std::string, std::vector<std::size_t>> authorMap;

  void rebuildIndices() {
    infoHashMap.clear();
    topicMap.clear();
    authorMap.clear();
    tree = PubTree{};

    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto &e = entries[i];
      if (!e.infoHash.empty()) {
        infoHashMap[e.infoHash] = i;
      }
      if (!e.authorFingerprint.empty()) {
        authorMap[e.authorFingerprint].push_back(i);
      }
      for (const auto &t : e.topics) {
        const auto norm = normalizeTopicTag(t);
        if (!norm.empty()) {
          topicMap[norm].push_back(i);
        }
      }

      const auto h = e.leafHash();
      merkle::HashT<32> mh;
      std::memcpy(mh.bytes, h.data(), 32);
      tree.insert(mh);
    }
  }
};

PublicationLedger::PublicationLedger() : impl_(std::make_unique<Impl>()) {}

PublicationLedger::~PublicationLedger() = default;

PublicationLedger::PublicationLedger(const PublicationLedger &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

PublicationLedger &
PublicationLedger::operator=(const PublicationLedger &other) {
  if (this != &other) {
    impl_ = std::make_unique<Impl>(*other.impl_);
  }
  return *this;
}

PublicationLedger::PublicationLedger(PublicationLedger &&) noexcept = default;
PublicationLedger &
PublicationLedger::operator=(PublicationLedger &&) noexcept = default;

std::pair<std::uint64_t, std::array<std::uint8_t, 32>>
PublicationLedger::appendPublication(PublicationEntry entry) {
  const std::size_t index = impl_->entries.size();
  entry.sequence          = static_cast<std::uint64_t>(index + 1);

  const auto h = entry.leafHash();
  merkle::HashT<32> mh;
  std::memcpy(mh.bytes, h.data(), 32);
  impl_->tree.insert(mh);

  if (!entry.infoHash.empty()) {
    impl_->infoHashMap[entry.infoHash] = index;
  }
  if (!entry.authorFingerprint.empty()) {
    impl_->authorMap[entry.authorFingerprint].push_back(index);
  }
  for (const auto &t : entry.topics) {
    const auto norm = normalizeTopicTag(t);
    if (!norm.empty()) {
      impl_->topicMap[norm].push_back(index);
    }
  }

  impl_->entries.push_back(std::move(entry));
  return {entry.sequence, root()};
}

std::array<std::uint8_t, 32> PublicationLedger::root() const {
  std::array<std::uint8_t, 32> out{};
  if (impl_->entries.empty()) {
    return out;
  }
  const auto r = impl_->tree.root();
  std::memcpy(out.data(), r.bytes, 32);
  return out;
}

std::string PublicationLedger::rootHex() const { return toHex32(root()); }

std::size_t PublicationLedger::size() const { return impl_->entries.size(); }

bool PublicationLedger::empty() const { return impl_->entries.empty(); }

const PublicationEntry &
PublicationLedger::entry(const std::size_t index) const {
  if (index >= impl_->entries.size()) {
    throw std::out_of_range("PublicationLedger index out of range");
  }
  return impl_->entries[index];
}

const std::vector<PublicationEntry> &PublicationLedger::entries() const {
  return impl_->entries;
}

MerkleProof PublicationLedger::generateProof(const std::size_t index) const {
  if (index >= impl_->entries.size()) {
    throw std::out_of_range("PublicationLedger index out of range");
  }

  const auto path = impl_->tree.path(index);
  MerkleProof proof;
  proof.leafIndex = index;
  proof.maxIndex  = impl_->entries.size() - 1;
  proof.leafHash  = impl_->entries[index].leafHash();
  proof.rootHash  = root();

  for (std::size_t i = 0; i < path->size(); ++i) {
    MerkleProof::Element elem;
    std::memcpy(elem.hash.data(), (*path)[i].bytes, 32);
    elem.isLeft = (0 != (index & (std::size_t{1} << i)));
    proof.path.push_back(elem);
  }

  return proof;
}

bool PublicationLedger::verifyInclusion(
    const PublicationEntry &entry, const MerkleProof &proof,
    const std::array<std::uint8_t, 32> &expectedRoot) {
  if (entry.leafHash() != proof.leafHash) {
    return false;
  }

  merkle::HashT<32> cur;
  std::memcpy(cur.bytes, proof.leafHash.data(), 32);

  for (const auto &elem : proof.path) {
    merkle::HashT<32> sibling;
    std::memcpy(sibling.bytes, elem.hash.data(), 32);
    merkle::HashT<32> parent;
    if (elem.isLeft) {
      sha256_pub(sibling, cur, parent);
    } else {
      sha256_pub(cur, sibling, parent);
    }
    cur = parent;
  }

  return std::memcmp(cur.bytes, expectedRoot.data(), 32) == 0;
}

const PublicationEntry *
PublicationLedger::findByInfoHash(std::string_view infoHash) const {
  const auto it = impl_->infoHashMap.find(std::string(infoHash));
  if (it != impl_->infoHashMap.end()) {
    return &impl_->entries[it->second];
  }
  return nullptr;
}

std::vector<const PublicationEntry *>
PublicationLedger::findByTopic(std::string_view topic) const {
  const auto norm = normalizeTopicTag(topic);
  const auto it   = impl_->topicMap.find(norm);
  std::vector<const PublicationEntry *> out;
  if (it != impl_->topicMap.end()) {
    out.reserve(it->second.size());
    for (const auto idx : it->second) {
      out.push_back(&impl_->entries[idx]);
    }
  }
  return out;
}

std::vector<const PublicationEntry *>
PublicationLedger::findByAuthor(std::string_view authorFingerprint) const {
  const auto it = impl_->authorMap.find(std::string(authorFingerprint));
  std::vector<const PublicationEntry *> out;
  if (it != impl_->authorMap.end()) {
    out.reserve(it->second.size());
    for (const auto idx : it->second) {
      out.push_back(&impl_->entries[idx]);
    }
  }
  return out;
}

std::string PublicationLedger::toYaml() const {
  std::ostringstream ss;
  ss << "publication_ledger:\n"
     << "  root: \"" << rootHex() << "\"\n"
     << "  count: " << impl_->entries.size() << "\n"
     << "  entries:\n";

  for (const auto &e : impl_->entries) {
    ss << "    - info_hash: \"" << e.infoHash << "\"\n"
       << "      bep46_uri: \"" << e.bep46Uri << "\"\n"
       << "      title: \"" << e.title << "\"\n"
       << "      author_name: \"" << e.authorName << "\"\n"
       << "      author_fingerprint: \"" << e.authorFingerprint << "\"\n"
       << "      abstract: \"" << e.abstractText << "\"\n"
       << "      timestamp: " << e.timestamp << "\n"
       << "      sequence: " << e.sequence << "\n"
       << "      total_bytes: " << e.totalBytes << "\n"
       << "      microversions: " << e.microversions << "\n"
       << "      has_transcopyright: "
       << (e.hasTranscopyright ? "true" : "false") << "\n"
       << "      transcopyright_terms: \"" << e.transcopyrightTerms << "\"\n"
       << "      merkle_root: \"" << toHex32(e.merkleRoot) << "\"\n"
       << "      signature: \"" << e.signature << "\"\n"
       << "      topics:\n";
    for (const auto &t : e.topics) {
      ss << "        - \"" << t << "\"\n";
    }
  }
  return ss.str();
}

PublicationLedger PublicationLedger::fromYaml(std::string_view yaml) {
  PublicationLedger ledger;
  // Parse entries simply line by line or minimal YAML scanner
  std::istringstream stream{std::string(yaml)};
  std::string line;
  PublicationEntry cur;
  bool inEntry  = false;
  bool inTopics = false;

  while (std::getline(stream, line)) {
    const auto trimmed = line.find_first_not_of(" \t\r\n");
    if (trimmed == std::string::npos) {
      continue;
    }
    const auto content = line.substr(trimmed);

    if (content.rfind("- info_hash:", 0) == 0) {
      if (inEntry) {
        ledger.appendPublication(std::move(cur));
        cur = PublicationEntry{};
      }
      inEntry       = true;
      inTopics      = false;
      const auto q1 = content.find('"');
      const auto q2 = content.rfind('"');
      if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
        cur.infoHash = content.substr(q1 + 1, q2 - q1 - 1);
      }
    } else if (inEntry) {
      auto extractQuoted = [&](std::string_view prefix) -> std::string {
        if (content.rfind(prefix, 0) == 0) {
          const auto q1 = content.find('"');
          const auto q2 = content.rfind('"');
          if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
            return content.substr(q1 + 1, q2 - q1 - 1);
          }
        }
        return {};
      };

      if (content.rfind("bep46_uri:", 0) == 0) {
        cur.bep46Uri = extractQuoted("bep46_uri:");
      } else if (content.rfind("title:", 0) == 0) {
        cur.title = extractQuoted("title:");
      } else if (content.rfind("author_name:", 0) == 0) {
        cur.authorName = extractQuoted("author_name:");
      } else if (content.rfind("author_fingerprint:", 0) == 0) {
        cur.authorFingerprint = extractQuoted("author_fingerprint:");
      } else if (content.rfind("abstract:", 0) == 0) {
        cur.abstractText = extractQuoted("abstract:");
      } else if (content.rfind("timestamp:", 0) == 0) {
        cur.timestamp = std::strtoull(content.substr(10).c_str(), nullptr, 10);
      } else if (content.rfind("sequence:", 0) == 0) {
        cur.sequence = std::strtoull(content.substr(9).c_str(), nullptr, 10);
      } else if (content.rfind("total_bytes:", 0) == 0) {
        cur.totalBytes = std::strtoull(content.substr(12).c_str(), nullptr, 10);
      } else if (content.rfind("microversions:", 0) == 0) {
        cur.microversions = static_cast<std::uint32_t>(
            std::strtoul(content.substr(14).c_str(), nullptr, 10));
      } else if (content.rfind("has_transcopyright:", 0) == 0) {
        cur.hasTranscopyright = (content.find("true") != std::string::npos);
      } else if (content.rfind("transcopyright_terms:", 0) == 0) {
        cur.transcopyrightTerms = extractQuoted("transcopyright_terms:");
      } else if (content.rfind("merkle_root:", 0) == 0) {
        const auto hex = extractQuoted("merkle_root:");
        if (const auto opt = fromHex32(hex)) {
          cur.merkleRoot = *opt;
        }
      } else if (content.rfind("signature:", 0) == 0) {
        cur.signature = extractQuoted("signature:");
      } else if (content.rfind("topics:", 0) == 0) {
        inTopics = true;
      } else if (inTopics && content.rfind("- \"", 0) == 0) {
        const auto q1 = content.find('"');
        const auto q2 = content.rfind('"');
        if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
          cur.topics.push_back(content.substr(q1 + 1, q2 - q1 - 1));
        }
      }
    }
  }

  if (inEntry) {
    ledger.appendPublication(std::move(cur));
  }

  return ledger;
}

bool PublicationLedger::saveToFile(const std::string &path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << toYaml();
  return out.good();
}

std::optional<PublicationLedger>
PublicationLedger::loadFromFile(const std::string &path) {
  std::ifstream in(path);
  if (!in.is_open()) {
    return std::nullopt;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  return fromYaml(content);
}

MadeTorrent PublicationLedger::sealToTorrent(std::string_view name,
                                             std::uint64_t pieceLength) const {
  std::vector<TorrentContent> files;
  const auto yamlStr = toYaml();
  files.push_back(TorrentContent{"PUBLICATION_LEDGER.yaml", yamlStr});

  const auto rootStr = rootHex() + "\n";
  files.push_back(TorrentContent{"ROOT.hex", rootStr});

  return makeTorrent(files, std::string(name), pieceLength);
}

} // namespace xanadu
