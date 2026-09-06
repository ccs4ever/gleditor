/**
 * @file swarm_catalog.cpp
 * @brief Implementation of catalog coordinator for Swarm Telescope discovery.
 */
#include "swarm_catalog.hpp"

#include <libtorrent/hasher.hpp>

#include <algorithm>
#include <map>

namespace xudu {

struct SwarmCatalog::Impl {
  PublicationLedger ledger;
  SwarmCatalogIndex index;
  std::vector<AuthorNode> authors;
  std::vector<TopicSwarmNode> topics;
  std::map<std::string, PublicationEntry> pubsByHash;

  explicit Impl(const std::string &dbPath) : index(dbPath) {}
};

SwarmCatalog::SwarmCatalog() : SwarmCatalog(":memory:") {}

SwarmCatalog::SwarmCatalog(const std::string &cacheDbPath)
    : impl_(std::make_unique<Impl>(cacheDbPath)) {
  seedDefaultDocuverse();
}

SwarmCatalog::~SwarmCatalog()                                   = default;
SwarmCatalog::SwarmCatalog(SwarmCatalog &&) noexcept            = default;
SwarmCatalog &SwarmCatalog::operator=(SwarmCatalog &&) noexcept = default;

void SwarmCatalog::addPublication(PublicationEntry entry, const int seeders,
                                  const int peers, const bool verified) {
  if (entry.infoHash.empty()) {
    return;
  }
  impl_->index.indexPublication(entry, seeders, peers, verified);
  impl_->pubsByHash[entry.infoHash] = entry;
  impl_->ledger.appendPublication(std::move(entry));
}

std::vector<AuthorNode> SwarmCatalog::followedAuthors() const {
  return impl_->authors;
}

std::vector<TopicSwarmNode> SwarmCatalog::topicSwarms() const {
  return impl_->topics;
}

std::vector<SearchResult>
SwarmCatalog::search(std::string_view query,
                     const std::optional<CatalogCategory> category,
                     const std::size_t limit) const {
  if (!category) {
    return impl_->index.search(query, limit);
  }

  // Pre-filter or post-filter by category
  auto results = impl_->index.search(query, limit * 2);
  std::vector<SearchResult> filtered;
  filtered.reserve(results.size());

  for (auto &&res : results) {
    if (*category == CatalogCategory::RecentLocal) {
      if (res.entry.bep46Uri.find("local") != std::string::npos ||
          res.entry.authorFingerprint.empty()) {
        filtered.push_back(std::move(res));
      }
    } else if (*category == CatalogCategory::FollowedAuthors) {
      bool isFollowed = false;
      for (const auto &auth : impl_->authors) {
        if (auth.fingerprint == res.entry.authorFingerprint) {
          isFollowed = true;
          break;
        }
      }
      if (isFollowed) {
        filtered.push_back(std::move(res));
      }
    } else if (*category == CatalogCategory::TopicSwarms) {
      if (!res.entry.topics.empty()) {
        filtered.push_back(std::move(res));
      }
    }
    if (filtered.size() >= limit) {
      break;
    }
  }
  return filtered;
}

std::optional<PublicationEntry>
SwarmCatalog::findPublication(std::string_view infoHash) const {
  const auto it = impl_->pubsByHash.find(std::string(infoHash));
  if (it != impl_->pubsByHash.end()) {
    return it->second;
  }
  return std::nullopt;
}

SwarmCatalogIndex &SwarmCatalog::index() { return impl_->index; }

const SwarmCatalogIndex &SwarmCatalog::index() const { return impl_->index; }

const PublicationLedger &SwarmCatalog::ledger() const { return impl_->ledger; }

void SwarmCatalog::seedDefaultDocuverse() {
  // 1. Authors
  impl_->authors = {
      AuthorNode{
          .name        = "Theodor Holm Nelson",
          .fingerprint = "9F4A28C108BE4D1F0E7A8B9C1D2E3F4A5B6C7D8E",
          .pubKeyHex = "9f4a28c1e7a8b9c1d2e3f4a5b6c7d8e0123456789abcdef01234567"
                       "89abcdef0",
          .seederNodes      = 14,
          .publicationCount = 2,
      },
      AuthorNode{
          .name        = "Alan Kay",
          .fingerprint = "8B31A5F09876543210FEDCBA9876543210FEDCBA",
          .pubKeyHex =
              "8b31a5f0fedcba9876543210fedcba0123456789abcdef0123456789abcdef0",
          .seederNodes      = 12,
          .publicationCount = 1,
      },
      AuthorNode{
          .name        = "Douglas Engelbart",
          .fingerprint = "7B8C9D0123456789ABCDEF0123456789ABCDEF01",
          .pubKeyHex =
              "7b8c9d01cdef0123456789abcdef0123456789abcdef0123456789abcdef012",
          .seederNodes      = 8,
          .publicationCount = 1,
      },
  };

  // 2. Topic Swarms
  impl_->topics = {
      TopicSwarmNode{.topic       = "hypertext",
                     .infoHash    = "a1b2c3d4e5f60718293a4b5c6d7e8f901a2b3c4d",
                     .activePeers = 38,
                     .publicationCount = 4},
      TopicSwarmNode{.topic       = "transclusion",
                     .infoHash    = "b2c3d4e5f60718293a4b5c6d7e8f901a2b3c4d5e",
                     .activePeers = 24,
                     .publicationCount = 2},
      TopicSwarmNode{.topic       = "distributed-os",
                     .infoHash    = "c3d4e5f60718293a4b5c6d7e8f901a2b3c4d5e6f",
                     .activePeers = 19,
                     .publicationCount = 2},
      TopicSwarmNode{.topic       = "typography",
                     .infoHash    = "d4e5f60718293a4b5c6d7e8f901a2b3c4d5e6f70",
                     .activePeers = 15,
                     .publicationCount = 1},
      TopicSwarmNode{.topic       = "transcopyright",
                     .infoHash    = "e5f60718293a4b5c6d7e8f901a2b3c4d5e6f7081",
                     .activePeers = 7,
                     .publicationCount = 1},
  };

  // 3. Publications
  PublicationEntry doc1{
      .infoHash   = "dc308895c32545a2fb09f050d0be66164b234219",
      .bep46Uri   = "btpk:9f4a28c1e7a8b9c1d2e3f4a5b6c7d8e:doc:transclusion",
      .title      = "The Foundations of Transclusion",
      .authorName = "Theodor Holm Nelson",
      .authorFingerprint = "9F4A28C108BE4D1F0E7A8B9C1D2E3F4A5B6C7D8E",
      .topics            = {"hypertext", "transclusion", "osmic", "xanadu"},
      .abstractText =
          "Theodor Holm Nelson's foundational thesis on non-destructive "
          "quotation, coordinate space invariance, and the elimination of "
          "copy-paste clipboard silos in universal literature.",
      .timestamp           = 1700000000ULL,
      .sequence            = 1,
      .totalBytes          = 1420000ULL,
      .microversions       = 42,
      .hasTranscopyright   = false,
      .transcopyrightTerms = "",
      .merkleRoot          = {0x01, 0x02, 0x03, 0x04},
      .signature           = "sig_ed25519_ted_nelson_foundations",
  };
  addPublication(doc1, 14, 3, true);

  PublicationEntry doc2{
      .infoHash          = "e2f3a4b5c6d7e8f901a2b3c4d5e6f708192a3b4c",
      .bep46Uri          = "btpk:7b8c9d01cdef0123456789abcdef012:doc:augment",
      .title             = "Augmenting Human Intellect",
      .authorName        = "Douglas Engelbart",
      .authorFingerprint = "7B8C9D0123456789ABCDEF0123456789ABCDEF01",
      .topics        = {"augmentation", "ui", "distributed-os", "hypertext"},
      .abstractText  = "A conceptual framework for the augmentation of man's "
                       "intellect through interactive computing, associative "
                       "trails, and shared collaborative hypermedia.",
      .timestamp     = 1701000000ULL,
      .sequence      = 2,
      .totalBytes    = 980000ULL,
      .microversions = 18,
      .hasTranscopyright   = false,
      .transcopyrightTerms = "",
      .merkleRoot          = {0x05, 0x06, 0x07, 0x08},
      .signature           = "sig_ed25519_doug_engelbart_augment",
  };
  addPublication(doc2, 8, 2, true);

  PublicationEntry doc3{
      .infoHash          = "f3a4b5c6d7e8f901a2b3c4d5e6f708192a3b4c5d",
      .bep46Uri          = "btpk:8b31a5f0fedcba9876543210fedcba:doc:dynabook",
      .title             = "Personal Dynamic Media",
      .authorName        = "Alan Kay",
      .authorFingerprint = "8B31A5F09876543210FEDCBA9876543210FEDCBA",
      .topics       = {"dynabook", "smalltalk", "typography", "distributed-os"},
      .abstractText = "Alan Kay and Adele Goldberg's foundational vision of "
                      "the Dynabook: an active, personal, dynamic medium for "
                      "creative thought and multidimensional communication.",
      .timestamp    = 1702000000ULL,
      .sequence     = 3,
      .totalBytes   = 2100000ULL,
      .microversions       = 31,
      .hasTranscopyright   = false,
      .transcopyrightTerms = "",
      .merkleRoot          = {0x09, 0x0a, 0x0b, 0x0c},
      .signature           = "sig_ed25519_alan_kay_dynabook",
  };
  addPublication(doc3, 12, 4, true);

  PublicationEntry doc4{
      .infoHash   = "a4b5c6d7e8f901a2b3c4d5e6f708192a3b4c5d6e",
      .bep46Uri   = "btpk:9f4a28c1e7a8b9c1d2e3f4a5b6c7d8e:doc:transcopyright",
      .title      = "Transcopyright: Economic Foundation of the Docuverse",
      .authorName = "Theodor Holm Nelson",
      .authorFingerprint = "9F4A28C108BE4D1F0E7A8B9C1D2E3F4A5B6C7D8E",
      .topics = {"transcopyright", "micropayments", "economics", "hypertext"},
      .abstractText  = "Permissionless quotation with direct reader-to-author "
                       "micropayment state channels. Anyone may quote without "
                       "prior license; reading triggers per-byte settlement.",
      .timestamp     = 1703000000ULL,
      .sequence      = 4,
      .totalBytes    = 750000ULL,
      .microversions = 14,
      .hasTranscopyright   = true,
      .transcopyrightTerms = "5 nano-XU/byte",
      .merkleRoot          = {0x0d, 0x0e, 0x0f, 0x10},
      .signature           = "sig_ed25519_ted_nelson_transcopyright",
  };
  addPublication(doc4, 18, 5, true);
}

} // namespace xudu
