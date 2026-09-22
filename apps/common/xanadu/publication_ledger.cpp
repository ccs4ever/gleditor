/**
 * @file publication_ledger.cpp
 * @brief Implementation of the append-only publication Merkle ledger.
 */
#include "publication_ledger.hpp"

#include <libtorrent/hasher.hpp>
#include <merklecpp.h>

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

std::string PublicationLedger::toTsv() const {
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

PublicationLedger PublicationLedger::fromTsv(const std::string_view tsv) {
  PublicationLedger ledger;
  std::istringstream stream{std::string(tsv)};
  std::string line;
  if (!std::getline(stream, line) || !line.starts_with("info_hash\t")) {
    return ledger;
  }
  while (std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
      const auto tab = line.find('\t', start);
      fields.push_back(
          line.substr(start, std::string::npos == tab ? tab : tab - start));
      if (std::string::npos == tab) {
        break;
      }
      start = tab + 1;
    }
    if (15 != fields.size()) {
      continue;
    }
    try {
      PublicationEntry entry;
      entry.infoHash          = tsvUnescape(fields[0]);
      entry.bep46Uri          = tsvUnescape(fields[1]);
      entry.title             = tsvUnescape(fields[2]);
      entry.authorName        = tsvUnescape(fields[3]);
      entry.authorFingerprint = tsvUnescape(fields[4]);
      entry.abstractText      = tsvUnescape(fields[5]);
      entry.timestamp         = std::stoull(fields[6]);
      entry.sequence          = std::stoull(fields[7]);
      entry.totalBytes        = std::stoull(fields[8]);
      entry.microversions = static_cast<std::uint32_t>(std::stoul(fields[9]));
      entry.hasTranscopyright   = "true" == fields[10];
      entry.transcopyrightTerms = tsvUnescape(fields[11]);
      entry.merkleRoot          = parseHex32(fields[12]);
      entry.signature           = tsvUnescape(fields[13]);
      entry.topics              = splitTopics(fields[14]);
      ledger.appendPublication(std::move(entry));
    } catch (const std::exception &) {
      continue;
    }
  }
  return ledger;
}

bool PublicationLedger::saveToFile(const std::string &path) const {
  std::ofstream out(path, std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }
  out << toTsv();
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
  return fromTsv(content);
}

MadeTorrent PublicationLedger::sealToTorrent(std::string_view name,
                                             std::uint64_t pieceLength) const {
  std::vector<TorrentContent> files;
  const auto tsv = toTsv();
  files.push_back(
      TorrentContent{.path = "PUBLICATION_LEDGER.tsv", .data = tsv});

  const auto rootStr = rootHex() + "\n";
  files.push_back(TorrentContent{.path = "ROOT.hex", .data = rootStr});

  return makeTorrent(files, std::string(name), pieceLength);
}

} // namespace xanadu
