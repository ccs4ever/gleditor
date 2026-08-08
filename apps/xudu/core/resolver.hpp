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
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "origin.hpp"
#include "spool.hpp"
#include "torrent.hpp"

namespace xudu {

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
  [[nodiscard]] virtual const Metainfo *metainfo(const InfoHash &hash) const = 0;

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
 * @class Resolver
 * @brief Reads spans, verifying anything that did not come from here.
 */
class Resolver {
public:
  /// @param source Where external content comes from. Not owned; may be null,
  ///        in which case external spans simply do not resolve.
  explicit Resolver(const ContentSource *aSource = nullptr) : source(aSource) {}

  void setSource(const ContentSource *aSource) { source = aSource; }
  [[nodiscard]] const ContentSource *contentSource() const { return source; }

  /**
   * @brief Read @p span of @p origin.
   *
   * Every piece the range touches is fetched whole and hashed against the
   * torrent before any of it is returned, because a piece hash covers a piece
   * and says nothing about a fragment of one. If any piece fails, or the
   * content is not available, nothing is returned: a partial or unverified
   * answer would be indistinguishable from the real thing to everything
   * downstream.
   *
   * @return The bytes, or nothing when they could not be obtained and
   *         verified.
   */
  [[nodiscard]] std::string read(const Origin &origin,
                                 const PrimediaSpan &span) const;

  /// Whether @p origin's content can be reached and verified at all.
  [[nodiscard]] bool available(const Origin &origin) const;

private:
  const ContentSource *source{};
};

} // namespace xudu

#endif // XUDU_RESOLVER_H
// vi: set sw=2 sts=2 ts=2 et:
