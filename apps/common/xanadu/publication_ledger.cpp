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

#include "merkle_domain.hpp"

namespace xanadu {

namespace {

std::string tsvEscape(std::string_view value) {
  std::string out;
  for (char c : value) {
    if (c == '\\' || c == '\t' || c == '\n' || c == '\r' || c == ',')
      out.push_back('\\');
    switch (c) {
    case '\t':
      out.push_back('t');
      break;
    case '\n':
      out.push_back('n');
      break;
    case '\r':
      out.push_back('r');
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

std::string tsvUnescape(std::string_view value) {
  std::string out;
  bool escaped = false;
  for (char c : value) {
    if (escaped) {
      out.push_back(c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c);
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else {
      out.push_back(c);
    }
  }
  if (escaped) out.push_back('\\');
  return out;
}

std::vector<std::string> splitTopics(std::string_view value) {
  std::vector<std::string> out;
  std::size_t start = 0;
  bool escaped      = false;
  for (std::size_t i = 0; i <= value.size(); ++i) {
    if (i < value.size() && value[i] == '\\') {
      escaped = !escaped;
      continue;
    }
    if (i == value.size() || (value[i] == ',' && !escaped)) {
      out.push_back(tsvUnescape(value.substr(start, i - start)));
      start = i + 1;
    }
    escaped = false;
  }
  if (out.size() == 1 && out.front().empty()) out.clear();
  return out;
}

std::array<std::uint8_t, 32> parseHex32(std::string_view value) {
  std::array<std::uint8_t, 32> out{};
  if (value.size() != 64) return out;
  for (std::size_t i = 0; i < out.size(); ++i) {
    unsigned v = 0;
    for (char c : value.substr(i * 2, 2)) {
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F')))
        return {};
      v <<= 4;
      v |= c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : c - 'A' + 10;
    }
    out[i] = static_cast<std::uint8_t>(v);
  }
  return out;
}

constexpr char kLeafDomain     = kMerkleLeafDomain;
constexpr char kInteriorDomain = kMerkleInteriorDomain;

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

  const std::uint64_t sequence = entry.sequence;
  impl_->entries.push_back(std::move(entry));
  return {sequence, root()};
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
  ss << "info_hash\tbep46_uri\ttitle\tauthor_name\tauthor_"
        "fingerprint\tabstract\ttimestamp\tsequence\ttotal_"
        "bytes\tmicroversions\thas_transcopyright\ttranscopyright_"
        "terms\tmerkle_root\tsignature\ttopics\n";
  for (const auto &e : impl_->entries) {
    ss << tsvEscape(e.infoHash) << '\t' << tsvEscape(e.bep46Uri) << '\t'
       << tsvEscape(e.title) << '\t' << tsvEscape(e.authorName) << '\t'
       << tsvEscape(e.authorFingerprint) << '\t' << tsvEscape(e.abstractText)
       << '\t' << e.timestamp << '\t' << e.sequence << '\t' << e.totalBytes
       << '\t' << e.microversions << '\t'
       << (e.hasTranscopyright ? "true" : "false") << '\t'
       << tsvEscape(e.transcopyrightTerms) << '\t' << toHex32(e.merkleRoot)
       << '\t' << tsvEscape(e.signature) << '\t';
    for (std::size_t i = 0; i < e.topics.size(); ++i) {
      if (i) ss << ',';
      ss << tsvEscape(e.topics[i]);
    }
    ss << '\n';
  }
  return ss.str();
}

PublicationLedger PublicationLedger::fromYaml(std::string_view yaml) {
  PublicationLedger ledger;
  if (yaml.find('\t') != std::string_view::npos) {
    std::istringstream lines{std::string(yaml)};
    std::string line;
    std::getline(lines, line);
    while (std::getline(lines, line)) {
      std::vector<std::string> f;
      std::size_t p = 0;
      while (p <= line.size()) {
        auto q = line.find('\t', p);
        f.push_back(line.substr(p, q == std::string::npos ? q : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
      }
      if (f.size() < 15) continue;
      try {
        PublicationEntry e;
        e.infoHash            = tsvUnescape(f[0]);
        e.bep46Uri            = tsvUnescape(f[1]);
        e.title               = tsvUnescape(f[2]);
        e.authorName          = tsvUnescape(f[3]);
        e.authorFingerprint   = tsvUnescape(f[4]);
        e.abstractText        = tsvUnescape(f[5]);
        e.timestamp           = std::stoull(f[6]);
        e.sequence            = std::stoull(f[7]);
        e.totalBytes          = std::stoull(f[8]);
        e.microversions       = static_cast<std::uint32_t>(std::stoul(f[9]));
        e.hasTranscopyright   = f[10] == "true";
        e.transcopyrightTerms = tsvUnescape(f[11]);
        e.merkleRoot          = parseHex32(f[12]);
        e.signature           = tsvUnescape(f[13]);
        e.topics              = splitTopics(f[14]);
        ledger.appendPublication(std::move(e));
      } catch (const std::exception &) {
        continue;
      }
    }
    return ledger;
  }
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

    if (content.starts_with("- info_hash:")) {
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
        if (content.starts_with(prefix)) {
          const auto q1 = content.find('"');
          const auto q2 = content.rfind('"');
          if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1) {
            return content.substr(q1 + 1, q2 - q1 - 1);
          }
        }
        return {};
      };

      if (content.starts_with("bep46_uri:")) {
        cur.bep46Uri = extractQuoted("bep46_uri:");
      } else if (content.starts_with("title:")) {
        cur.title = extractQuoted("title:");
      } else if (content.starts_with("author_name:")) {
        cur.authorName = extractQuoted("author_name:");
      } else if (content.starts_with("author_fingerprint:")) {
        cur.authorFingerprint = extractQuoted("author_fingerprint:");
      } else if (content.starts_with("abstract:")) {
        cur.abstractText = extractQuoted("abstract:");
      } else if (content.starts_with("timestamp:")) {
        cur.timestamp = std::strtoull(content.substr(10).c_str(), nullptr, 10);
      } else if (content.starts_with("sequence:")) {
        cur.sequence = std::strtoull(content.substr(9).c_str(), nullptr, 10);
      } else if (content.starts_with("total_bytes:")) {
        cur.totalBytes = std::strtoull(content.substr(12).c_str(), nullptr, 10);
      } else if (content.starts_with("microversions:")) {
        cur.microversions = static_cast<std::uint32_t>(
            std::strtoul(content.substr(14).c_str(), nullptr, 10));
      } else if (content.starts_with("has_transcopyright:")) {
        cur.hasTranscopyright = (content.contains("true"));
      } else if (content.starts_with("transcopyright_terms:")) {
        cur.transcopyrightTerms = extractQuoted("transcopyright_terms:");
      } else if (content.starts_with("merkle_root:")) {
        const auto hex = extractQuoted("merkle_root:");
        if (const auto opt = fromHex32(hex)) {
          cur.merkleRoot = *opt;
        }
      } else if (content.starts_with("signature:")) {
        cur.signature = extractQuoted("signature:");
      } else if (content.starts_with("topics:")) {
        inTopics = true;
      } else if (inTopics && content.starts_with("- \"")) {
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
  files.push_back(
      TorrentContent{.path = "PUBLICATION_LEDGER.tsv", .data = yamlStr});

  const auto rootStr = rootHex() + "\n";
  files.push_back(TorrentContent{.path = "ROOT.hex", .data = rootStr});

  return makeTorrent(files, std::string(name), pieceLength);
}

} // namespace xanadu
