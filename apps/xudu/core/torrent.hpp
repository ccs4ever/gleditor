/**
 * @file torrent.hpp
 * @brief A torrent as a stable, verifiable name for content.
 *
 * Xanadu wants content to have "canonical network addresses" -- a name that
 * identifies a piece of content itself, everywhere, permanently. The primedia
 * spool as it stands gives an offset into one file on one machine, which is
 * none of those things: it means nothing to anyone else, and it means nothing
 * to you either once that machine is gone.
 *
 * A torrent's info hash is a name of the kind Xanadu asks for, and it is one
 * that already exists and is already deployed.
 *
 * - It is derived from the content, so the name and the identity are the same
 *   thing. Nobody assigns it and nobody can reassign it.
 * - The same content always produces the same name, so a reference made today
 *   still resolves in twenty years provided anything anywhere still has the
 *   bytes.
 * - Resolving it does not depend on a particular server staying up, which is
 *   the failure that makes ordinary hyperlinks rot. Nelson's model has "feed
 *   and sale... from original"; a swarm is that without a single original
 *   having to answer the phone.
 * - Every piece carries a hash, so what arrives can be checked against what
 *   was asked for. This is the part that makes transclusion honest rather than
 *   hopeful: the premise is that there is only one copy of anything, and
 *   verification is how a reader knows the bytes in front of them really are
 *   that copy.
 *
 * What is here is the addressing and the verification, which are pure
 * computation and are tested as such. Where the bytes are fetched from is
 * somebody else's problem -- see ContentSource in resolver.hpp.
 */
