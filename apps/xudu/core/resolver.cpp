#include "resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
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

  // A multi-file torrent's paths are relative to a directory named after the
  // torrent, and what somebody has on disk is as often the directory holding
  // that directory -- which is where a downloader puts it and where sealing
  // writes it. Both spellings are accepted by looking for the first file:
  // guessing wrong here produces a document that reads as empty, which is a
  // long way from the mistake that caused it.
  if (!meta.files().empty()) {
    const std::filesystem::path root(dataRoot);
    const auto &first = meta.files().front().path;
    // is_regular_file rather than exists: a torrent whose name matches its
    // first file's name -- which sealing produces, since the scroll and the
    // content it carries are both called after the salt -- otherwise finds the
    // directory and tries to read the file out of it.
    if (!std::filesystem::is_regular_file(root / first) &&
        std::filesystem::is_regular_file(root / meta.name() / first)) {
      dataRoot = (root / meta.name()).string();
    }
  }

  held.insert_or_assign(hash, Held{std::move(meta), std::move(dataRoot)});
  return hash;
}

const Metainfo *DirectoryContentSource::metainfo(const InfoHash &hash) const {
  const auto found = held.find(hash);
  return found == held.end() ? nullptr : &found->second.meta;
}

std::string
DirectoryContentSource::readStream(const InfoHash &hash,
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
  return std::ranges::all_of(
      scroll.segments, [this](const ScrollSegment &segment) {
        return nullptr != source->metainfo(segment.torrent);
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
  const auto readFrom =
      static_cast<std::uint64_t>(firstPiece) * meta->pieceLength();
  std::string verified;
  verified.reserve(
      static_cast<std::size_t>((endPiece - firstPiece) * meta->pieceLength()));

  for (auto piece = firstPiece; piece < endPiece; piece++) {
    const auto at = static_cast<std::uint64_t>(piece) * meta->pieceLength();
    const auto bytes =
        source->readStream(segment.torrent, at, meta->lengthOfPiece(piece));
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

ResolveResult Resolver::resolve(const Scroll &scroll,
                                const PrimediaSpan &span) const {
  if (span.empty()) {
    return ResolveResult{.status = ResolutionStatus::MissingPieces};
  }

  std::string out;
  auto at = span.start;
  while (at < span.end()) {
    const auto *const segment = scroll.segmentAt(at);
    if (nullptr == segment) {
      return ResolveResult{.status = ResolutionStatus::MissingPieces};
    }
    const auto count = std::min(span.end(), segment->end()) - at;

    if (segment->isWithheld()) {
      if (segment->holeRecord &&
          segment->holeRecord->reason == HoleReason::TranscopyrightLock &&
          segment->holeRecord->transcopyright) {
        const auto &tc = *segment->holeRecord->transcopyright;
        CekRecord cekRec;
        if (source != nullptr && cache.get_cek(tc.keyId, cekRec)) {
          // The key is held, so this span has been paid for. What is left is
          // to fetch the ciphertext and open it.
          const auto *const meta = source->metainfo(segment->torrent);
          if (nullptr == meta) {
            return ResolveResult{.status = ResolutionStatus::MissingPieces};
          }
          // Unsigned, so a segment claiming to start past the end of the
          // stream would otherwise wrap into an enormous length.
          if (segment->streamOffset > meta->totalLength()) {
            return ResolveResult{.status = ResolutionStatus::UnverifiedHash};
          }
          const auto cipherLen =
              std::min(meta->totalLength() - segment->streamOffset,
                       segment->length + crypto::kTagSize);
          // Through readSegment, so the ciphertext is piece-verified before
          // it is decrypted -- the same discipline the plain path below uses.
          // Reading it straight off the source meant a hostile seeder's bytes
          // reached the decryptor, and Poly1305 rejecting them was reported as
          // "you have not paid", which is a different thing and the one thing
          // the reader most needs to be able to tell apart.
          auto cipherBytes = readSegment(*segment, segment->at, cipherLen);
          if (cipherBytes.size() != cipherLen ||
              cipherBytes.size() < crypto::kTagSize) {
            return ResolveResult{.status = ResolutionStatus::UnverifiedHash};
          }
          crypto::Nonce24 nonce{};
          std::memcpy(nonce.data(), tc.keyId.data(),
                      std::min<std::size_t>(tc.keyId.size(), nonce.size()));
          try {
            auto plain = crypto::decryptSpanSlice(
                cipherBytes, 0, cekRec.cek, nonce, at - segment->at, count);
            if (!plain) {
              // Verified bytes that the held key does not open: the key is
              // wrong or the author re-sealed, not an unpaid span.
              return ResolveResult{.status   = ResolutionStatus::UnverifiedHash,
                                   .lockInfo = tc,
                                   .holeRecord = segment->holeRecord};
            }
            out.append(*plain);
            at += count;
            continue;
          } catch (...) {
            return ResolveResult{.status     = ResolutionStatus::UnverifiedHash,
                                 .lockInfo   = tc,
                                 .holeRecord = segment->holeRecord};
          }
        }
        return ResolveResult{.status   = ResolutionStatus::TranscopyrightLocked,
                             .lockInfo = tc,
                             .holeRecord = segment->holeRecord};
      }

      return ResolveResult{.status     = ResolutionStatus::WithheldRedacted,
                           .holeRecord = segment->holeRecord};
    }

    if (nullptr == source) {
      return ResolveResult{.status = ResolutionStatus::MissingPieces};
    }

    auto bytes = readSegment(*segment, at, count);
    if (bytes.size() != count) {
      return ResolveResult{.status = ResolutionStatus::MissingPieces};
    }
    out += bytes;
    at += count;
  }

  cache.put(span, out);
  return ResolveResult{.status = ResolutionStatus::VerifiedBytes,
                       .text   = std::move(out)};
}

std::string Resolver::read(const Scroll &scroll,
                           const PrimediaSpan &span) const {
  auto res = resolve(scroll, span);
  if (res.status == ResolutionStatus::VerifiedBytes) {
    return res.text;
  }
  return {};
}

bool Resolver::unlockTranscopyright(
    const std::array<std::uint8_t, 32> &keyId, const crypto::Key32 &cek,
    const std::uint64_t pricePaid,
    const std::string_view currencySymbol) const {
  CekRecord rec;
  rec.cek               = cek;
  rec.unlockedTimestamp = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  rec.pricePaid = pricePaid;
  rec.currency.fill('\0');
  std::memcpy(
      rec.currency.data(), currencySymbol.data(),
      std::min<std::size_t>(currencySymbol.size(), rec.currency.size()));
  return cache.put_cek(keyId, rec);
}

} // namespace xudu
