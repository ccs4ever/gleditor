/**
 * @file resolver.hpp
 * @brief Turning a torrent-backed address into bytes, and checking them.
 *
 * Split in two on purpose.
 *
 * Deciding *which* bytes a reference names, and whether the bytes in hand are
 * really those, is arithmetic over the piece hashes -- it needs no network, it
 * is the same everywhere, and it is where a mistake would be silent and
 * permanent. That part is here and is tested as such.
 *
 * Actually obtaining the bytes is somebody else's problem, behind
 * ContentSource. One implementation reads a directory that already holds the
 * torrent's data, which is what a machine that has finished downloading has;
 * another could drive a swarm. The resolver does not care which, because it
 * verifies whatever it is handed before believing it.
 *
 * That last point is the reason for the whole arrangement. Transclusion claims
 * there is only one copy of anything; without verification a reader has no way
 * to tell the copy in front of them from a substitution, and the claim is
 * merely a hope. Here, content that does not hash to what the reference named
 * is not returned at all.
 */
#ifndef XUDU_RESOLVER_H
#define XUDU_RESOLVER_H

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <gleditor/paths.hpp>

#include "lmdb_cache.hpp"
#include "scroll.hpp"
#include "spool.hpp"
#include "torrent.hpp"
#include "transcopyright_crypto.hpp"

namespace xanadu {

/// Status of a span resolution attempt.
enum class ResolutionStatus : std::uint8_t {
  VerifiedBytes        = 0,
  MissingPieces        = 1,
  UnverifiedHash       = 2,
  WithheldRedacted     = 3,
  TranscopyrightLocked = 4
};

/// Result of resolving a span, including text or lock/hole metadata.
struct ResolveResult {
  ResolutionStatus status{ResolutionStatus::VerifiedBytes};
  std::string text{};
  std::optional<TranscopyrightDescriptor> lockInfo{};
  std::optional<PublishedHoleRecord> holeRecord{};

  [[nodiscard]] bool isVerified() const noexcept {
    return status == ResolutionStatus::VerifiedBytes;
  }
  [[nodiscard]] bool isLocked() const noexcept {
    return status == ResolutionStatus::TranscopyrightLocked;
  }
  [[nodiscard]] bool isWithheld() const noexcept {
    return status == ResolutionStatus::WithheldRedacted;
  }
};

/**
 * @brief Somewhere the bytes of a torrent can be obtained from.
 *
 * Offsets are into the torrent's concatenated stream, not into any one file:
 * a piece straddles file boundaries, and verification is per piece, so the
 * stream is the only coordinate in which both make sense.
 */
class ContentSource {
public:
  ContentSource()          = default;
  virtual ~ContentSource() = default;

  ContentSource(const ContentSource &)            = delete;
  ContentSource &operator=(const ContentSource &) = delete;
  ContentSource(ContentSource &&)                 = delete;
  ContentSource &operator=(ContentSource &&)      = delete;

  /// The torrent's metadata, or nullptr when this source has never heard of
  /// it. Without it nothing can be verified, so nothing is returned.
  [[nodiscard]] virtual const Metainfo *
  metainfo(const InfoHash &hash) const = 0;

  /**
   * @brief Read [@p offset, @p offset + @p length) of the concatenated stream.
   *
   * May return fewer bytes than asked for, which the resolver treats as "not
   * available" rather than as a short answer. Returning the wrong bytes is the
   * one thing an implementation must not do; the resolver checks anyway.
   */
  [[nodiscard]] virtual std::string readStream(const InfoHash &hash,
                                               std::uint64_t offset,
                                               std::uint64_t length) const = 0;
};

/**
 * @brief A content source over a directory that already holds torrent data.
 *
 * What a machine that has finished downloading has: the files laid out under a
 * directory named by the torrent, exactly as the torrent describes them. This
 * is deliberately the simplest thing that is genuinely useful -- it makes
 * torrent-backed references work today, offline, and it is what the tests use.
 * A swarm-backed source implements the same two methods.
 */
class DirectoryContentSource : public ContentSource {
public:
  /**
   * @brief Make a torrent's content available.
   * @param torrentFile The contents of a .torrent file.
   * @param dataRoot Directory the torrent's files live under. For a
   *        single-file torrent this is the directory holding the file; for a
   *        multi-file one it is the directory the torrent's name refers to.
   * @return The info hash the content is known by.
   * @throws std::runtime_error if the torrent cannot be parsed.
   */
  InfoHash add(std::string_view torrentFile, std::string dataRoot);

  [[nodiscard]] const Metainfo *metainfo(const InfoHash &hash) const override;
  [[nodiscard]] std::string readStream(const InfoHash &hash,
                                       std::uint64_t offset,
                                       std::uint64_t length) const override;

private:
  struct Held {
    Metainfo meta;
    std::string root;
  };
  std::map<InfoHash, Held> held;
};

/**
 * @brief Verified pieces, kept by content address for one Resolver's lifetime.
 *
 * The key is `(info hash, piece index)`, which **names the bytes exactly**: a
 * piece hash is a cryptographic commitment, so any bytes that verify against it
 * are the bytes the reference meant, whatever they were fetched from. That is
 * what makes this cache sound where the resolved-text cache that used to sit in
 * resolve() was not -- a `PrimediaSpan` names a scroll by its slot in one
 * Store's externals table, so two documents' "scroll 1" collided in one
 * process-wide LMDB.
 *
 * **It is deliberately in memory and deliberately not the LMDB cache.** The
 * cost of caching a verified piece is that tampering with the local copy stops
 * being noticed for as long as the cache holds it:
 * `alteredContentIsNotReturned` in tests/xudu/resolver.cpp is the property
 * being traded against, and it is load-bearing rather than incidental. Scoping
 * the cache to one Resolver -- so one open document -- bounds that window to a
 * session and makes reopening a re-verification of everything, where a
 * persistent cache would have made the staleness permanent. A piece not yet
 * cached is still verified, so tampering is still caught everywhere the reader
 * has not already looked.
 *
 * Held through a shared_ptr so that copying a Resolver shares the cache rather
 * than being refused by the mutex. Sharing is right anyway: the key is a
 * content address, so two Resolvers cannot disagree about what a hit means.
 */
class VerifiedPieceCache {
public:
  /// Bytes to hold before evicting, not pieces: piece length varies per
  /// torrent, and it is the memory that needs bounding. Four MiB is 64 pieces
  /// at this tree's 64 KiB Merkle piece size, against the ~60 cells a frame
  /// visits -- enough that a frame reads no piece twice.
  static constexpr std::size_t defaultBudgetBytes = 4ULL * 1024ULL * 1024ULL;

  struct Stats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::size_t pieces{0};
    std::size_t bytes{0};
  };

  /// The piece, or nothing. Answers by value: the caller appends it to a buffer
  /// and a view would have to outlive an eviction by another thread.
  [[nodiscard]] std::optional<std::string> get(const InfoHash &torrent,
                                               std::size_t piece) const;

  /// Remember @p bytes as piece @p piece of @p torrent. Only ever called with
  /// bytes that have just verified, which is the invariant the whole class
  /// rests on -- there is no path that stores an unverified piece.
  void put(const InfoHash &torrent, std::size_t piece,
           const std::string &bytes);

  /// Forget everything. What a caller that knows the local copy changed
  /// underneath it does; nothing here can detect that by itself.
  void clear();

  void setBudgetBytes(std::size_t bytes);

  [[nodiscard]] Stats stats() const;

private:
  struct Key {
    InfoHash torrent;
    std::size_t piece{0};
    bool operator==(const Key &) const = default;
  };
  struct KeyHash {
    std::size_t operator()(const Key &key) const noexcept;
  };
  /// Most recently used at the front. A list so that an entry can be moved to
  /// the front without invalidating the map's iterators into it.
  using Entries = std::list<std::pair<Key, std::string>>;

  void evictDownToBudget();

  mutable std::mutex guard;
  mutable Entries entries;
  mutable std::unordered_map<Key, Entries::iterator, KeyHash> index;
  std::size_t budgetBytes{defaultBudgetBytes};
  mutable std::size_t bytesHeld{0};
  mutable Stats counters;
};

/**
 * @class Resolver
 * @brief Reads spans, verifying anything that did not come from here.
 */
class Resolver {
public:
  /// The cache holds unlocked Content Encryption Keys, so it belongs under
  /// the user's own cache directory rather than a fixed path in /tmp: a
  /// predictable world-writable name is one another account can create first
  /// and then read out of.
  [[nodiscard]] static std::filesystem::path defaultCacheDir() {
    return std::filesystem::path(gleditor::paths::cacheDir("xudu")) / "content";
  }

  /// @param source Where external content comes from. Not owned; may be null,
  ///        in which case external spans simply do not resolve.
  explicit Resolver(const ContentSource *aSource = nullptr)
      : Resolver(aSource, defaultCacheDir()) {}

  Resolver(const ContentSource *aSource, const std::filesystem::path &cacheDir)
      : source(aSource), cache(cacheDir) {}

  void setSource(const ContentSource *aSource) { source = aSource; }
  [[nodiscard]] const ContentSource *contentSource() const { return source; }

  /**
   * @brief Read @p span of @p scroll.
   *
   * This is the one place scroll coordinates are turned into torrent
   * coordinates, and the only place that knows a scroll has segments at all. A
   * range crossing a seal is fetched from both torrents and joined, so nothing
   * above here can tell where one segment ended.
   *
   * Every piece the range touches is fetched whole and hashed against its
   * torrent before any of it is returned, because a piece hash covers a piece
   * and says nothing about a fragment of one. If any piece fails, or any part
   * of the range is not carried by a segment at all, nothing is returned: a
   * partial or unverified answer would be indistinguishable from the real
   * thing to everything downstream. A quotation that spans a seal therefore
   * needs both segments -- half of it is not an answer.
   *
   * @return The bytes, or nothing when they could not be obtained and
   *         verified.
   */
  [[nodiscard]] std::string read(const Scroll &scroll,
                                 const PrimediaSpan &span) const;

  /**
   * @brief Resolve @p span of @p scroll, returning full verification status,
   *        withheld reason, or Transcopyright lock descriptor.
   */
  [[nodiscard]] ResolveResult resolve(const Scroll &scroll,
                                      const PrimediaSpan &span) const;

  /// Whether every segment of @p scroll names a torrent this source knows, so
  /// that the whole of it could be read and verified.
  [[nodiscard]] bool available(const Scroll &scroll) const;

  /// Cache an unlocked Transcopyright Content Encryption Key.
  bool unlockTranscopyright(const std::array<std::uint8_t, 32> &keyId,
                            const crypto::Key32 &cek,
                            std::uint64_t pricePaid         = 0,
                            std::string_view currencySymbol = "XU") const;

  [[nodiscard]] LMDBContentCache &contentCache() noexcept { return cache; }
  [[nodiscard]] const LMDBContentCache &contentCache() const noexcept {
    return cache;
  }

  /// The verified-piece cache. Public so that a caller who knows the content
  /// under a torrent has changed -- a re-seal, a repaired download -- can
  /// clear() it, and so that a test can read its Stats and see that a second
  /// read of one piece did not hash it again.
  [[nodiscard]] VerifiedPieceCache &pieceCache() const noexcept {
    return *pieces;
  }

private:
  /// One segment's worth: [@p from, @p from + @p count) in scroll
  /// coordinates, which this turns into stream coordinates and verifies.
  [[nodiscard]] std::string readSegment(const ScrollSegment &segment,
                                        std::uint64_t from,
                                        std::uint64_t count) const;

  const ContentSource *source{};
  mutable LMDBContentCache cache;
  /// Never null. Shared rather than held by value so that a Resolver stays
  /// copyable with a mutex inside its cache -- see VerifiedPieceCache.
  std::shared_ptr<VerifiedPieceCache> pieces{
      std::make_shared<VerifiedPieceCache>()};
};

} // namespace xanadu

#endif // XUDU_RESOLVER_H