#ifndef XUDU_TORRENT_H
#define XUDU_TORRENT_H

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xudu {

/// A BitTorrent v1 info hash: the SHA-1 of the bencoded info dictionary.
struct InfoHash {
  std::array<std::uint8_t, 20> bytes{};

  /// Lowercase hex, which is how one is written in a magnet link.
  [[nodiscard]] std::string hex() const;
  /// Parse forty hex digits. Nothing else is accepted, including the base-32
  /// form, which is a different length and is not what this writes.
  [[nodiscard]] static InfoHash fromHex(std::string_view text);
  [[nodiscard]] bool isZero() const;

  bool operator==(const InfoHash &) const = default;
  [[nodiscard]] bool operator<(const InfoHash &other) const {
    return bytes < other.bytes;
  }
};

/// One file inside a torrent, and where it sits in the piece stream.
struct TorrentFile {
  /// Path components as the torrent gives them, joined with '/'.
  std::string path;
  std::uint64_t length{};
  /**
   * @brief Where this file begins in the concatenated stream.
   *
   * "the multi-file case is treated as only having a single file by
   * concatenating the files in the order they appear in the files list", so a
   * piece can straddle a file boundary and a file rarely begins on one.
   */
  std::uint64_t offset{};
};

/**
 * @class Metainfo
 * @brief A parsed torrent file.
 */
class Metainfo {
public:
  /**
   * @brief Parse the contents of a .torrent file.
   * @throws std::runtime_error if it is not one, or is missing anything the
   *         addressing needs.
   *
   * The info hash is taken over the bytes of the info dictionary exactly as
   * they were found, not over a re-encoding: the spec asks for the hash "of
   * the bencoded form of the info value from the metainfo file", and a
   * re-encoding is only probably the same bytes.
   */
  [[nodiscard]] static Metainfo parse(std::string_view torrentFile);

  [[nodiscard]] const InfoHash &hash() const { return infoHash; }
  /// The suggested name of the file or directory. Display only; two torrents
  /// may share a name and neither identifies anything by it.
  [[nodiscard]] const std::string &name() const { return torrentName; }
  [[nodiscard]] std::uint64_t pieceLength() const { return bytesPerPiece; }
  [[nodiscard]] std::size_t pieceCount() const { return pieceHashes.size(); }
  /// Total length of the concatenated stream.
  [[nodiscard]] std::uint64_t totalLength() const;
  [[nodiscard]] const std::vector<TorrentFile> &files() const {
    return entries;
  }

  /// The index of the file named @p path, or nothing.
  [[nodiscard]] std::optional<std::uint32_t>
  fileIndex(std::string_view path) const;

  /// The SHA-1 of piece @p index, as recorded in the torrent.
  [[nodiscard]] const std::array<std::uint8_t, 20> &
  pieceHash(std::size_t index) const;

  /// Length of piece @p index. The last one is short unless the content
  /// happens to divide exactly.
  [[nodiscard]] std::uint64_t lengthOfPiece(std::size_t index) const;

  /// The pieces covering [@p offset, @p offset + @p length) of the stream, as
  /// a half-open range of piece indices.
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  piecesForRange(std::uint64_t offset, std::uint64_t length) const;

  /**
   * @brief Whether @p piece hashes to what the torrent says it should.
   *
   * @p data must be the whole piece. A short read is not a partial answer, it
   * is a different piece, and reporting it as verified would defeat the point.
   */
  [[nodiscard]] bool verifyPiece(std::size_t index,
                                 std::string_view data) const;

  /// A magnet link naming this torrent, for handing the reference to
  /// something that can actually go and find it.
  [[nodiscard]] std::string magnet() const;

private:
  InfoHash infoHash;
  std::string torrentName;
  std::uint64_t bytesPerPiece{};
  std::vector<std::array<std::uint8_t, 20>> pieceHashes;
  std::vector<TorrentFile> entries;
};

/**
 * @brief A magnet link: a content reference written down portably.
 *
 * This is the form a Xanadu address travels in. It carries the info hash --
 * the whole of the identity -- and some hints about where to find the content
 * and what it is called.
 *
 * What it does not carry is the metadata: the piece hashes and the file list
 * live in the info dictionary, which a magnet link only names. So a magnet is
 * enough to *say which content is meant*, and not enough to fetch or verify a
 * byte of it. That is not a shortcoming of this implementation but of the
 * form: a client resolves a magnet by joining the swarm and asking a peer for
 * the info dictionary (BEP 9), then checking that what it is given hashes to
 * the name it started from.
 *
 * Xudu therefore accepts a magnet wherever a torrent is accepted, and can
 * resolve one exactly when the metadata it names has been obtained some other
 * way -- from a .torrent file today, or from a swarm-backed ContentSource.
 */
struct MagnetLink {
  InfoHash hash;
  /// The `dn` parameter. A display hint; it identifies nothing.
  std::string displayName;
  /// `tr` parameters, in order. Kept for whatever eventually joins a swarm.
  std::vector<std::string> trackers;
  /**
   * @brief The `so` parameter of BEP 53: which files of the torrent are meant.
   *
   * Expanded from the ranges the parameter is written in, so "0-2,4" arrives
   * as 0, 1, 2, 4. Empty when the link does not narrow to particular files,
   * which means all of them.
   */
  std::vector<std::uint32_t> selectedFiles;

  /**
   * @brief Read a magnet link.
   * @throws std::runtime_error if it is not one, or names no BitTorrent v1
   *         info hash. A link naming only a v2 hash is refused rather than
   *         half-understood: this addresses content by SHA-1 and would have no
   *         way to check content named by a SHA-256 merkle root.
   *
   * Accepts both spellings of the hash: forty hex digits, and the thirty-two
   * character base-32 form that older links use.
   */
  [[nodiscard]] static MagnetLink parse(std::string_view uri);

  /// Whether @p text is worth handing to parse(), as opposed to being a path.
  [[nodiscard]] static bool looksLikeMagnet(std::string_view text);
};

/**
 * @brief Build a single-file torrent carrying @p data.
 *
 * The other half of Metainfo, which until now could only read one. Sealing a
 * scroll means turning bytes this machine holds into bytes anybody can ask a
 * swarm for, and that begins with a torrent describing them.
 *
 * No announce list: this project finds peers through the DHT, and a tracker
 * URL would be a machine to depend on in a design whose point is not to have
 * one. A torrent without one is still a torrent -- BEP 5 and BEP 9 are how it
 * is found and fetched.
 *
 * @param name The suggested file name. Display only, as it always is.
 * @param pieceLength Bytes per piece. A power of two, at least 16 KiB.
 * @return The .torrent file's bytes, and the info hash naming it.
 */
struct MadeTorrent {
  std::string file;
  InfoHash hash;
};
[[nodiscard]] MadeTorrent makeTorrent(std::string_view data, std::string name,
                                      std::uint64_t pieceLength = 256 * 1024);

/// A file to put into a torrent. TorrentFile above is the other direction --
/// what a parsed torrent says it holds.
struct TorrentContent {
  /// Name within the torrent's directory. No separators: a flat directory is
  /// all this needs, and a path with one in it is how a torrent writes outside
  /// the directory it was given.
  std::string path;
  std::string data;
};

/**
 * @brief Build a torrent carrying several files under one directory.
 *
 * Pieces run over the concatenation of the files in the order given, which is
 * what makes a file's offset within the stream fixed the moment the torrent is
 * made -- and therefore what lets a scroll segment name a stretch of one file
 * and keep meaning it.
 *
 * What this is for: sealing a scroll seals more than the content. The record
 * of who wrote it goes in the same torrent, so that the info hash covers both
 * and neither can be substituted for the other afterwards. A reader who has
 * the bytes has the claim of authorship that came with them.
 *
 * @param name The directory name. Display only, as it always is.
 * @throws std::invalid_argument for a zero piece length, an empty file list,
 *         or a path containing a separator.
 */
[[nodiscard]] MadeTorrent makeTorrent(std::span<const TorrentContent> files,
                                      std::string name,
                                      std::uint64_t pieceLength = 256 * 1024);

/// SHA-1 of @p data, which is the hash BitTorrent v1 is built on.
[[nodiscard]] std::array<std::uint8_t, 20> sha1(std::string_view data);

/// Lowercase hex, which is how every BitTorrent specification writes binary
/// into a URI.
[[nodiscard]] std::string toHex(std::string_view bytes);

/// Decode hex.
/// @throws std::runtime_error on an odd number of digits or a non-digit. Both
///         are refused rather than salvaged: these spell out keys and hashes,
///         where a half-understood value would be a different value.
[[nodiscard]] std::string fromHex(std::string_view text);

/**
 * @brief Walk the parameters of a magnet URI.
 *
 * Hands @p visit each key with its percent-decoded value, in the order they
 * appear -- which matters, because a magnet may repeat a key and the
 * repetitions are not interchangeable.
 *
 * Shared because a magnet link and a BEP 46 mutable link are the same syntax
 * carrying different parameters, and two readings of that syntax would be two
 * things to keep in agreement.
 */
void forEachMagnetParameter(
    std::string_view uri,
    const std::function<void(std::string_view key, std::string value)> &visit);

} // namespace xudu

#endif // XUDU_TORRENT_H
// vi: set sw=2 sts=2 ts=2 et:
