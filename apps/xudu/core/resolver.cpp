#include "resolver.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace xudu {

namespace {

/// Read [offset, offset+length) of one file, or as much of it as exists.
std::string readFileRange(const std::filesystem::path &path,
                          const std::uint64_t offset,
                          const std::uint64_t length) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  in.seekg(static_cast<std::streamoff>(offset));
  if (!in) {
    return {};
  }
  std::string out(static_cast<std::size_t>(length), '\0');
  in.read(out.data(), static_cast<std::streamsize>(length));
  out.resize(static_cast<std::size_t>(in.gcount()));
  return out;
}

} // namespace

InfoHash DirectoryContentSource::add(const std::string_view torrentFile,
                                     std::string dataRoot) {
  auto meta       = Metainfo::parse(torrentFile);
  const auto hash = meta.hash();
  held.insert_or_assign(hash, Held{std::move(meta), std::move(dataRoot)});
  return hash;
}

const Metainfo *DirectoryContentSource::metainfo(const InfoHash &hash) const {
  const auto found = held.find(hash);
  return found == held.end() ? nullptr : &found->second.meta;
}

std::string DirectoryContentSource::readStream(const InfoHash &hash,
                                               const std::uint64_t offset,
                                               const std::uint64_t length) const {
  const auto found = held.find(hash);
  if (found == held.end()) {
    return {};
  }
  const auto &meta = found->second.meta;
  const std::filesystem::path root(found->second.root);

  // The stream is the files concatenated, so a read walks whichever files the
  // range lands in and stitches them together. A piece-sized read routinely
  // crosses a boundary, which is the case this exists to handle.
  std::string out;
  const auto last = std::min(offset + length, meta.totalLength());
  for (const auto &file : meta.files()) {
    const auto fileEnd = file.offset + file.length;
    if (fileEnd <= offset || file.offset >= last) {
      continue;
    }
    const auto from  = std::max(offset, file.offset) - file.offset;
    const auto count = std::min(fileEnd, last) - file.offset - from;
    auto piece       = readFileRange(root / file.path, from, count);
    if (piece.size() != count) {
      // Short: the file on disk is not the file the torrent describes. Saying
      // so by returning nothing is better than returning a gap, which would
      // then fail verification with a less specific complaint.
      return {};
    }
    out += piece;
  }
  return out;
}

bool Resolver::available(const Scroll &scroll) const {
  if (nullptr == source || scroll.segments.empty()) {
    return false;
  }
  return std::ranges::all_of(scroll.segments,
                             [this](const ScrollSegment &segment) {
                               return nullptr !=
                                      source->metainfo(segment.torrent);
                             });
}

std::string Resolver::readSegment(const ScrollSegment &segment,
                                  const std::uint64_t from,
                                  const std::uint64_t count) const {
  const auto *const meta = source->metainfo(segment.torrent);
  if (nullptr == meta) {
    return {};
  }
  // Scroll coordinates in, stream coordinates out. The segment is the whole of
  // what relates them, and it is the only thing here that a re-seal changes.
  const auto streamAt = segment.streamOffset + (from - segment.at);

  const auto [firstPiece, endPiece] = meta->piecesForRange(streamAt, count);
  if (endPiece <= firstPiece) {
    return {};
  }

  // Whole pieces, because a piece hash covers a piece and says nothing about
  // a fragment of one. Reading only the requested bytes would be cheaper and
  // would leave them unverifiable.
  const auto readFrom = static_cast<std::uint64_t>(firstPiece) * meta->pieceLength();
  std::string verified;
  verified.reserve(static_cast<std::size_t>((endPiece - firstPiece) *
                                            meta->pieceLength()));

  for (auto piece = firstPiece; piece < endPiece; piece++) {
    const auto at    = static_cast<std::uint64_t>(piece) * meta->pieceLength();
    const auto bytes = source->readStream(segment.torrent, at,
                                          meta->lengthOfPiece(piece));
    if (!meta->verifyPiece(piece, bytes)) {
      // Nothing is returned rather than the pieces that did check out.
      // Downstream cannot tell verified bytes from unverified ones, so a
      // partial answer is a substitution with extra steps.
      return {};
    }
    verified += bytes;
  }

  // Cut the requested range out of the pieces that were fetched to cover it.
  const auto into = static_cast<std::size_t>(streamAt - readFrom);
  if (into >= verified.size()) {
    return {};
  }
  return verified.substr(into, static_cast<std::size_t>(count));
}

std::string Resolver::read(const Scroll &scroll,
                           const PrimediaSpan &span) const {
  if (nullptr == source || span.empty()) {
    return {};
  }

  std::string out;
  auto at = span.start;
  while (at < span.end()) {
    const auto *const segment = scroll.segmentAt(at);
    if (nullptr == segment) {
      // A stretch nobody has sealed, or a scroll this store knows only part
      // of. Half a quotation is not a shorter quotation, it is a different
      // one, so this reports nothing at all.
      return {};
    }
    const auto count = std::min(span.end(), segment->end()) - at;
    auto bytes       = readSegment(*segment, at, count);
    if (bytes.size() != count) {
      return {};
    }
    out += bytes;
    at += count;
  }
  return out;
}

} // namespace xudu

// vi: set sw=2 sts=2 ts=2 et:
