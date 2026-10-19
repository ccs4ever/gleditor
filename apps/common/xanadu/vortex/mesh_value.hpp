/**
 * @file mesh_value.hpp
 * @brief Signed, torrent-backed graph values at the distributed arena boundary.
 *
 * C++ here handles publication, signatures, verified I/O and ref translation;
 * graph computation remains the responsibility of the private worker runtime.
 */
#ifndef COMMON_XANADU_VORTEX_MESH_VALUE_HPP
#define COMMON_XANADU_VORTEX_MESH_VALUE_HPP

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "common/xanadu/publication.hpp"
#include "common/xanadu/store.hpp"
#include "common/xanadu/vortex/spool_mesh.hpp"

namespace zigzag::vortex::mesh {

struct ValueLimits {
  std::size_t manifestBytes{4 * 1024 * 1024};
  std::uint64_t payloadBytes{64 * 1024 * 1024};
  std::uint32_t operations{65536};
};

/// The signature binds the root and causal parent as well as the publication.
/// A publication signature alone would let a relay substitute a different root.
struct PublishedValue {
  NodeId origin;
  std::string
      job; ///< content identity of the immutable computation, when present
  std::string
      continuation; ///< signed checkpoint metadata, separate from job identity
  xanadu::MicroversionId parent;
  xanadu::GlobalOpRef root;
  xanadu::Publication publication;
  xanadu::Signature signature;
};

struct ValuePayloadSeed {
  xanadu::MadeTorrent torrent;
  std::filesystem::path path;
};

struct ValueSeed {
  PublishedValue value;
  xanadu::SealedScroll history;
  xanadu::MadeTorrent carrier;
  std::filesystem::path carrierPath;
  std::vector<ValuePayloadSeed> payloads;
};

[[nodiscard]] std::string valueSigningBuffer(const PublishedValue &value);
[[nodiscard]] std::string encodeValue(const PublishedValue &value);
[[nodiscard]] std::expected<PublishedValue, std::string>
decodeValue(std::string_view bytes, ValueLimits limits = {});

/// seedRoot is the stable author seed cache, shared by all jobs and retained
/// across restarts so prior permascroll segments remain available for seeding.
/// Promotes the reachable answer locally, then seals and signs its full
/// history. The supplied provenance must already be signed by the host's
/// authoring path.
[[nodiscard]] std::expected<ValueSeed, std::string>
publishValue(xanadu::Store &store, const xanadu::MicroversionId &parent,
             const ArenaManifold &arena, CellRef root,
             const xanadu::MutableKeys &keys, NodeId origin,
             const xanadu::SignedProvenance &provenance,
             const std::filesystem::path &seedRoot, ValueLimits limits = {},
             std::string job = {}, std::string continuation = {});

using ValueProvenanceSigner =
    std::function<std::expected<xanadu::SignedProvenance, std::string>(
        xanadu::Store &)>;

using ValuePrimediaProvenanceSigner =
    std::function<std::expected<xanadu::SignedProvenance, std::string>(
        xanadu::Store &, std::string_view bytes, std::uint64_t start,
        std::string_view scrollKey)>;

/// Signs the prepared post-promotion store, rather than a stale pre-evaluation
/// snapshot. The callback is local authoring policy, never received remotely.
[[nodiscard]] std::expected<ValueSeed, std::string>
publishValue(xanadu::Store &store, const xanadu::MicroversionId &parent,
             const ArenaManifold &arena, CellRef root,
             const xanadu::MutableKeys &keys, NodeId origin,
             const ValueProvenanceSigner &signer,
             const std::filesystem::path &seedRoot, ValueLimits limits = {},
             std::string job = {}, std::string continuation = {},
             ValuePrimediaProvenanceSigner primediaSigner = {});

class VerifiedSliceContentSource;

struct ReceivedValue {
  PublishedValue value;
  std::string contentHash;
  std::shared_ptr<VerifiedSliceContentSource> sliceSource;
  std::unique_ptr<xanadu::Store> store;
  std::unique_ptr<Manifold> manifold;
  CellRef root;
};

/// The ContentSource must outlive the received value. No arena ref crosses this
/// boundary; root is resolved from its global operation identity after replay.
[[nodiscard]] std::expected<ReceivedValue, std::string>
receiveValue(std::string_view manifest, const xanadu::ContentSource &source,
             std::shared_ptr<xanadu::UserPermascroll> reader,
             ValueLimits limits = {});

/// Fetch only after inspecting torrent metadata and the configured byte budget.
/// Resolver verifies the manifest's pieces; history replay verifies
/// dependencies.
[[nodiscard]] std::expected<ReceivedValue, std::string>
fetchValue(const xanadu::InfoHash &hash, const xanadu::ContentSource &source,
           std::shared_ptr<xanadu::UserPermascroll> reader,
           ValueLimits limits = {});

struct ImportedValue {
  std::unique_ptr<ArenaManifold> arena;
  CellRef root;
};

/// Rebuild an owned graph with receiver-local references and globally identical
/// spans. Registering scroll descriptors does not append primedia or
/// operations.
[[nodiscard]] std::expected<ImportedValue, std::string>
importValue(const ReceivedValue &value, xanadu::Store &destination,
            ValueLimits limits = {});

struct MergedValue {
  std::string contentHash;
  Promoted promoted;
};

/// Sort before minting any operations. Each result branches from the supplied
/// parent, retaining conflicting results; transport arrival order has no role.
[[nodiscard]] std::expected<std::vector<MergedValue>, std::string>
mergeValues(xanadu::Store &destination, const xanadu::MicroversionId &parent,
            std::span<const ReceivedValue *const> values,
            ValueLimits limits = {});

inline constexpr std::size_t kSpoolMerkleLeafBytes = 1024;

/// A self-contained, 1 KiB Merkle-leaf-verified and piece-verified torrent
/// stream slice, allowing zero-torrent-round-trip in-memory resolution of
/// SegmentedOpsSpool segments and newly authored primedia spans.
struct VerifiedTorrentSlice {
  xanadu::InfoHash hash;
  std::string metainfoBytes;
  std::string streamBytes;
  std::vector<std::string> merkleLeafHashes;
};

struct SpoolSliceBundle {
  PublishedValue value;
  std::string manifestBytes;
  VerifiedTorrentSlice carrier;
  VerifiedTorrentSlice history;
  std::vector<VerifiedTorrentSlice> payloads;
};

[[nodiscard]] std::vector<std::string>
computeSpoolMerkleLeaves(std::string_view streamBytes);

[[nodiscard]] bool
verifySpoolMerkleLeaves(std::string_view streamBytes,
                        std::span<const std::string> leafHashes);

[[nodiscard]] std::expected<VerifiedTorrentSlice, std::string>
packTorrentSlice(std::string_view torrentFile,
                 const std::filesystem::path &seedDir, std::uint64_t maxBytes);

[[nodiscard]] std::expected<xanadu::Metainfo, std::string>
verifyTorrentSlice(const VerifiedTorrentSlice &slice, std::uint64_t maxBytes);

[[nodiscard]] std::expected<SpoolSliceBundle, std::string>
packSpoolSliceBundle(const ValueSeed &seed, ValueLimits limits = {});

[[nodiscard]] std::string
encodeSpoolSliceBundle(const SpoolSliceBundle &bundle);

[[nodiscard]] std::expected<SpoolSliceBundle, std::string>
decodeSpoolSliceBundle(std::string_view bytes, ValueLimits limits = {});

/// Thread-safe in-memory ContentSource overlay verified at 1 KiB Merkle leaf
/// and BitTorrent piece boundaries, falling back to an underlying ContentSource
/// (such as SwarmContentSource) for uncached archival segments.
class VerifiedSliceContentSource final : public xanadu::ContentSource {
public:
  explicit VerifiedSliceContentSource(
      const xanadu::ContentSource *fallback = nullptr)
      : fallback_(fallback) {}

  VerifiedSliceContentSource *
  setFallback(const xanadu::ContentSource *fallback) noexcept {
    std::scoped_lock lock(mutex_);
    fallback_ = fallback;
    return this;
  }

  [[nodiscard]] std::expected<void, std::string>
  ingestSlice(const VerifiedTorrentSlice &slice, std::uint64_t maxBytes);

  [[nodiscard]] std::expected<void, std::string>
  ingestBundle(const SpoolSliceBundle &bundle, ValueLimits limits = {});

  [[nodiscard]] std::expected<void, std::string>
  ingestSeed(const ValueSeed &seed, ValueLimits limits = {});

  [[nodiscard]] bool contains(const xanadu::InfoHash &hash) const;

  [[nodiscard]] std::optional<VerifiedTorrentSlice>
  exportSlice(const xanadu::InfoHash &hash) const;

  [[nodiscard]] gleditor::cpp26::optional<const xanadu::Metainfo &>
  metainfo(const xanadu::InfoHash &hash) const override;

  [[nodiscard]] std::string readStream(const xanadu::InfoHash &hash,
                                       std::uint64_t offset,
                                       std::uint64_t length) const override;

private:
  struct Entry {
    xanadu::Metainfo meta;
    std::string metainfoBytes;
    std::string streamBytes;
    std::vector<std::string> merkleLeafHashes;
  };
  mutable std::mutex mutex_;
  const xanadu::ContentSource *fallback_{nullptr};
  std::map<xanadu::InfoHash, Entry> entries_;
};

[[nodiscard]] std::expected<ReceivedValue, std::string> receiveSpoolSliceBundle(
    std::string_view bundleBytes, const xanadu::ContentSource *fallbackSource,
    std::shared_ptr<xanadu::UserPermascroll> reader, ValueLimits limits = {});

} // namespace zigzag::vortex::mesh
#endif
