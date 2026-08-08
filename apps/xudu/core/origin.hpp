/**
 * @file origin.hpp
 * @brief Where content that was not typed here comes from.
 *
 * Xanadu asks for content to have "canonical network addresses" -- names that
 * identify content itself, everywhere, permanently. The local spool cannot
 * provide one: an offset into a file on one machine is meaningless elsewhere,
 * and meaningless there too once the machine is gone. A reference that stops
 * resolving is exactly the rot Xanadu was meant to avoid, and it is what the
 * ordinary web does.
 *
 * A torrent's info hash is a name of the kind asked for. It is derived from
 * the content, so nobody assigns it and nobody can reassign it; the same
 * content always produces it; resolving it does not require one particular
 * server to still be answering; and every piece carries a hash, so what
 * arrives can be checked against what was named.
 *
 * An Origin is that reference, narrowed to one file: which torrent, which file
 * in it, and where that file sits in the torrent's concatenated stream. A span
 * into it is an offset within the file, which is what a person means by "these
 * bytes of this document".
 */
#ifndef XUDU_ORIGIN_H
#define XUDU_ORIGIN_H

#include <cstdint>
#include <string>

#include "spool.hpp"
#include "torrent.hpp"

namespace xudu {

/// One body of externally addressed content.
struct Origin {
  /// The torrent this content belongs to. Its info hash is the whole of the
  /// identity; the rest of this struct is how to find one file inside it.
  InfoHash torrent;
  /// Which file, as an index into the torrent's file list.
  std::uint32_t fileIndex{};
  /// That file's path within the torrent, for display and for locating it on
  /// disk. Not the identity: two torrents may hold files of the same name.
  std::string path;
  /// Where the file begins in the torrent's concatenated stream, and how long
  /// it is. Kept here so an origin can be understood without re-reading the
  /// torrent, though verifying content still needs the piece hashes.
  std::uint64_t fileOffset{};
  std::uint64_t fileLength{};

  /// Whether two origins name the same content. The path is display and is
  /// deliberately not compared.
  [[nodiscard]] bool sameContentAs(const Origin &other) const {
    return torrent == other.torrent && fileIndex == other.fileIndex;
  }

  /// How this reference is written down: the torrent, then the file.
  [[nodiscard]] std::string describe() const {
    return torrent.hex() + "/" + std::to_string(fileIndex) +
           (path.empty() ? "" : " (" + path + ")");
  }
};

} // namespace xudu

#endif // XUDU_ORIGIN_H
// vi: set sw=2 sts=2 ts=2 et:
