#include "resolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace xanadu {

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

std::size_t
VerifiedPieceCache::KeyHash::operator()(const Key &key) const noexcept {
  // The info hash is already a hash, so its first eight bytes are as good a
  // bucket as anything this could compute, mixed with the piece index.
  std::uint64_t folded = 0;
  for (std::size_t i = 0; i < 8; i++) {
    folded = (folded << 8) | key.torrent.bytes[i];
  }
  return std::hash<std::uint64_t>{}(folded ^
                                    (key.piece * 0x9e3779b97f4a7c15ULL));
}

std::optional<std::string>
VerifiedPieceCache::get(const InfoHash &torrent,
                        const std::size_t piece) const {
  const std::lock_guard lock(guard);
  const auto found = index.find(Key{torrent, piece});
  if (found == index.end()) {
    counters.misses++;
    return std::nullopt;
  }
  counters.hits++;
  entries.splice(entries.begin(), entries, found->second);
  return found->second->second;
}

void VerifiedPieceCache::put(const InfoHash &torrent, const std::size_t piece,
                             const std::string &bytes) {
  const std::lock_guard lock(guard);
  const Key key{torrent, piece};
  if (const auto found = index.find(key); found != index.end()) {
    entries.splice(entries.begin(), entries, found->second);
    return;
  }
  // A piece larger than the whole budget would otherwise be stored and then
  // immediately evicted, paying the copy for nothing.
  if (bytes.size() > budgetBytes) {
    return;
  }
  entries.emplace_front(key, bytes);
  index.emplace(key, entries.begin());
  bytesHeld += bytes.size();
  evictDownToBudget();
}

void VerifiedPieceCache::evictDownToBudget() {
  while (bytesHeld > budgetBytes && !entries.empty()) {
    const auto &oldest = entries.back();
    bytesHeld -= oldest.second.size();
    index.erase(oldest.first);
    entries.pop_back();
    counters.evictions++;
  }
}

void VerifiedPieceCache::clear() {
  const std::lock_guard lock(guard);
  entries.clear();
  index.clear();
  bytesHeld = 0;
}

void VerifiedPieceCache::setBudgetBytes(const std::size_t bytes) {
  const std::lock_guard lock(guard);
  budgetBytes = bytes;
  evictDownToBudget();
}

VerifiedPieceCache::Stats VerifiedPieceCache::stats() const {
  const std::lock_guard lock(guard);
  auto out   = counters;
  out.pieces = index.size();
  out.bytes  = bytesHeld;
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
    // Already verified once, and the key names the bytes rather than a position
    // in some document: a piece hash is a commitment, so a cached hit is the
    // same bytes a fetch-and-verify would have produced. This is what stops
    // stageVisibleCells re-hashing 64 KiB per visible cell per frame.
    if (auto held = pieces->get(segment.torrent, piece); held) {
      verified += *held;
      continue;
    }
    const auto at = static_cast<std::uint64_t>(piece) * meta->pieceLength();
    const auto bytes =
        source->readStream(segment.torrent, at, meta->lengthOfPiece(piece));
    if (!meta->verifyPiece(piece, bytes)) {
      // Nothing is returned rather than the pieces that did check out.
      // Downstream cannot tell verified bytes from unverified ones, so a
      // partial answer is a substitution with extra steps.
      //
      // Nor is the failure cached. A piece that does not verify today is one
      // whose download has not finished or whose copy is damaged, and both get
      // repaired without anything here being told; remembering the refusal
      // would outlive the repair.
      return {};
    }
    pieces->put(segment.torrent, piece, bytes);
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
          const auto nonce = crypto::nonceForKeyId(tc.keyId);
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

  // Resolved *text* is still not cached here, and the cache.put(span, out) that
  // used to sit here is still gone: a PrimediaSpan names a scroll by its slot
  // index in *this* Store's externals table, so document A's scroll 1 and
  // document B's scroll 1 were two scrolls under one key in a process-wide LMDB
  // that outlived them both. That key cannot be repaired, only replaced.
  //
  // What replaced it is a layer down, in readSegment: verified pieces keyed by
  // (info hash, piece index), which does name the bytes. See
  // VerifiedPieceCache for the property that buys -- tampering with a local
  // copy stops being noticed for as long as a piece is held -- and for why the
  // cache is per-Resolver and in memory rather than persistent.
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

} // namespace xanadu
